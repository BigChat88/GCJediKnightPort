#ifndef _GC_EXFAT_H
#define _GC_EXFAT_H

// Minimal read-only exFAT reader, just enough to find the game's disc image on an
// SD card and read it (libfat only handles FAT12/16/32, while the large cards Swiss
// users run are usually exFAT).

#include <stdint.h>
#include <ogc/disc_io.h>

// Mount the exFAT volume on `iface` (raw volume, MBR or GPT) and look for an
// .iso/.gcm whose first 6 bytes are `id6`: in the root, and up to two levels
// below /games. On success the image becomes the file gcExfat_Read reads from.
int gcExfat_FindImage(const DISC_INTERFACE* iface, const char* id6, char* pPathOut, int pathLen);

// Read from the image found by gcExfat_FindImage. Bytes past its end read as zero.
// Not thread safe.
int gcExfat_Read(void* pDst, uint32_t len, uint64_t offset);

#endif // _GC_EXFAT_H
