#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#define MINIMP3_IMPLEMENTATION
#include "vendor/minimp3.h"

#include "audio.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define PCM_BUF_SAMPLES  4096   /* samples per NDSP wavebuf (per ch) */
#define NUM_BUFS          3
#define CHANNEL           0
#define THREAD_STACK_SZ   (64 * 1024)

/* ── State ──────────────────────────────────────────────────────── */

static bool       s_inited;
static bool       s_enabled;
static mp3dec_t   s_dec;

static u8        *s_mp3_buf;       /* raw MP3 file data (malloc'd) */
static size_t     s_mp3_size;
static size_t     s_mp3_pos;       /* current read offset into MP3 */

static int        s_channels;
static int        s_sample_rate;

static ndspWaveBuf s_wavbufs[NUM_BUFS];
static s16        *s_ndsp_buf[NUM_BUFS]; /* linearAlloc'd NDSP buffers */

static Thread      s_thread;
static LightEvent  s_event;
static volatile bool s_thread_quit;

/* Leftover samples from a frame that didn't fully fit in the
 * previous buffer. Carried over to the next fill_buffer call. */
static s16    s_carry[MINIMP3_MAX_SAMPLES_PER_FRAME];
static size_t s_carry_count;   /* interleaved sample count */

/* ── Helpers ────────────────────────────────────────────────────── */

static void fill_buffer(int idx)
{
    size_t max_samples = (size_t)PCM_BUF_SAMPLES * s_channels;
    size_t written = 0;

    /* Drain any leftover samples from the previous call first */
    if (s_carry_count > 0) {
        size_t n = s_carry_count < max_samples ? s_carry_count : max_samples;
        memcpy(s_ndsp_buf[idx], s_carry, n * sizeof(s16));
        written = n;
        if (n < s_carry_count) {
            /* Still more carry than buffer space (shouldn't happen
             * in practice, but handle it safely) */
            memmove(s_carry, s_carry + n, (s_carry_count - n) * sizeof(s16));
            s_carry_count -= n;
        } else {
            s_carry_count = 0;
        }
    }

    /* Decode frames until the buffer is full */
    while (written < max_samples) {
        if (s_mp3_pos >= s_mp3_size) {
            mp3dec_init(&s_dec);
            s_mp3_pos = 0;
        }

        int remaining = (int)(s_mp3_size - s_mp3_pos);
        mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;

        int samples = mp3dec_decode_frame(&s_dec, s_mp3_buf + s_mp3_pos,
                                          remaining, frame_pcm, &info);
        if (info.frame_bytes == 0) {
            mp3dec_init(&s_dec);
            s_mp3_pos = 0;
            break;
        }
        s_mp3_pos += (size_t)info.frame_bytes;

        if (samples == 0)
            continue;

        size_t frame_total = (size_t)samples * info.channels;
        size_t space = max_samples - written;

        if (frame_total <= space) {
            /* Whole frame fits */
            memcpy(s_ndsp_buf[idx] + written, frame_pcm,
                   frame_total * sizeof(s16));
            written += frame_total;
        } else {
            /* Partial fit — copy what fits, save the rest */
            memcpy(s_ndsp_buf[idx] + written, frame_pcm,
                   space * sizeof(s16));
            s_carry_count = frame_total - space;
            memcpy(s_carry, frame_pcm + space,
                   s_carry_count * sizeof(s16));
            written = max_samples;
        }
    }

    s_wavbufs[idx].nsamples = (u32)(written / s_channels);
    DSP_FlushDataCache(s_ndsp_buf[idx], max_samples * sizeof(s16));
}

/* ── NDSP callback (runs on NDSP thread — keep tiny) ───────────── */

static void ndsp_callback(void *data)
{
    (void)data;
    LightEvent_Signal(&s_event);
}

/* ── Audio thread ──────────────────────────────────────────────── */

static void audio_thread_func(void *arg)
{
    (void)arg;

    /* Initial fill + queue */
    for (int i = 0; i < NUM_BUFS; i++) {
        fill_buffer(i);
        ndspChnWaveBufAdd(CHANNEL, &s_wavbufs[i]);
    }

    while (!s_thread_quit) {
        LightEvent_Wait(&s_event);
        LightEvent_Clear(&s_event);

        if (s_thread_quit) break;
        if (!s_enabled) continue;

        for (int i = 0; i < NUM_BUFS; i++) {
            if (s_wavbufs[i].status == NDSP_WBUF_DONE) {
                fill_buffer(i);
                ndspChnWaveBufAdd(CHANNEL, &s_wavbufs[i]);
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

void audio_init(const char *path)
{
    if (s_inited) return;

    /* Read entire MP3 file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    s_mp3_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_mp3_buf = malloc(s_mp3_size);
    if (!s_mp3_buf) { fclose(f); return; }
    if (fread(s_mp3_buf, 1, s_mp3_size, f) != s_mp3_size) {
        free(s_mp3_buf);
        s_mp3_buf = NULL;
        fclose(f);
        return;
    }
    fclose(f);

    /* Probe first frame for format info (pass NULL for PCM to skip
     * the heavy synthesis step — uses minimal stack) */
    mp3dec_init(&s_dec);
    s_mp3_pos     = 0;
    s_carry_count = 0;
    s_channels    = 0;
    s_sample_rate = 0;
    {
        size_t probe_pos = 0;
        while (probe_pos < s_mp3_size) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&s_dec, s_mp3_buf + probe_pos,
                                              (int)(s_mp3_size - probe_pos),
                                              NULL, &info);
            if (info.frame_bytes == 0) break;
            probe_pos += (size_t)info.frame_bytes;
            if (samples > 0) {
                s_channels    = info.channels;
                s_sample_rate = info.hz;
                break;
            }
        }
        mp3dec_init(&s_dec);
        s_mp3_pos = 0;
    }

    if (s_channels < 1)    s_channels    = 2;
    if (s_sample_rate < 1) s_sample_rate = 44100;

    /* Init NDSP */
    if (R_FAILED(ndspInit())) {
        free(s_mp3_buf);
        s_mp3_buf = NULL;
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(CHANNEL, (float)s_sample_rate);
    ndspChnSetFormat(CHANNEL,
        s_channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                        : NDSP_FORMAT_MONO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(CHANNEL, mix);

    /* Allocate linear NDSP buffers */
    size_t buf_bytes = (size_t)PCM_BUF_SAMPLES * s_channels * sizeof(s16);
    for (int i = 0; i < NUM_BUFS; i++) {
        s_ndsp_buf[i] = linearAlloc(buf_bytes);
        if (!s_ndsp_buf[i]) {
            for (int j = 0; j < i; j++) linearFree(s_ndsp_buf[j]);
            memset(s_ndsp_buf, 0, sizeof(s_ndsp_buf));
            ndspExit();
            free(s_mp3_buf);
            s_mp3_buf = NULL;
            return;
        }
        memset(s_ndsp_buf[i], 0, buf_bytes);
        memset(&s_wavbufs[i], 0, sizeof(ndspWaveBuf));
        s_wavbufs[i].data_vaddr = s_ndsp_buf[i];
        s_wavbufs[i].nsamples   = 0;
        s_wavbufs[i].looping    = false;
        s_wavbufs[i].status     = NDSP_WBUF_FREE;
    }

    s_enabled      = true;
    s_inited       = true;
    s_thread_quit  = false;

    LightEvent_Init(&s_event, RESET_ONESHOT);
    ndspSetCallback(ndsp_callback, NULL);

    /* Audio thread on core 1 with high priority */
    s_thread = threadCreate(audio_thread_func, NULL,
                            THREAD_STACK_SZ, 0x18, 1, false);
    if (!s_thread)
        s_thread = threadCreate(audio_thread_func, NULL,
                                THREAD_STACK_SZ, 0x18, -2, false);
}

void audio_tick(void)
{
    /* Decoding runs on a dedicated thread; this is a no-op. */
}

void audio_set_enabled(bool enabled)
{
    if (!s_inited) return;
    s_enabled = enabled;
    ndspChnSetPaused(CHANNEL, !enabled);
}

bool audio_get_enabled(void)
{
    return s_enabled;
}

void audio_exit(void)
{
    if (!s_inited) return;

    s_thread_quit = true;
    LightEvent_Signal(&s_event);
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }

    ndspSetCallback(NULL, NULL);
    ndspChnReset(CHANNEL);
    ndspExit();

    for (int i = 0; i < NUM_BUFS; i++) {
        if (s_ndsp_buf[i]) { linearFree(s_ndsp_buf[i]); s_ndsp_buf[i] = NULL; }
    }

    free(s_mp3_buf);
    s_mp3_buf = NULL;
    s_inited  = false;
}
