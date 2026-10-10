#include "General/stdEndian.h"
#include "sithSound.h"
#ifdef TARGET_GAMECUBE
#include "Main/jkRes.h"
#endif

#include "Win95/stdSound.h"
#include "General/stdHashtbl.h"
#include "General/stdString.h"
#include "General/stdFnames.h"
#include "World/sithWorld.h"
#include "stdPlatform.h" // Added: word-safe read bounce for VRAM-resident PCM
#include "jk.h"
#ifdef TARGET_GAMECUBE
#include "Platform/GameCube/gcSystem.h"
#include "Platform/GameCube/gcSndBank.h"
#include "Platform/GameCube/gcAram.h"
#endif

#include <ctype.h>

// Un-inlined: searches semicolon-delimited directory list for a sound file.
// Returns file handle on success, 0 on failure. outPath receives the full path found.
static stdFile_t sithSound_FindInSearchPaths(const char *filename, char *outPath, int outPathSize)
{
    char dirBuf[128];
    char *searchPaths = "sound;voice";
    while ( 1 )
    {
        searchPaths = stdString_CopyBetweenDelimiter(searchPaths, dirBuf, 128, ";");
        if ( dirBuf[0] )
        {
            stdString_snprintf(outPath, outPathSize, "%s%c%s", dirBuf, '\\', filename);
            stdFile_t fd = pSithHS->fileOpen(outPath, "rb");
            if ( fd )
                return fd;
        }
        if ( !searchPaths )
            return 0;
    }
}

int sithSound_Startup()
{
#if defined(TARGET_GAMECUBE)
    // Added: sounds load from the disc on first use and stay resident up to this
    // much PCM. 4 MiB plus the texture copies fragmented main RAM late in a level
    // until full-screen menu backgrounds no longer fit; reloads come from the ARAM
    // disc cache, so 2 MiB costs little.
    // The PCM goes to the sound arena (gcHeap.c, 1.5 MiB): stay a little under it
    // so the least recently used sounds are dropped before it overflows into the
    // main heap.
    sithSound_maxDataLoaded = 0x160000;
#elif defined(TARGET_RETRO_HOMEBREW)
    if (openjkdf2_bIsExtraLowMemoryPlatform)
        sithSound_maxDataLoaded = 0x60000;
    else
        sithSound_maxDataLoaded = 0x200000;
#elif defined(QOL_IMPROVEMENTS)
    sithSound_maxDataLoaded = 0x4000000; // 64MiB
#endif

    if ( stdSound_Startup() )
    {
        sithSound_hashtable = stdHashtbl_New(256);
        if ( sithSound_hashtable )
        {
            sithSound_bInit = 1;
            return 1;
        }
    }
    return 0;
}

int sithSound_Shutdown()
{
    if ( !sithSound_bInit )
        return 0;

    stdSound_Shutdown();
    if ( sithSound_hashtable )
    {
        stdHashtbl_Free(sithSound_hashtable);
        sithSound_hashtable = 0;
    }

    sithSound_bInit = 0;
    return 1;
}

int sithSound_ReadSoundsListText(SithWorld *pWorld, int bSkip)
{
    int numSounds; // eax

    SITH_ASSERTREL(pWorld); // Added: from OpenJones3D

    if ( bSkip )
        return 0;

    sithWorld_UpdateLoadProgress(0.0);
    if (!stdConffile_ReadArgs() 
        || _strcmp(stdConffile_g_entry.aArgs[0].value, "world") 
        || _strcmp(stdConffile_g_entry.aArgs[1].value, "sounds") )
    {
        sithSound_FreeWorldSounds(pWorld);
        return 0;
    }
    numSounds = _atoi(stdConffile_g_entry.aArgs[2].value);
    if ( !numSounds )
        return 1;

    sithSound_New(pWorld, numSounds);
    
    while ( stdConffile_ReadArgs() )
    {
        if ( !_strcmp(stdConffile_g_entry.aArgs[0].value, "end") )
            break;
        if ( sithSound_bInit )
            sithSound_Load(stdConffile_g_entry.aArgs[0].value, stdConffile_g_entry.numArgs > 1u);
    }
    return 1;
}

void sithSound_FreeWorldSounds(SithWorld *pWorld)
{
    SITH_ASSERTREL(pWorld != NULL); // Added: from OpenJones3D

    if (pWorld->sounds)
    {
        for (int i = 0; i < pWorld->numSoundsLoaded; i++)
        {
            sithSound_UnloadData(&pWorld->sounds[i]);
#ifdef TARGET_GAMECUBE
            if (pWorld->sounds[i].gcAram) // Added: its copy in the ARAM sound bank
                gcAram_Free(pWorld->sounds[i].gcAram, pWorld->sounds[i].gcAramBytes);
            pWorld->sounds[i].gcAram = 0;
#endif
            stdHashtbl_Remove(sithSound_hashtable, pWorld->sounds[i].sound_fname);
        }
        SITH_FREE(pWorld->sounds);
        pWorld->numSoundsLoaded = 0;
        pWorld->numSounds = 0;
        pWorld->sounds = 0;
    }
}

// MOTS altered
int sithSound_New(SithWorld *world, int num)
{
    // MOTS added: reserve sound 0?
    if (Main_bMotsCompat) {
        num++;
    }

    sithSound* sounds  = (sithSound *)SITH_ALLOC(sizeof(sithSound) * num);
    world->sounds = sounds;
    if ( sounds )
    {
        _memset(sounds, 0, sizeof(sithSound) * num);
        world->numSounds = num;
        world->numSoundsLoaded = 0;

        //  MOTS added TODO: wat?
        if (Main_bMotsCompat) {
            //world->numSoundsLoaded = 1;
        }
        return 1;
    }
    return 0;
}

#ifdef SITHSOUND_INFO_TABLE
// Added: pre-parsed WAV headers (resource/wavinfo.txt, generated when the disc is
// built). On optical media each header read is a seek; with the table, a level
// load touches no sound file at all and the PCM is only read on first play.
typedef struct sithSoundInfo
{
    char name[48];         // "sound\\xxx.wav", lowercase
    uint32_t sampleRate;
    int32_t bitsPerSample;
    int32_t bStereo;
    int32_t dataOffset;
    uint32_t dataBytes;
} sithSoundInfo;

static sithSoundInfo* sithSound_aInfo = NULL;
static int sithSound_numInfo = 0;
static tHashTable* sithSound_pInfoHash = NULL;
static int sithSound_bInfoTried = 0;

static void sithSound_LoadInfoTable(void)
{
    char line[160];
    sithSound_bInfoTried = 1;

    stdFile_t f = pSithHS->fileOpen("resource\\wavinfo.txt", "r");
    if (!f) return;
    int count = 0;
    if (!pSithHS->fileGets(f, line, sizeof(line)) || _sscanf(line, "WAVINFO %d", &count) != 1 || count <= 0) {
        pSithHS->fileClose(f);
        return;
    }
    sithSound_aInfo = (sithSoundInfo*)pSithHS->alloc(sizeof(sithSoundInfo) * count);
    sithSound_pInfoHash = stdHashtbl_New(count * 2);
    if (!sithSound_aInfo || !sithSound_pInfoHash) {
        pSithHS->fileClose(f);
        return;
    }
    while (sithSound_numInfo < count && pSithHS->fileGets(f, line, sizeof(line))) {
        sithSoundInfo* pInfo = &sithSound_aInfo[sithSound_numInfo];
        if (_sscanf(line, "%47s %u %d %d %d %u", pInfo->name, &pInfo->sampleRate, &pInfo->bitsPerSample,
                    &pInfo->bStereo, &pInfo->dataOffset, &pInfo->dataBytes) == 6) {
            stdHashtbl_Add(sithSound_pInfoHash, pInfo->name, pInfo);
            sithSound_numInfo++;
        }
    }
    pSithHS->fileClose(f);
    stdPlatform_Printf("sithSound: %d pre-parsed WAV headers\n", sithSound_numInfo);
}

static sithSoundInfo* sithSound_FindInfo(const char* filename)
{
    static const char* aDirs[] = { "sound", "voice" };
    char key[64];
    if (!sithSound_bInfoTried) sithSound_LoadInfoTable();
    if (!sithSound_pInfoHash) return NULL;
    for (int i = 0; i < 2; i++) {
        stdString_snprintf(key, sizeof(key), "%s\\%s", aDirs[i], filename);
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        sithSoundInfo* pInfo = (sithSoundInfo*)stdHashtbl_Find(sithSound_pInfoHash, key);
        if (pInfo) return pInfo;
    }
    return NULL;
}
#endif // SITHSOUND_INFO_TABLE

sithSound* sithSound_Load(char *sound_fname, int a2)
{
    stdFile_t sound_file; // ebp
    sithSound *sound; // esi
    uint32_t v7; // eax
    uint32_t v10; // eax
    uint32_t frequencyKHz; // eax
    struct HostServices *v12; // ecx
    char tmp2[128];

    sound_file = 0;

    SITH_ASSERTREL(sound_fname != NULL); // Added: from OpenJones3D (filename)

    if ( !sithSound_bInit )
        return 0;

    if ( !_strcmp(sound_fname, "none") )
        return 0;

    //printf("sithSound_Load %s\n", sound_fname);
    sound = (sithSound *)stdHashtbl_Find(sithSound_hashtable, sound_fname);
    if ( sound )
    {
        if ( a2 && (sound->isLoaded & 1) == 0
#ifdef TARGET_GAMECUBE
             && !sound->gcAram // Added: in ARAM already, decoded when it plays
#endif
           )
        {
            sithSound_LoadFileData(sound);
            return sound;
        }
        return sound;
    }

#ifdef TARGET_GAMECUBE
    // Added: the sound bank (gcSndBank): the sound's DSP-ADPCM goes to ARAM now,
    // and it is decoded from there when it plays -- never read from the disc in
    // play. Without room in ARAM it plays from the WAV as before.
    gcSndBankInfo bankInfo;
    uint32_t bankAram = 0, bankAramBytes = 0;
    if ( sithWorld_g_pLastLoadedWorld->numSoundsLoaded < sithWorld_g_pLastLoadedWorld->numSounds
      && gcSndBank_Find(sound_fname, &bankInfo) )
        bankAram = gcSndBank_LoadToAram(&bankInfo, &bankAramBytes);
#endif

#ifdef SITHSOUND_INFO_TABLE
    sithSoundInfo* pInfo = sithSound_FindInfo(sound_fname);
    if (!pInfo)
#endif
#ifdef TARGET_GAMECUBE
    if (!bankAram)
#endif
    {
        sound_file = sithSound_FindInSearchPaths(sound_fname, tmp2, 128);
        if ( !sound_file )
            return 0;
    }

    if ( sithWorld_g_pLastLoadedWorld->numSoundsLoaded < sithWorld_g_pLastLoadedWorld->numSounds )
    {
        sound = &sithWorld_g_pLastLoadedWorld->sounds[sithWorld_g_pLastLoadedWorld->numSoundsLoaded];
        sound->id = sithWorld_g_pLastLoadedWorld->numSoundsLoaded;
        if ((sithWorld_g_pLastLoadedWorld->level_type_maybe & 1))
        {
            sound->id |= 0x8000;
        }
        stdString_SafeStrCopy(sound->sound_fname, sound_fname, 32);
#ifdef TARGET_GAMECUBE
        sound->gcAram = bankAram;
        sound->gcAramBytes = bankAramBytes;
        if (bankAram) {
            // Decoded, the bank's sounds are always 16-bit.
            sound->bufferBytes = bankInfo.pcmBytes;
            sound->sampleRate = bankInfo.sampleRate;
            sound->bitsPerSample = 16;
            sound->bStereo = bankInfo.channels == 2;
            sound->seekOffset = 0;
        }
        else
#endif
#ifdef SITHSOUND_INFO_TABLE
        if (pInfo) {
            sound->bufferBytes = pInfo->dataBytes;
            sound->sampleRate = pInfo->sampleRate;
            sound->bitsPerSample = pInfo->bitsPerSample;
            sound->bStereo = pInfo->bStereo;
            sound->seekOffset = pInfo->dataOffset;
        }
        else
#endif
        sound->bufferBytes = stdSound_ParseWav(sound_file, &sound->sampleRate, &sound->bitsPerSample, &sound->bStereo, &sound->seekOffset);
        if ( sound->bufferBytes )
        {
            frequencyKHz = sound->bufferBytes / (sound->sampleRate / 1000u);

            sound->sound_len = frequencyKHz;
            if ( sound->bitsPerSample == 16 )
                sound->sound_len = frequencyKHz / 2;
            if ( sound->bitsPerSample == 24 ) // Added
                sound->sound_len = frequencyKHz / 3;
            if ( sound->bStereo )
                sound->sound_len = sound->sound_len >> 1;
            sound->isLoaded = 0;
            sound->infoLoaded = 1;
            stdHashtbl_Add(sithSound_hashtable, sound->sound_fname, sound);
#ifdef TARGET_GAMECUBE
            // Added: sound data is only read the first time it plays (a stall on
            // the first shot): have the disc cache fetch it in the background now.
            // (Not for sounds in the bank: those are in ARAM already.)
            if (!sound->gcAram)
            {
                char hintPath[128];
                stdString_snprintf(hintPath, sizeof(hintPath), "sound\\%s", sound_fname);
                if (!jkRes_GcHintFile(hintPath)) {
                    stdString_snprintf(hintPath, sizeof(hintPath), "voice\\%s", sound_fname);
                    jkRes_GcHintFile(hintPath);
                }
            }
#endif
            v12 = pSithHS;
            ++sithWorld_g_pLastLoadedWorld->numSoundsLoaded;
            if (sound_file) // Added: none when the header came from the info table
                v12->fileClose(sound_file);
            return sound;
        }
    }
    if ( sound_file )
        pSithHS->fileClose(sound_file);
#ifdef TARGET_GAMECUBE
    if (bankAram) gcAram_Free(bankAram, bankAramBytes);
#endif
    return 0;
}

sithSound* sithSound_GetFromIdx(int idx)
{
    SithWorld* world = sithWorld_g_pCurrentWorld;

    if (idx & 0x8000)
    {
        world = sithWorld_g_pStaticWorld;
        idx &= ~0x8000;
    }

    if ( idx < 0 || idx >= world->numSoundsLoaded )
        return 0;


    return &world->sounds[idx];
}


#ifdef TARGET_RETRO_HOMEBREW
// Added: fileRead byte-writes into the destination, but long-sound PCM may live
// in word-addressable-only memory (DC VRAM arena). Bounce those through a chunk
// buffer with word-safe copies; everything else reads direct.
static int sithSound_ReadIntoBuffer(int fd, void* pDst, int len)
{
    if (!stdPlatform_IsWordAddressableOnly(pDst))
        return pSithHS->fileRead(fd, pDst, len);

    static char aBounce[16384]; // sound loads are main-thread only
    char* pOut = (char*)pDst;
    int total = 0;
    while (total < len) {
        int chunk = len - total;
        if (chunk > (int)sizeof(aBounce)) chunk = (int)sizeof(aBounce);
        int got = pSithHS->fileRead(fd, aBounce, chunk);
        if (got <= 0) break;
        stdPlatform_Memcpy32(pOut + total, aBounce, got);
        total += got;
        if (got < chunk) break;
    }
    return total;
}
#else
#define sithSound_ReadIntoBuffer(fd, pDst, len) pSithHS->fileRead((fd), (pDst), (len))
#endif

#ifdef TARGET_GAMECUBE
uint32_t sithSound_gcBadLoads = 0; // Added: diagnostics (stats panel)
char sithSound_gcLastBad[24] = "";
#endif

int sithSound_LoadFileData(sithSound *sound)
{
    void *buf; // ebp
    int32_t bufferMaxSize; // [esp+10h] [ebp-84h] BYREF
    char outstr2[128];

    stdFile_t fd = 0;
    if (sound->isLoaded & 1)
        return 0;

    if ( sound->bufferBytes + sithSound_curDataLoaded > sithSound_maxDataLoaded )
        sithSound_FreeUpMemory(sound->bufferBytes + 0x19000);
#ifdef TARGET_GAMECUBE
    // Added: keep GC_MEM_RESERVE free; idle sounds reload from the ARAM cache.
    // Not once the sound arena exists: sounds live there, so dropping them gives
    // the main heap nothing back (it only threw out sounds about to play, which
    // then had to be decoded again: the level 1 TIE bomber sound started late).
    extern uint32_t gcHeap_SndArenaSize(void);
    if (!gcHeap_SndArenaSize())
    {
        uint32_t shortfall = gcSystem_MemShortfall(sound->bufferBytes);
        if (shortfall)
            sithSound_FreeUpMemory(shortfall);
    }
#endif
    stdSound_buffer_t* dsoundBuf = stdSound_BufferCreate(sound->bStereo, sound->sampleRate, sound->bitsPerSample, sound->bufferBytes);
#ifdef TARGET_GAMECUBE
    if ( dsoundBuf && sound->gcAram )
    {
        // Added: from the sound bank in ARAM (see gcSndBank.c), no disc access.
        sound->dsoundBuffer2 = dsoundBuf;
        sithSound_curDataLoaded += sound->bufferBytes;
        buf = stdSound_BufferSetData(sound->dsoundBuffer2, sound->bufferBytes, &bufferMaxSize);
        if ( buf && (uint32_t)bufferMaxSize >= sound->bufferBytes )
        {
            gcSndBank_Decode(sound->gcAram, sound->bStereo ? 2 : 1,
                             sound->bufferBytes / (sound->bStereo ? 4 : 2), (int16_t*)buf);
            if ( stdSound_BufferUnlock(sound->dsoundBuffer2, buf, sound->bufferBytes) )
            {
                sound->isLoaded |= 1u;
                return 1;
            }
        }
    }
    else
#endif
    if ( dsoundBuf )
    {
        sound->dsoundBuffer2 = dsoundBuf;
        sithSound_curDataLoaded += sound->bufferBytes;
        
        fd = sithSound_FindInSearchPaths(sound->sound_fname, outstr2, 128);
        if ( fd )
        {
            pSithHS->fseek(fd, sound->seekOffset, 0);
            // Added: on TWL the ARM9 mixes from source PCM, so it can live in
            // extram (the fill below goes through the word-safe read bounce);
            // only the ARM7-visible mix output buffer needs HEAP_AUDIO.
            { TWL_EXTRAM_SUGGEST(pSithHS);
            buf = stdSound_BufferSetData(sound->dsoundBuffer2, sound->bufferBytes, &bufferMaxSize);
            TWL_EXTRAM_RESTORE(pSithHS); }
            if ( buf )
            {
                int numRead = sithSound_ReadIntoBuffer(fd, buf, bufferMaxSize); // Added: word-safe for VRAM-resident PCM
                if (sound->bitsPerSample == 16) // Added: WAV PCM is little-endian
                    stdEndian_SwapLE16Array(buf, numRead / 2);
#ifdef TARGET_GAMECUBE
                // Added: a read that got nothing is a failed load (BufferUnlock takes
                // 0 as "the whole buffer").
                if (numRead != bufferMaxSize) {
                    extern uint32_t sithSound_gcBadLoads;
                    extern char sithSound_gcLastBad[24];
                    sithSound_gcBadLoads++;
                    stdString_SafeStrCopy(sithSound_gcLastBad, sound->sound_fname, sizeof(sithSound_gcLastBad));
                }
                if (numRead <= 0) buf = NULL;
                else
#endif
                if ( stdSound_BufferUnlock(sound->dsoundBuffer2, buf, numRead) )
                {
                    sound->isLoaded |= 1u;
                    pSithHS->fileClose(fd);
                    return 1;
                }
            }
        }
    }
    if ( fd )
        pSithHS->fileClose(fd);
    if ( sound->dsoundBuffer2 )
    {
        stdSound_BufferRelease(sound->dsoundBuffer2);
        // Added: the counterpart of the += above -- a failed load must not leak
        // accounting, or curDataLoaded inflates until no sound can ever load again.
        sithSound_curDataLoaded -= sound->bufferBytes;
    }
    sound->dsoundBuffer2 = 0;
    return 0;
}

int sithSound_UnloadData(sithSound *sound)
{
    if (!(sound->isLoaded & 1)) {
        return 0;
    }

    stdSound_BufferRelease(sound->dsoundBuffer2);
    sound->isLoaded &= ~1u;
    sound->dsoundBuffer2 = 0;
    sithSound_curDataLoaded -= sound->bufferBytes;
    return 1;
}

stdSound_buffer_t* sithSound_LoadData(sithSound *sound)
{
    if (!(sound->isLoaded & 1))
    {
        //printf("sithSound_LoadData %s\n", sound->sound_fname);
        sithSound_LoadFileData(sound);
        if (!(sound->isLoaded & 1))
            return 0;
    }

    sound->infoLoaded = 1;
    return stdSound_BufferDuplicate(sound->dsoundBuffer2);
}

int sithSound_ReadDataFromFd(int fd, sithSound *sound)
{
    void *data;

    int32_t bufferBytes;
    { TWL_EXTRAM_SUGGEST(pSithHS); // Added: see sithSound_LoadFileData
    data = stdSound_BufferSetData(sound->dsoundBuffer2, sound->bufferBytes, &bufferBytes);
    TWL_EXTRAM_RESTORE(pSithHS); }
    if ( data )
    {
        int amt = sithSound_ReadIntoBuffer(fd, data, bufferBytes); // Added: word-safe for VRAM-resident PCM
        if (sound->bitsPerSample == 16) // Added: WAV PCM is little-endian
            stdEndian_SwapLE16Array(data, amt / 2);
        return stdSound_BufferUnlock(sound->dsoundBuffer2, data, amt);
    }
    return 0;
}

#ifdef TARGET_GAMECUBE
// Added: unload the PCM of every sound that isn't playing, in the static world
// and the current one. sithSound_FreeUpMemory starts from the current world and
// gives up when there is none -- the case at every loading screen -- so the
// static world's sounds (shots, steps, pickups...), loaded as they first play
// during a level, were never freed: ~0.5 MiB per level that stayed scattered
// through the heap and split the free memory the next level's large arrays need.
uint32_t sithSound_GcFreeAllIdle(void)
{
    uint32_t freed = 0;
    SithWorld* aWorlds[2] = { sithWorld_g_pStaticWorld, sithWorld_g_pCurrentWorld };
    for (int w = 0; w < 2; w++) {
        SithWorld* world = aWorlds[w];
        if (!world || (w == 1 && world == aWorlds[0]) || !world->sounds) continue;
        for (uint32_t i = 0; i < world->numSoundsLoaded; i++) {
            sithSound* snd = &world->sounds[i];
            if (!(snd->isLoaded & 1) || snd->field_40 || stdSound_IsPlaying(snd->dsoundBuffer2, 0))
                continue;
            stdSound_BufferRelease(snd->dsoundBuffer2);
            snd->isLoaded &= ~1u;
            snd->dsoundBuffer2 = 0;
            sithSound_curDataLoaded -= snd->bufferBytes;
            freed += snd->bufferBytes;
        }
    }
    return freed;
}
#endif

int sithSound_FreeUpMemory(uint32_t numBytesNeeded)
{
    SithWorld *world; // edi
    int result; // eax
    int v8; // [esp+10h] [ebp-Ch]
    int v9; // [esp+14h] [ebp-8h]
    unsigned int v10; // [esp+18h] [ebp-4h]

    v8 = sithSound_var5;
    v10 = 0;
    v9 = 0;
    while ( 1 )
    {
        world = sithWorld_g_pCurrentWorld;
        if ( v8 )
        {
            world = sithWorld_g_pStaticWorld;
            if (!world)
                world = sithWorld_g_pCurrentWorld;
            else if(!world) // Added: allow freeing sounds during loading
                world = sithWorld_g_pLastLoadedWorld;
            else
                v8 = 0;
        }

        // Added: Prevent nullptr deref
        if (!world) {
            return 0;
        }

        for (uint32_t v3 = sithSound_var4; v3 < world->numSoundsLoaded; v3++)
        {
            if ((world->sounds[v3].isLoaded & 1) 
                && !world->sounds[v3].field_40
                && !stdSound_IsPlaying(world->sounds[v3].dsoundBuffer2, 0))
            {
                if (world->sounds[v3].infoLoaded)
                {
                    world->sounds[v3].infoLoaded = 0;
                }
                else
                {
                    result = world->sounds[v3].bufferBytes + v9;
                    v9 = result;
                    if ((world->sounds[v3].isLoaded & 1))
                    {
                        stdSound_BufferRelease(world->sounds[v3].dsoundBuffer2);
                        world->sounds[v3].isLoaded &= ~1u;
                        world->sounds[v3].dsoundBuffer2 = 0;
                        sithSound_curDataLoaded -= world->sounds[v3].bufferBytes;
                        result = v9;
                    }
                    if ( result >= numBytesNeeded )
                    {
                        sithSound_var5 = v8;
                        sithSound_var4 = v3 + 1;
                        return result;
                    }
                }
            }
        }
        if ( v10 >= 2 )
            return 0;
        ++v10;
        sithSound_var4 = 0;
        v8 = v8 == 0;
        sithSound_var4 = 0;
    }
}

stdSound_buffer_t* sithSound_InitFromPath(char *path)
{
    stdFile_t fd; // ebx
    int bufferLen; // edi
    stdSound_buffer_t *createdBuf; // eax
    stdSound_buffer_t *dsoundBuf; // esi
    int32_t bStereo; // [esp+Ch] [ebp-94h] BYREF
    int32_t bufferMaxSize; // [esp+10h] [ebp-90h] BYREF
    uint32_t nSamplesPerSec; // [esp+14h] [ebp-8Ch] BYREF
    int32_t offset; // [esp+18h] [ebp-88h] BYREF
    int32_t bitsPerSample; // [esp+1Ch] [ebp-84h] BYREF
    char tmp[128]; // [esp+20h] [ebp-80h] BYREF

    if (!path)
        return NULL;

    _sprintf(tmp, "sound%c%s", LEC_PATH_SEPARATOR_CHR, path);
    fd = pSithHS->fileOpen(tmp, "rb");
    if ( fd )
    {
        bufferLen = stdSound_ParseWav(fd, &nSamplesPerSec, &bitsPerSample, &bStereo, &offset);
        if ( bufferLen )
        {
            createdBuf = stdSound_BufferCreate(bStereo, nSamplesPerSec, bitsPerSample, bufferLen);
            dsoundBuf = createdBuf;
            if ( createdBuf )
            {
                void* data = stdSound_BufferSetData(createdBuf, bufferLen, &bufferMaxSize);
                if ( !data )
                    return NULL;
                size_t amtRead = pSithHS->fileRead(fd, (void *)data, bufferMaxSize);
                if (bitsPerSample == 16) // Added: WAV PCM is little-endian
                    stdEndian_SwapLE16Array(data, amtRead / 2);
                if ( stdSound_BufferUnlock(dsoundBuf, data, amtRead) )
                {
                    pSithHS->fileClose(fd);
                    return dsoundBuf;
                }
            }
        }
        else
        {
            //dsoundBuf = (stdSound_buffer_t *)offset;
            // Added: fix undefined behavior?
            dsoundBuf = NULL;
        }
        pSithHS->fileClose(fd);
        if ( dsoundBuf )
            stdSound_BufferRelease(dsoundBuf);
    }
    return NULL;
}

