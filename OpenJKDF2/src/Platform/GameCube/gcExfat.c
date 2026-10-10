// Minimal read-only exFAT reader (see gcExfat.h).
//
// Volume layout (boot sector, all little-endian):
//   0x03 "EXFAT   ", 0x50 FAT offset, 0x58 cluster heap offset (sectors),
//   0x5C cluster count, 0x60 root directory cluster,
//   0x6C log2(bytes per sector), 0x6D log2(sectors per cluster).
// Directory entries are 32 bytes, grouped in sets: 0x85 File (attributes, number
// of secondaries), 0xC0 Stream Extension (flags, name length, first cluster, data
// length), then 0xC1 File Name entries holding 15 UTF-16 characters each. Files
// flagged NoFatChain are contiguous; others follow the 32-bit FAT.
//
// The image's cluster chain is flattened into extents once, so reads map straight
// to device sectors.

#include "gcExfat.h"

#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GCEXFAT_SECTOR      512
#define GCEXFAT_BOUNCE      (32 * 1024)
#define GCEXFAT_MAX_DIRECT  256          // sectors per direct readSectors call

typedef struct gcExfatChain {
    uint32_t firstCluster;
    uint64_t length;       // bytes, 0 = unknown (root directory: follow the FAT)
    int      bNoFatChain;
} gcExfatChain;

typedef struct gcExfatEntry {
    char         name[256];   // non-ASCII characters become '?'
    int          bDir;
    gcExfatChain chain;
} gcExfatEntry;

typedef struct gcExfatExtent {
    uint64_t fileOffset;
    uint64_t sector;          // device sector
    uint64_t len;             // bytes
} gcExfatExtent;

typedef struct gcExfatScan {
    const char*  id;
    int          depth;
    int          bRoot;
    int          bFound;
    gcExfatEntry hit;
    char         path[256];
} gcExfatScan;

typedef int (*gcExfatDirFn)(const gcExfatEntry* pEntry, void* pCtx);

static const DISC_INTERFACE* gcExfat_pIface = NULL;
static uint32_t gcExfat_secShift;        // log2(device sectors per exFAT sector)
static uint32_t gcExfat_clusterShift;    // log2(device sectors per cluster)
static uint64_t gcExfat_fatStart;        // device sectors
static uint64_t gcExfat_heapStart;       // device sectors
static uint32_t gcExfat_rootCluster;
static uint32_t gcExfat_clusterCount;

static uint8_t* gcExfat_pSector = NULL;
static uint8_t* gcExfat_pFat = NULL;
static uint64_t gcExfat_fatSector = ~0ull;
static uint8_t* gcExfat_pBounce = NULL;

static gcExfatExtent* gcExfat_aExtents = NULL;
static int      gcExfat_numExtents = 0;
static int      gcExfat_lastExtent = 0;
static uint64_t gcExfat_fileSize = 0;

static uint16_t gcExfat_Rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t gcExfat_Rd32(const uint8_t* p) { return gcExfat_Rd16(p) | ((uint32_t)gcExfat_Rd16(p + 2) << 16); }
static uint64_t gcExfat_Rd64(const uint8_t* p) { return gcExfat_Rd32(p) | ((uint64_t)gcExfat_Rd32(p + 4) << 32); }

static int gcExfat_ReadSectors(uint64_t sector, uint32_t count, void* pDst)
{
    return gcExfat_pIface->readSectors((sec_t)sector, count, pDst);
}

static int gcExfat_ValidCluster(uint32_t cluster)
{
    return cluster >= 2 && cluster < gcExfat_clusterCount + 2;
}

static uint64_t gcExfat_ClusterSector(uint32_t cluster)
{
    return gcExfat_heapStart + ((uint64_t)(cluster - 2) << gcExfat_clusterShift);
}

static uint32_t gcExfat_NextCluster(uint32_t cluster)
{
    uint64_t byteOff = (uint64_t)cluster * 4;
    uint64_t sector = gcExfat_fatStart + byteOff / GCEXFAT_SECTOR;
    if (sector != gcExfat_fatSector) {
        if (!gcExfat_ReadSectors(sector, 1, gcExfat_pFat))
            return 0xFFFFFFFF;
        gcExfat_fatSector = sector;
    }
    return gcExfat_Rd32(gcExfat_pFat + byteOff % GCEXFAT_SECTOR);
}

// --- Mounting ------------------------------------------------------------------------

static int gcExfat_TryBootSector(uint64_t lba)
{
    if (!gcExfat_ReadSectors(lba, 1, gcExfat_pSector)) return 0;
    const uint8_t* b = gcExfat_pSector;
    if (memcmp(b + 3, "EXFAT   ", 8)) return 0;

    uint32_t bytesShift = b[0x6C];
    if (bytesShift < 9 || bytesShift > 12) return 0;
    gcExfat_secShift = bytesShift - 9;
    gcExfat_clusterShift = b[0x6D] + gcExfat_secShift;
    gcExfat_fatStart = lba + ((uint64_t)gcExfat_Rd32(b + 0x50) << gcExfat_secShift);
    gcExfat_heapStart = lba + ((uint64_t)gcExfat_Rd32(b + 0x58) << gcExfat_secShift);
    gcExfat_clusterCount = gcExfat_Rd32(b + 0x5C);
    gcExfat_rootCluster = gcExfat_Rd32(b + 0x60);
    gcExfat_fatSector = ~0ull;
    return 1;
}

static int gcExfat_Mount(void)
{
    if (gcExfat_TryBootSector(0)) return 1;

    if (!gcExfat_ReadSectors(0, 1, gcExfat_pSector)) return 0;
    if (gcExfat_pSector[510] != 0x55 || gcExfat_pSector[511] != 0xAA) return 0;
    uint8_t mbr[64];
    memcpy(mbr, gcExfat_pSector + 0x1BE, sizeof(mbr));

    for (int i = 0; i < 4; i++)
    {
        uint8_t type = mbr[i * 16 + 4];
        uint32_t start = gcExfat_Rd32(mbr + i * 16 + 8);
        if (type == 0xEE) {
            // Protective MBR: GPT header at LBA 1.
            if (!gcExfat_ReadSectors(1, 1, gcExfat_pSector) || memcmp(gcExfat_pSector, "EFI PART", 8))
                continue;
            uint64_t entriesLba = gcExfat_Rd64(gcExfat_pSector + 0x48);
            uint32_t numEntries = gcExfat_Rd32(gcExfat_pSector + 0x50);
            uint32_t entrySize = gcExfat_Rd32(gcExfat_pSector + 0x54);
            if (entrySize < 0x28 || entrySize > GCEXFAT_SECTOR) continue;
            for (uint32_t k = 0; k < numEntries && k < 128; k++) {
                uint64_t off = (uint64_t)k * entrySize;
                if (!gcExfat_ReadSectors(entriesLba + off / GCEXFAT_SECTOR, 1, gcExfat_pSector)) break;
                uint64_t first = gcExfat_Rd64(gcExfat_pSector + off % GCEXFAT_SECTOR + 0x20);
                if (first && gcExfat_TryBootSector(first)) return 1;
            }
        }
        else if (type && start && gcExfat_TryBootSector(start)) {
            return 1;
        }
    }
    return 0;
}

// --- Directories ---------------------------------------------------------------------

// Calls fn for each file/directory in `dir`; stops early when it returns nonzero.
static int gcExfat_IterateDir(const gcExfatChain* dir, gcExfatDirFn fn, void* pCtx)
{
    // Own buffer: callbacks read sectors and recurse into subdirectories.
    static uint8_t aBufs[4][GCEXFAT_SECTOR] __attribute__((aligned(32)));
    static int level = 0;
    if (level >= 4) return 0;
    uint8_t* buf = aBufs[level];

    uint32_t sectorsPerCluster = 1u << gcExfat_clusterShift;
    uint64_t remaining = dir->length ? dir->length : ~0ull;
    uint32_t cluster = dir->firstCluster;
    gcExfatEntry cur;
    int secondaryLeft = 0, nameLen = 0, namePos = 0;
    int ret = 0;

    level++;
    for (uint32_t guard = 0; gcExfat_ValidCluster(cluster) && remaining && guard < 65536; guard++)
    {
        for (uint32_t s = 0; s < sectorsPerCluster && remaining; s++)
        {
            if (!gcExfat_ReadSectors(gcExfat_ClusterSector(cluster) + s, 1, buf)) goto done;
            for (int i = 0; i < GCEXFAT_SECTOR; i += 32)
            {
                const uint8_t* e = buf + i;
                uint8_t type = e[0];
                if (type == 0x00) goto done;          // end of directory
                if (type == 0x85) {
                    memset(&cur, 0, sizeof(cur));
                    cur.bDir = (gcExfat_Rd16(e + 4) & 0x10) != 0;
                    secondaryLeft = e[1];
                    nameLen = namePos = 0;
                    continue;
                }
                if (!secondaryLeft) continue;
                if (!(type & 0x80)) {                 // deleted entry inside a set
                    secondaryLeft = 0;
                    continue;
                }
                if (type == 0xC0) {
                    cur.chain.bNoFatChain = (e[1] & 0x02) != 0;
                    nameLen = e[3];
                    cur.chain.firstCluster = gcExfat_Rd32(e + 20);
                    cur.chain.length = gcExfat_Rd64(e + 24);
                }
                else if (type == 0xC1) {
                    for (int k = 0; k < 15 && namePos < nameLen && namePos < 255; k++) {
                        uint16_t ch = gcExfat_Rd16(e + 2 + 2 * k);
                        cur.name[namePos++] = ch < 0x80 ? (char)ch : '?';
                    }
                }
                if (--secondaryLeft == 0) {
                    cur.name[namePos] = 0;
                    if (fn(&cur, pCtx)) { ret = 1; goto done; }
                }
            }
            remaining = remaining > GCEXFAT_SECTOR ? remaining - GCEXFAT_SECTOR : 0;
        }
        cluster = dir->bNoFatChain ? cluster + 1 : gcExfat_NextCluster(cluster);
    }
done:
    level--;
    return ret;
}

static int gcExfat_IsImage(const gcExfatEntry* e, const char* id)
{
    if (!gcExfat_ValidCluster(e->chain.firstCluster) || e->chain.length < 6) return 0;
    if (!gcExfat_ReadSectors(gcExfat_ClusterSector(e->chain.firstCluster), 1, gcExfat_pSector)) return 0;
    return !memcmp(gcExfat_pSector, id, 6);
}

static int gcExfat_ScanFn(const gcExfatEntry* e, void* pCtx)
{
    gcExfatScan* scan = (gcExfatScan*)pCtx;
    if (e->bDir) {
        int bGames = scan->bRoot && !strcasecmp(e->name, "games");
        if (!bGames && scan->depth <= 0) return 0;
        gcExfatScan sub = *scan;
        sub.bRoot = 0;
        sub.depth = bGames ? 2 : scan->depth - 1;
        snprintf(sub.path, sizeof(sub.path), "%s/%s", scan->path, e->name);
        gcExfat_IterateDir(&e->chain, gcExfat_ScanFn, &sub);
        if (sub.bFound) {
            *scan = sub;
            return 1;
        }
        return 0;
    }
    const char* ext = strrchr(e->name, '.');
    if (ext && (!strcasecmp(ext, ".iso") || !strcasecmp(ext, ".gcm")) && gcExfat_IsImage(e, scan->id)) {
        scan->hit = *e;
        scan->bFound = 1;
        size_t n = strlen(scan->path);
        snprintf(scan->path + n, sizeof(scan->path) - n, "/%s", e->name);
        return 1;
    }
    return 0;
}

// --- The image -----------------------------------------------------------------------

static int gcExfat_BuildExtents(const gcExfatChain* chain)
{
    free(gcExfat_aExtents);
    gcExfat_aExtents = NULL;
    gcExfat_numExtents = gcExfat_lastExtent = 0;
    gcExfat_fileSize = chain->length;

    int capacity = 0;
    uint64_t clusterBytes = (uint64_t)GCEXFAT_SECTOR << gcExfat_clusterShift;
    uint32_t cluster = chain->firstCluster;
    for (uint64_t off = 0; off < gcExfat_fileSize; )
    {
        if (!gcExfat_ValidCluster(cluster)) return 0;
        uint64_t sector = gcExfat_ClusterSector(cluster);
        uint64_t len = gcExfat_fileSize - off < clusterBytes ? gcExfat_fileSize - off : clusterBytes;

        gcExfatExtent* last = gcExfat_numExtents ? &gcExfat_aExtents[gcExfat_numExtents - 1] : NULL;
        if (last && last->sector + last->len / GCEXFAT_SECTOR == sector) {
            last->len += len;
        }
        else {
            if (gcExfat_numExtents == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                gcExfatExtent* p = (gcExfatExtent*)realloc(gcExfat_aExtents, capacity * sizeof(gcExfatExtent));
                if (!p) return 0;
                gcExfat_aExtents = p;
            }
            gcExfat_aExtents[gcExfat_numExtents++] = (gcExfatExtent){ off, sector, len };
        }
        off += len;
        cluster = chain->bNoFatChain ? cluster + 1 : gcExfat_NextCluster(cluster);
    }
    return gcExfat_numExtents > 0;
}

int gcExfat_FindImage(const DISC_INTERFACE* iface, const char* id6, char* pPathOut, int pathLen)
{
    if (!gcExfat_pSector) {
        gcExfat_pSector = (uint8_t*)memalign(32, GCEXFAT_SECTOR);
        gcExfat_pFat = (uint8_t*)memalign(32, GCEXFAT_SECTOR);
        gcExfat_pBounce = (uint8_t*)memalign(32, GCEXFAT_BOUNCE);
        if (!gcExfat_pSector || !gcExfat_pFat || !gcExfat_pBounce) return 0;
    }

    gcExfat_pIface = iface;
    if (!iface->startup() || !iface->isInserted()) return 0;
    if (!gcExfat_Mount()) return 0;
    printf("gcExfat: exFAT volume, %u clusters of %u KiB\n",
           gcExfat_clusterCount, (GCEXFAT_SECTOR << gcExfat_clusterShift) / 1024);

    gcExfatScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.id = id6;
    scan.bRoot = 1;
    gcExfatChain root = { gcExfat_rootCluster, 0, 0 };
    gcExfat_IterateDir(&root, gcExfat_ScanFn, &scan);
    if (!scan.bFound) return 0;

    if (!gcExfat_BuildExtents(&scan.hit.chain)) {
        printf("gcExfat: could not follow the cluster chain of %s\n", scan.path);
        return 0;
    }
    snprintf(pPathOut, pathLen, "sd:%s", scan.path);
    printf("gcExfat: %s, %u fragment(s)\n", pPathOut, gcExfat_numExtents);
    return 1;
}

static int gcExfat_FindExtent(uint64_t offset)
{
    const gcExfatExtent* x = &gcExfat_aExtents[gcExfat_lastExtent];
    if (offset >= x->fileOffset && offset < x->fileOffset + x->len)
        return gcExfat_lastExtent;
    int lo = 0, hi = gcExfat_numExtents - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (gcExfat_aExtents[mid].fileOffset <= offset) lo = mid;
        else hi = mid - 1;
    }
    return gcExfat_lastExtent = lo;
}

int gcExfat_Read(void* pDst, uint32_t len, uint64_t offset)
{
    uint8_t* dst = (uint8_t*)pDst;
    while (len)
    {
        if (offset >= gcExfat_fileSize) {
            memset(dst, 0, len);
            break;
        }
        const gcExfatExtent* x = &gcExfat_aExtents[gcExfat_FindExtent(offset)];
        uint64_t rel = offset - x->fileOffset;
        uint64_t sector = x->sector + rel / GCEXFAT_SECTOR;
        uint32_t inSector = rel % GCEXFAT_SECTOR;
        uint64_t avail = x->len - rel;
        uint32_t chunk = len < avail ? len : (uint32_t)avail;

        if (inSector == 0 && chunk >= GCEXFAT_SECTOR && !((uintptr_t)dst & 31)) {
            uint32_t n = chunk / GCEXFAT_SECTOR;
            if (n > GCEXFAT_MAX_DIRECT) n = GCEXFAT_MAX_DIRECT;
            if (!gcExfat_ReadSectors(sector, n, dst)) return 0;
            chunk = n * GCEXFAT_SECTOR;
        }
        else {
            uint32_t n = (inSector + chunk + GCEXFAT_SECTOR - 1) / GCEXFAT_SECTOR;
            if (n > GCEXFAT_BOUNCE / GCEXFAT_SECTOR) n = GCEXFAT_BOUNCE / GCEXFAT_SECTOR;
            if (!gcExfat_ReadSectors(sector, n, gcExfat_pBounce)) return 0;
            uint32_t got = n * GCEXFAT_SECTOR - inSector;
            if (chunk > got) chunk = got;
            memcpy(dst, gcExfat_pBounce + inSector, chunk);
        }
        dst += chunk;
        offset += chunk;
        len -= chunk;
    }
    return 1;
}
