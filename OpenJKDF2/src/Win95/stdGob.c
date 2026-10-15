#include "General/stdEndian.h"
#include "stdGob.h"
#ifdef TARGET_GAMECUBE
#include "gcDvd.h"
#endif

#include <stdio.h>

#include "jk.h"
#include "stdPlatform.h"

#include "General/stdHashtbl.h"
#include "General/stdString.h"
#include "Platform/Common/stdEmbeddedRes.h"

static HostServices gobHS;
static HostServices* pGobHS;
static int stdGob_bInit;
static char stdGob_fpath[128];

int stdGob_Startup(HostServices *pHS)
{
    _memcpy(&gobHS, pHS, sizeof(gobHS));
    pGobHS = &gobHS;
    stdGob_bInit = 1;
    return 1;
}

void stdGob_Shutdown()
{
    stdGob_bInit = 0;
}

Gob* stdGob_Load(char *pFilename, int numFileHandles, int bMMapFile)
{
    Gob* gob = (Gob*)STD_ALLOC(sizeof(Gob));
    if (gob)
    {
        _memset(gob, 0, sizeof(Gob)); // TODO why was this needed
        stdGob_LoadEntry(gob, pFilename, numFileHandles, bMMapFile); // TODO verify this? it does weird stuff
        return gob;
    }
    return NULL;
}

int stdGob_LoadEntry(Gob *pGob, char *pFilename, int numFileHandles, int bMMapFile)
{
    int v8; // edx
    GobFileHandle *v9; // eax
    stdGobHeader header; // [esp+10h] [ebp-Ch]

    stdString_SafeStrCopy(pGob->fpath, pFilename, 128);
    pGob->numHandles = numFileHandles;
    pGob->pCurHandle = 0;

    //TODO fix this? WINE/df2_reimpl.dll keeps corrupting the gobs? Might be something else idk.
#if 0
    if ( bMMapFile )
    {
        HANDLE v6 = jk_CreateFileA(pGob->fpath, 0x80000000, 1u, 0, 3u, 0x10000000u, 0);
        pGob->hFile = v6;
        HANDLE v7 = jk_CreateFileMappingA(v6, 0, 2u, 0, 0, 0);
        pGob->hMapFile = v7;
        if ( v7 )
        {
            v8 = pGob->numHandles;
            pGob->bFileMap = 1;
            v9 = (GobFileHandle *)jk_LocalAlloc(0x40u, 16 * v8);
            pGob->aHandles = v9;
            if ( v9 )
            {
                pGob->pBase = jk_MapViewOfFile(pGob->hMapFile, 4u, 0, 0, 0);
                return 1;
            }
            else
            {
                jk_UnmapViewOfFile(pGob->pBase);
                jk_CloseHandle(pGob->hMapFile);
            }
        }
        else
        {
            jk_CloseHandle(pGob->hFile);
        }
    }
#endif

    pGob->bFileMap = 0;
    pGob->hGobFile = pGobHS->fileOpen(pGob->fpath, "rb"); // Added: r+b -> rb, we don't actually need to write GOBs
    if ( !pGob->hGobFile ) {
        stdPlatform_Printf("OpenJKDF2: Gob failed to open `%s`.\n", pGob->fpath); // Added
        return 0;
    }
    else {
        stdPlatform_Printf("OpenJKDF2: Gob opened `%s`.\n", pGob->fpath); // Added
    }
    pGob->aHandles = (GobFileHandle *)STD_ALLOC(sizeof(GobFileHandle) * pGob->numHandles);
    if ( !pGob->aHandles )
      return 0;
    _memset(pGob->aHandles, 0, sizeof(GobFileHandle) * pGob->numHandles);
    pGobHS->fileRead(pGob->hGobFile, &header, sizeof(stdGobHeader));
    STD_LE32(header.version); // Added: big-endian hosts
    STD_LE32(header.entryTable_offs);
    if ( _memcmp((const char *)&header, "GOB ", 4u) )
    {
      stdPrintf(std_g_pHS->errorPrint, ".\\Win95\\stdGob.c", 270, "Error: Bad signature in header of gob file.\n", 0, 0, 0, 0);
      return 0;
    }
    if ( header.version != 20 )
    {
      stdPrintf(std_g_pHS->errorPrint, ".\\Win95\\stdGob.c", 277, "Error: Bad version %d for gob file\n", header.version, 0, 0, 0);
      return 0;
    }
    pGobHS->fseek(pGob->hGobFile, header.entryTable_offs, 0);
    pGobHS->fileRead(pGob->hGobFile, &pGob->numFiles, sizeof(uint32_t));
    STD_LE32(pGob->numFiles); // Added: big-endian hosts
    pGob->entries = (stdGobEntry *)STD_ALLOC(sizeof(stdGobEntry) * pGob->numFiles);
    if ( !pGob->entries )
      return 0;
    
    // Added
    _memset(pGob->entries, 0, sizeof(stdGobEntry) * pGob->numFiles);

    // We're not adding anything so like, keep it small?
#ifdef TARGET_RETRO_HOMEBREW
    pGob->pDirHash = stdHashtbl_New(pGob->numFiles);
#else
    pGob->pDirHash = stdHashtbl_New(1024);
#endif
    for (int v4 = 0; v4 < pGob->numFiles; v4++)
    {
#ifdef STDGOB_COMPACT_ENTRIES
        // Added: stage the fixed 136-byte disk entry; only offset/size stay
        // resident (the CRC-keyed pHashtbl doesn't retain the name pointer).
        stdGobDiskEntry diskEntry;
        pGobHS->fileRead(pGob->hGobFile, &diskEntry, sizeof(stdGobDiskEntry));
        pGob->entries[v4].fileOffset = stdEndian_LE32(diskEntry.fileOffset);
        pGob->entries[v4].fileSize = (int32_t)stdEndian_LE32(diskEntry.fileSize);
        stdHashtbl_Add(pGob->pDirHash, diskEntry.fname, &pGob->entries[v4]);
#else
        pGobHS->fileRead(pGob->hGobFile, &pGob->entries[v4], sizeof(stdGobEntry));
        STD_LE32(pGob->entries[v4].fileOffset); // Added: big-endian hosts
        STD_LE32(pGob->entries[v4].fileSize);
        stdHashtbl_Add(pGob->pDirHash, pGob->entries[v4].fname, &pGob->entries[v4]);
#endif
    }

    stdPlatform_Printf("OpenJKDF2: Gob loaded GOB file `%s`...\n", pFilename);
    
    return 1;
}

void stdGob_Free(Gob *pGob)
{
    if (!pGob )
        return;

    stdGob_FreeEntry(pGob);
    STD_FREE(pGob);
}

void stdGob_FreeEntry(Gob *pGob)
{
    if ( pGob->bFileMap )
    {
        jk_UnmapViewOfFile(pGob->pBase);
        jk_CloseHandle(pGob->hMapFile);
        jk_CloseHandle(pGob->hFile);
    }
    else
    {
        // Added: Fix file handle leak
        if (pGob->hGobFile) {
            pGobHS->fileClose(pGob->hGobFile);
            pGob->hGobFile = 0;
        }
        // Added: Fix memleak
        if (pGob->aHandles) {
            STD_FREE(pGob->aHandles);
            pGob->aHandles = NULL;
        }
        if ( pGob->entries )
        {
            STD_FREE(pGob->entries);
            pGob->entries = 0;
        }
        if ( pGob->pDirHash )
        {
            stdHashtbl_Free(pGob->pDirHash);
            pGob->pDirHash = 0;
        }
    }
}

GobFileHandle* stdGob_FileOpen(Gob *pGob, const char *aName)
{
    stdGobEntry *entry = NULL;
    GobFileHandle *result = NULL;
    int v5;
#ifdef TARGET_GAMECUBE
    stdString_SafeStrCopy(gcDvd_szLastOpen, aName, sizeof(gcDvd_szLastOpen)); // Added: diagnostics
#endif

    // Added: J3D asserts (both are unconditionally dereferenced below)
    STD_ASSERTREL(pGob);
    STD_ASSERTREL(aName);

    // Embedded resources
#if defined(QOL_IMPROVEMENTS)
    size_t sz = 0;
    void* data = stdEmbeddedRes_LoadOnlyInternal(aName, &sz);
    if (data) {
        result = pGob->aHandles;
        v5 = 0;
        if ( !pGob->numHandles )
            return 0;

        while ( result->bUsed )
        {
            ++result;
            if ( ++v5 >= pGob->numHandles )
                return 0;
        }
        result->bIsMemoryMapped = 1;
        result->pMemory = (intptr_t)data;
        result->memorySz = sz;

        result->bUsed = 1;
        result->parent = pGob;
        result->entry = entry;
        result->offset = 0;
        // Added: Opening another file in this GOB makes the shared handle's position
        // ambiguous: invalidate the seek-skip cache so the next read re-seeks.
        pGob->pCurHandle = 0;
        return result;
    }
#endif

    // Added: clean up paths
    if (aName[0] == '.' && (aName[1] == '/' || aName[1] == '\\')) {
        aName += 2;
    }
    stdString_SafeStrCopy(stdGob_fpath, aName, 128);
    stdString_CStrToLower(stdGob_fpath);

#ifdef PLATFORM_POSIX
    for (int i = 0; i < 128; i++)
    {
        if (stdGob_fpath[i] == '/')
            stdGob_fpath[i] = '\\';
    }
#endif
    entry = (stdGobEntry*)stdHashtbl_Find(pGob->pDirHash, stdGob_fpath);
    if (!entry)
        return 0;

    result = pGob->aHandles;
    v5 = 0;
    if ( !pGob->numHandles )
        return 0;

    while ( result->bUsed )
    {
        ++result;
        if ( ++v5 >= pGob->numHandles )
            return 0;
    }
#ifdef QOL_IMPROVEMENTS
    result->bIsMemoryMapped = 0;
    result->pMemory = (intptr_t)NULL;
    result->memorySz = 0;
#endif
#ifdef GC_LOG_GOB_OPENS
    // Added: dev builds record the access order (packaging/gamecube reorders the GOBs
    // so each level's files are contiguous on the disc).
    printf("GOBOPEN %s %s\n", pGob->fpath, stdGob_fpath);
#endif
#if defined(QOL_IMPROVEMENTS) && defined(STDGOB_PRELOAD_MAX)
    // Added: on optical media every seek costs ~100ms, and the engine's access
    // pattern (small reads, seeking back and forth, several GOBs interleaved)
    // turns level loads into thousands of seeks. Slurp small sub-files in one
    // sequential read and serve them from memory (freed on close).
    if (entry->fileSize > 0 && entry->fileSize <= STDGOB_PRELOAD_MAX)
    {
        void* pData = malloc(entry->fileSize);
        if (pData)
        {
            pGobHS->fseek(pGob->hGobFile, entry->fileOffset, SEEK_SET);
            if (pGobHS->fileRead(pGob->hGobFile, pData, entry->fileSize) == (size_t)entry->fileSize)
            {
                result->bIsMemoryMapped = 1;
                result->pMemory = (intptr_t)pData;
                result->memorySz = entry->fileSize;
            }
            else
            {
                free(pData);
            }
        }
    }
#endif
    result->bUsed = 1;
    result->parent = pGob;
    result->entry = entry;
    result->offset = 0;
    // Added: Opening another file in this GOB makes the shared handle's position
    // ambiguous: invalidate the seek-skip cache so the next read re-seeks.
    pGob->pCurHandle = 0;
    return result;
}

#ifdef TARGET_GAMECUBE
// Added: tell the disc cache that a sub-file will be read soon, without opening
// (and so reading) it now.
int stdGob_GcHintFile(Gob *pGob, const char *aName)
{
    char path[128];
    stdString_SafeStrCopy(path, aName, sizeof(path));
    stdString_CStrToLower(path);
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') path[i] = '\\';
    stdGobEntry* entry = (stdGobEntry*)stdHashtbl_Find(pGob->pDirHash, path);
    if (!entry) return 0;
#ifdef GC_LOG_GOB_OPENS
    // Hinted sounds are read later, at first play: record them too, so each
    // level's sounds end up contiguous and its hints fit the ARAM cache.
    printf("GOBOPEN %s %s\n", pGob->fpath, path);
#endif
    gcDvd_HintFileRange((FILE*)pGob->hGobFile, entry->fileOffset, entry->fileSize);
    return 1;
}
#endif

void stdGob_FileClose(GobFileHandle *pHandle)
{
    STD_ASSERTREL(pHandle->bUsed); // Added: J3D assert (pHandle unconditionally dereferenced below)
#ifdef QOL_IMPROVEMENTS
    if (pHandle->pMemory) {
        free((void*)pHandle->pMemory);
        pHandle->pMemory = (intptr_t)NULL;
    }
    pHandle->bIsMemoryMapped = 0;
#endif

    Gob* gob = pHandle->parent;
    pHandle->bUsed = 0;

    if (pHandle == gob->pCurHandle) {
        gob->pCurHandle = 0;
    }
}

int stdGob_FileSeek(GobFileHandle *pHandle, int offset, int origin)
{
    int seekOffsAbsolute;
    Gob *gob;

    STD_ASSERTREL(pHandle); // Added: J3D assert (pHandle unconditionally dereferenced below)

    seekOffsAbsolute = 0;
    switch (origin)
    {
        case SEEK_SET:
            seekOffsAbsolute = offset;
            break;
        case SEEK_CUR:
            seekOffsAbsolute = offset + pHandle->offset;
            break;
        case SEEK_END:
            seekOffsAbsolute = offset + pHandle->entry->fileSize;
            break;
        default:
            return 0;
    }

    gob = pHandle->parent;
    pHandle->offset = seekOffsAbsolute;

    if (pHandle == gob->pCurHandle)
        gob->pCurHandle = 0;

    return 1;
}

int32_t stdGob_FileTell(GobFileHandle *pHandle)
{
    STD_ASSERTREL(pHandle); // Added: J3D assert (pHandle unconditionally dereferenced below)
    return pHandle->offset;
}

bool stdGob_FileEOF(GobFileHandle *pHandle)
{
    STD_ASSERTREL(pHandle); // Added: J3D assert (pHandle unconditionally dereferenced below)
    int ret = 0;
    ret = pHandle->offset >= pHandle->entry->fileSize - 1;
    return ret;
}

size_t stdGob_FileRead(GobFileHandle *pHandle, void *data, uint32_t size)
{
    Gob *gob;
    size_t result;

    //printf("\x1b[0;0Hreading %s %p %x\n", f->entry->fname, out, len);

#ifdef QOL_IMPROVEMENTS
    if (pHandle->bIsMemoryMapped) {
        size_t to_read = size;
        if (pHandle->offset >= pHandle->memorySz) {
            pHandle->offset = pHandle->memorySz;
            return 0;
        }

        if (pHandle->offset + to_read > pHandle->memorySz) {
            to_read = pHandle->memorySz - pHandle->offset;
        }
        memcpy(data, (void*)(pHandle->pMemory + pHandle->offset), to_read);
        pHandle->offset += to_read;

        return to_read;
    }
#endif

    gob = pHandle->parent;
    if (gob->pCurHandle != pHandle)
    {
        pGobHS->fseek(gob->hGobFile, pHandle->offset + pHandle->entry->fileOffset, 0);
        gob = pHandle->parent;
        gob->pCurHandle = pHandle;
    }

    if ( pHandle->entry->fileSize - pHandle->offset < size )
        size = pHandle->entry->fileSize - pHandle->offset;

    result = pGobHS->fileRead(gob->hGobFile, data, size);
    pHandle->offset += result;
    return result;
}

const char* stdGob_FileGets(GobFileHandle *pGobFileHandle, char *pStr, unsigned int size)
{
    stdGobEntry *entry;
    int offset;
    const char *result;
    Gob *gob;

#ifdef QOL_IMPROVEMENTS
    if (pGobFileHandle->bIsMemoryMapped) {
        size_t to_read = size;
        if (pGobFileHandle->offset >= pGobFileHandle->memorySz) {
            pGobFileHandle->offset = pGobFileHandle->memorySz;
            return NULL;
        }

        if (pGobFileHandle->offset + to_read > pGobFileHandle->memorySz) {
            to_read = pGobFileHandle->memorySz - pGobFileHandle->offset;
        }
        if (!to_read || !size) {
            return NULL;
        }
        // Added: fgets semantics -- at most size-1 chars, stop after '\n', always
        // terminated. (strncpy of the full size left pStr unterminated, and the
        // strchr below then ran into stale buffer contents, dropping lines.)
        if (to_read > size - 1) {
            to_read = size - 1;
        }
        const char* pSrc = (const char*)(pGobFileHandle->pMemory + pGobFileHandle->offset);
        size_t actual_read = 0;
        while (actual_read < to_read && pSrc[actual_read]) {
            char c = pSrc[actual_read];
            pStr[actual_read++] = c;
            if (c == '\n') break;
        }
        pStr[actual_read] = 0;
        pGobFileHandle->offset += actual_read;

        if (!actual_read) return NULL;

        return pStr;
    }
#endif

    entry = pGobFileHandle->entry;
    offset = pGobFileHandle->offset;
    if ( offset >= entry->fileSize - 1 )
        return 0;
    gob = pGobFileHandle->parent;
    if ( gob->pCurHandle != pGobFileHandle )
    {
        pGobHS->fseek(gob->hGobFile, offset + entry->fileOffset, 0);
        gob = pGobFileHandle->parent;
        gob->pCurHandle = pGobFileHandle;
    }

    if ( pGobFileHandle->entry->fileSize - pGobFileHandle->offset + 1 < size )
        size = pGobFileHandle->entry->fileSize - pGobFileHandle->offset + 1;

    result = pGobHS->fileGets(gob->hGobFile, pStr, size);
    if ( result )
        pGobFileHandle->offset += _strlen(result);

    return result;
}

const char16_t* stdGob_FileGetws(GobFileHandle *f, char16_t *out, unsigned int len)
{
    stdGobEntry *entry; // ecx
    int offset; // edx
    Gob *gob; // eax
    unsigned int seekOffs_; // edi
    unsigned int len_wide; // ecx
    const char16_t *ret; // eax
    const char16_t *ret_; // edi

#ifdef QOL_IMPROVEMENTS
    if (f->bIsMemoryMapped) {
        size_t to_read = len * sizeof(char16_t);
        if (f->offset >= f->memorySz) {
            f->offset = f->memorySz;
            return 0;
        }

        if (f->offset + to_read > f->memorySz) {
            to_read = f->memorySz - f->offset;
        }
        if (!to_read || !len) {
            return NULL;
        }
        // Added: same fgets semantics as stdGob_FileGets (at most len-1 chars,
        // stop after '\n', always terminated inside the buffer).
        size_t maxChars = to_read / sizeof(char16_t);
        if (maxChars > len - 1) {
            maxChars = len - 1;
        }
        const char16_t* pSrc = (const char16_t*)(f->pMemory + f->offset);
        size_t numChars = 0;
        while (numChars < maxChars && pSrc[numChars]) {
            char16_t c = pSrc[numChars];
            out[numChars++] = c;
            if (c == '\n') break;
        }
        out[numChars] = 0;

        size_t actual_read = numChars * sizeof(char16_t);
        f->offset += actual_read;

        if (!actual_read) return NULL;

        return out;
    }
#endif

    entry = f->entry;
    offset = f->offset;
    if ( offset >= entry->fileSize - 1 )
        return 0;
    gob = f->parent;
    if ( gob->pCurHandle != f )
    {
        pGobHS->fseek(gob->hGobFile, offset + entry->fileOffset, 0);
        gob = f->parent;
        gob->pCurHandle = f;
    }
    seekOffs_ = f->offset;
    len_wide = len;
    if ( ((f->entry->fileSize - seekOffs_) >> 1) + 1 < len )
        len_wide = ((f->entry->fileSize - seekOffs_) >> 1) + 1;
    ret = pGobHS->fileGetws(gob->hGobFile, out, len_wide);
    if (ret)
        f->offset += _wcslen(ret);
    return ret;
}

// ADDED
size_t stdGob_FileSize(GobFileHandle *f)
{
    if (!f) return 0;
    if (!f->entry) return 0;
#ifdef QOL_IMPROVEMENTS
    if (f->bIsMemoryMapped) {
        return f->memorySz;
    }
#endif

    return f->entry->fileSize;
}
