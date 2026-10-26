#ifndef _LIBSMUSHER_SMUSH_H
#define _LIBSMUSHER_SMUSH_H

#include "endian.h"

#include <stdio.h>

extern int _smush_debug_prints;

typedef FILE* filehandle_t;
typedef void (*smush_audio_callback_t)(const uint8_t*, size_t);

typedef struct smush_header
{
    uint8_t magic[4];
    uint8_t size[4];
} smush_header;

typedef struct smush_ahdr
{
    uint8_t magic[4];
    uint8_t size[4];
    uint8_t version[2];
    uint8_t num_frames[2];
    uint8_t unk1[2];
    uint8_t palette[256 * 3];
} smush_ahdr;

typedef struct smush_ahdr_ext
{
    uint8_t frame_rate[4];
    uint8_t unk2[4];
    uint8_t audio_rate[4];
} smush_ahdr_ext;

typedef struct smush_fobj
{
    uint8_t magic[4];
    uint8_t size[4];
    uint8_t codec;
    uint8_t codec_param;
    uint8_t x[2];
    uint8_t y[2];
    uint8_t width[2];
    uint8_t height[2];
    uint8_t unk3[2];
    uint8_t unk4[2];
} smush_fobj;

typedef struct smush_iact
{
    uint8_t code[2];
    uint8_t flags[2];
    uint8_t unk[2];
    uint8_t track_flags[2];
} smush_iact;

typedef struct smush_imuse_iact
{
    uint8_t track_id[2];
    uint8_t index[2];
    uint8_t frame_count[2];
    uint8_t bytes_left[4];
} smush_imuse_iact;

typedef struct smush_tres
{
    uint8_t unk1[2];
    uint8_t unk2[2];
    uint8_t unk3[4];
    uint8_t unk4[2];
    uint8_t unk5[2];
    uint8_t unk6[2];
    uint8_t unk7[2];
    uint8_t subtitle_index[2];
} smush_tres;

typedef struct smush_stor
{
    uint8_t idk[4];
} smush_stor;

typedef struct smush_ftch
{
    uint8_t idk_0[2];
    uint8_t idk[2];
    uint8_t padding[2];
} smush_ftch;

typedef struct codec48_ctx codec48_ctx; 
typedef struct codec47_ctx codec47_ctx;
typedef struct smush_ctx
{
    char fpath[512];
    filehandle_t f;
    smush_header header;
    smush_ahdr ahdr;
    smush_ahdr_ext ahdr_ext;

    uint8_t palette[256*3];

    uint32_t start_fpos;
    uint32_t max_fpos;
    uint32_t frame_fpos;
    uint8_t num_channels;
    uint32_t cur_frame;

    codec48_ctx* c48_ctx;
    codec47_ctx* c47_ctx;
    uint8_t* framebuffer;      // Added: allocated on first use (smush_framebuffer)
    uint8_t* framebuffer_stor;
    uint8_t* video;            // Added: the frame to show when a codec's own buffer holds it
    uint32_t video_size;       // Added: its size in bytes
    int store_next;
    uint32_t store_param;

    int16_t codec_x;
    int16_t codec_y;
    uint16_t codec_w;
    uint16_t codec_h;

    int current_sub;

    uint16_t delta_palette[256 * 3];

    int iact_idx;
    uint8_t iact_tmp[0x10008];

    smush_audio_callback_t audio_callback;
    uint32_t audio_buffer_size;
    uint32_t audio_buffer_collected;
    uint32_t audio_buffer_collected_flushed;
    uint8_t* audio_buffer_tmp;
    uint32_t audio_frame_fpos;
    uint32_t audio_cur_frame;

    // Added: one chunk buffer reused for every FOBJ/XPAL/IACT read (a fresh
    // ~250 KiB malloc per frame fails on a fragmented console heap).
    uint8_t* scratch;
    uint32_t scratch_size;
} smush_ctx;

// BE32
#define SMUSH_MAGIC_ANIM (0x414e494d)
#define SMUSH_MAGIC_AHDR (0x41484452)
#define SMUSH_MAGIC_FRME (0x46524D45)
#define SMUSH_MAGIC_FOBJ (0x464F424A)
#define SMUSH_MAGIC_XPAL (0x5850414C)
#define SMUSH_MAGIC_NPAL (0x4E50414C)
#define SMUSH_MAGIC_IACT (0x49414354)
#define SMUSH_MAGIC_FTCH (0x46544348)
#define SMUSH_MAGIC_STOR (0x53544F52)
#define SMUSH_MAGIC_TRES (0x54524553)

#define smush_error(...) { printf(__VA_ARGS__); }
#define smush_warn(...) { printf(__VA_ARGS__); }
#define smush_debug(...) { if (_smush_debug_prints) printf(__VA_ARGS__); }

// Added: called when an allocation fails, so the host can free caches; the
// allocation is then retried once.
extern void (*smush_low_memory_hook)(uint32_t size);
// Added: tried when malloc still fails; its blocks must be freeable with free().
extern void* (*smush_alloc_fallback)(uint32_t size);
extern int smush_alloc_failed; // an allocation failed even after the hook (video can't play)
extern uint32_t smush_failed_size; // the size of that allocation
void* smush_alloc(uint32_t size);
// Added: the 640x480 frame buffer, allocated (zeroed) on first use; NULL without memory.
uint8_t* smush_framebuffer(smush_ctx* ctx);

smush_ctx* smush_from_fpath(const char* fpath);
void smush_set_debug(smush_ctx* ctx, int val);
void smush_set_audio_buffer_size(smush_ctx* ctx, uint32_t buffer_size);
void smush_set_audio_callback(smush_ctx* ctx, smush_audio_callback_t callback);

void smush_destroy(smush_ctx* ctx);
int smush_done(smush_ctx* ctx);
int smush_cur_frame(smush_ctx* ctx);
int smush_num_frames(smush_ctx* ctx);
uint8_t* smush_get_palette(smush_ctx* ctx);
uint8_t* smush_get_video(smush_ctx* ctx);
uint32_t smush_video_width(smush_ctx* ctx);
uint32_t smush_video_height(smush_ctx* ctx);
int smush_video_fps(smush_ctx* ctx);
int smush_audio_rate(smush_ctx* ctx);     // SAHD-declared audio rate (11025 for DroidWorks)
int smush_audio_channels(smush_ctx* ctx); // IACT channel count (1 = mono)
int smush_get_current_subtitle(smush_ctx* ctx);

void smush_audio_flush(smush_ctx* ctx);

void smush_frame(smush_ctx* ctx);
void smush_proc_frme(smush_ctx* ctx, uint32_t seek_pos, uint32_t total_size, int is_audio_only);

void smush_print(smush_ctx* ctx);
void smush_print_frme(smush_ctx* ctx, uint32_t seek_pos, uint32_t total_size);

#endif // _LIBSMUSHER_SMUSH_H