/* audio.c - thin wrapper around miniaudio for decode+playback, plus a
 * real-time spectrum analyzer computed straight from the PCM frames as
 * they're mixed to the output device (cava-style: analyze what's
 * actually playing, not a pre-decoded copy).
 *
 * Decoding runs ahead of playback on a dedicated background thread into
 * a small ring buffer. The real-time audio callback (data_callback)
 * only ever copies already-decoded/resampled float32 frames out of that
 * ring buffer - it never calls into the decoder itself. This matters
 * because ma_decoder_read_pcm_frames() can be expensive and *variable*
 * in cost: any file whose native sample rate doesn't match the output
 * device (44.1kHz) gets resampled on read, and that resampling work
 * used to happen synchronously inside the callback. On a slow enough
 * file that could blow the callback's real-time deadline and cause an
 * underrun - audible as a stutter/glitch, or as total freezing if it
 * happens repeatedly. Moving decode+resample to a background thread
 * means the callback's cost is now just a memcpy, regardless of the
 * source file's format/sample rate/bitrate. */

#include "audio.h"
#include "fft.h"
#include "miniaudio.h"

#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#define FFT_SIZE 2048  /* power of 2 */

/* Decode-ahead ring buffer. 8192 frames is ~185ms at 44.1kHz - plenty of
 * slack to absorb scheduling jitter on the decode thread (or an
 * expensive resample) without ever starving the audio callback, while
 * staying small enough that seek/track-change flushes feel instant and
 * the visualizer (which reads post-ring, in the callback) doesn't lag
 * noticeably behind the audio. */
#define RING_FRAMES  8192
#define RING_CHANNELS 2   /* matches cfg.playback.channels in audio_init */
#define DECODE_CHUNK 1024 /* frames decoded per background-thread iteration */

static ma_decoder   g_decoder;
static ma_device    g_device;
static bool         g_decoder_loaded = false;
static bool         g_device_started = false;
static atomic_int   g_state = PLAYER_STOPPED; /* player_state_t */
static atomic_bool  g_finished_flag = false;
static atomic_bool  g_decoder_eof = false; /* decode thread hit MA_AT_END */
static _Atomic float g_volume = 0.8f;

/* g_decoder is touched from two threads: the decode thread (reads PCM
 * frames continuously) and the main thread (swaps decoders on track
 * change, seeks, reads position/length). miniaudio decoders aren't safe
 * to use concurrently from multiple threads without this - flipping
 * g_state alone is not enough, since the decode thread can already be
 * mid-read when the main thread frees/reinits the decoder. Note this is
 * no longer touched by the real-time audio callback at all, so
 * contention here can never cause an audio dropout - only the decode
 * thread's throughput (which runs well ahead of playback) is affected. */
static ma_mutex g_decoder_mutex;

/* Guards g_window/g_window_pos/g_running_peak, which the audio callback
 * writes continuously and audio_play_file resets on track change. */
static ma_mutex g_window_mutex;

/* Rolling mono window feeding the FFT, filled by the audio callback. */
static float  g_window[FFT_SIZE];
static int    g_window_pos = 0;
static float  g_running_peak = 1.0f;

static float  g_bands[VIS_BANDS];
static ma_mutex g_bands_mutex;

/* Decode-ahead ring buffer: interleaved stereo float32, produced by the
 * decode thread and consumed by the real-time audio callback. Indices
 * are monotonically increasing frame counts (not wrapped) so "available
 * frames" is always just write - read; g_ring_mutex is only ever held
 * for a bounded memcpy-sized critical section on either side, so it's
 * cheap enough to take from the callback (unlike g_decoder_mutex, which
 * can be held for as long as a decode+resample call takes). */
static float    g_ring[RING_FRAMES * RING_CHANNELS];
static size_t   g_ring_write = 0;
static size_t   g_ring_read  = 0;
static ma_mutex g_ring_mutex;

static pthread_t   g_decode_thread;
static bool         g_decode_thread_started = false;
static atomic_bool  g_decode_thread_run = false;

static void ring_flush(void) {
    ma_mutex_lock(&g_ring_mutex);
    g_ring_write = 0;
    g_ring_read  = 0;
    ma_mutex_unlock(&g_ring_mutex);
}

static void *decode_thread_main(void *arg) {
    (void)arg;
    float chunk[DECODE_CHUNK * RING_CHANNELS];

    while (atomic_load(&g_decode_thread_run)) {
        if (atomic_load(&g_state) != PLAYER_PLAYING) {
            usleep(10 * 1000);
            continue;
        }

        ma_mutex_lock(&g_ring_mutex);
        size_t used = g_ring_write - g_ring_read;
        size_t free_frames = RING_FRAMES - used;
        ma_mutex_unlock(&g_ring_mutex);

        if (free_frames == 0) {
            usleep(5 * 1000);
            continue;
        }

        ma_uint64 want = free_frames < DECODE_CHUNK ? free_frames : DECODE_CHUNK;

        ma_mutex_lock(&g_decoder_mutex);
        ma_uint64 framesRead = 0;
        ma_result r = MA_ERROR;
        if (g_decoder_loaded) {
            r = ma_decoder_read_pcm_frames(&g_decoder, chunk, want, &framesRead);
        }
        ma_mutex_unlock(&g_decoder_mutex);

        if (framesRead == 0) {
            /* Nothing to decode right now - either genuine end of track,
             * or the decoder was just swapped out from under us on a
             * track change/seek. Avoid busy-spinning either way. */
            if (r == MA_AT_END) {
                atomic_store(&g_decoder_eof, true);
            }
            usleep(10 * 1000);
            continue;
        }

        ma_mutex_lock(&g_ring_mutex);
        for (ma_uint64 i = 0; i < framesRead; i++) {
            size_t idx = (g_ring_write + i) % RING_FRAMES;
            g_ring[idx * RING_CHANNELS + 0] = chunk[i * RING_CHANNELS + 0];
            g_ring[idx * RING_CHANNELS + 1] = chunk[i * RING_CHANNELS + 1];
        }
        g_ring_write += framesRead;
        ma_mutex_unlock(&g_ring_mutex);
    }
    return NULL;
}

static void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
    (void)pInput;
    float *out = (float *)pOutput;
    ma_uint32 channels = pDevice->playback.channels;

    if (atomic_load(&g_state) != PLAYER_PLAYING) {
        memset(out, 0, frameCount * channels * sizeof(float));
        return;
    }

    /* Pull already-decoded/resampled frames out of the ring buffer. This
     * critical section is just a bounded memcpy-equivalent loop - no
     * decode, no resample, no syscalls - so it's safe to do under a
     * mutex from the real-time thread. */
    ma_mutex_lock(&g_ring_mutex);
    size_t available = g_ring_write - g_ring_read;
    ma_uint64 framesRead = available < frameCount ? available : frameCount;
    for (ma_uint64 i = 0; i < framesRead; i++) {
        size_t idx = (g_ring_read + i) % RING_FRAMES;
        out[i * channels + 0] = g_ring[idx * RING_CHANNELS + 0];
        if (channels > 1) out[i * channels + 1] = g_ring[idx * RING_CHANNELS + 1];
    }
    g_ring_read += framesRead;
    ma_mutex_unlock(&g_ring_mutex);

    float vol = atomic_load(&g_volume);
    for (ma_uint64 i = 0; i < framesRead * channels; i++) {
        out[i] *= vol;
    }

    if (framesRead < frameCount) {
        /* Ring buffer ran dry -> silence the remainder. If the decode
         * thread already hit end-of-file *and* we've drained everything
         * it produced, that's a genuine end of track. If it's just
         * temporarily behind (e.g. first few callbacks after a track
         * change), g_decoder_eof won't be set yet, so we correctly don't
         * flag "finished" - we just get a very brief silence gap instead
         * of the old (worse) failure mode of a stalled callback. */
        memset(out + framesRead * channels, 0, (frameCount - framesRead) * channels * sizeof(float));
        if (framesRead == 0 && atomic_load(&g_decoder_eof)) {
            atomic_store(&g_finished_flag, true);
            atomic_store(&g_decoder_eof, false);
        }
    }

    /* Feed mono-summed samples into the rolling FFT window. g_window_pos
     * is also reset by audio_play_file on track change, so it needs the
     * same lock - a dedicated one, not g_ring_mutex/g_decoder_mutex. */
    ma_mutex_lock(&g_window_mutex);
    for (ma_uint64 i = 0; i < framesRead; i++) {
        float mono = 0.0f;
        for (ma_uint32 c = 0; c < channels; c++) mono += out[i * channels + c];
        mono /= (float)channels;
        g_window[g_window_pos++] = mono;
        if (g_window_pos >= FFT_SIZE) {
            g_window_pos = 0;
            float mag[FFT_SIZE / 2];
            fft_magnitude(g_window, FFT_SIZE, mag);
            float bands[VIS_BANDS];
            fft_bands_log(mag, FFT_SIZE / 2, (float)pDevice->sampleRate, bands, VIS_BANDS, &g_running_peak);
            ma_mutex_lock(&g_bands_mutex);
            memcpy(g_bands, bands, sizeof(bands));
            ma_mutex_unlock(&g_bands_mutex);
        }
    }
    ma_mutex_unlock(&g_window_mutex);
}

int audio_init(void) {
    ma_mutex_init(&g_bands_mutex);
    ma_mutex_init(&g_decoder_mutex);
    ma_mutex_init(&g_window_mutex);
    ma_mutex_init(&g_ring_mutex);
    memset(g_bands, 0, sizeof(g_bands));
    memset(g_window, 0, sizeof(g_window));
    memset(g_ring, 0, sizeof(g_ring));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = RING_CHANNELS;
    cfg.sampleRate        = 44100;
    cfg.dataCallback      = data_callback;

    if (ma_device_init(NULL, &cfg, &g_device) != MA_SUCCESS) {
        return -1;
    }
    if (ma_device_start(&g_device) != MA_SUCCESS) {
        ma_device_uninit(&g_device);
        return -1;
    }
    g_device_started = true;

    atomic_store(&g_decode_thread_run, true);
    if (pthread_create(&g_decode_thread, NULL, decode_thread_main, NULL) != 0) {
        atomic_store(&g_decode_thread_run, false);
        ma_device_uninit(&g_device);
        g_device_started = false;
        return -1;
    }
    g_decode_thread_started = true;

    return 0;
}

void audio_shutdown(void) {
    if (g_decode_thread_started) {
        atomic_store(&g_decode_thread_run, false);
        pthread_join(g_decode_thread, NULL);
        g_decode_thread_started = false;
    }
    if (g_device_started) {
        ma_device_uninit(&g_device);
        g_device_started = false;
    }
    ma_mutex_lock(&g_decoder_mutex);
    if (g_decoder_loaded) {
        ma_decoder_uninit(&g_decoder);
        g_decoder_loaded = false;
    }
    ma_mutex_unlock(&g_decoder_mutex);
    ma_mutex_uninit(&g_decoder_mutex);
    ma_mutex_uninit(&g_window_mutex);
    ma_mutex_uninit(&g_bands_mutex);
    ma_mutex_uninit(&g_ring_mutex);
}

int audio_play_file(const char *path) {
    ma_decoder new_decoder;
    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, g_device.playback.channels, g_device.sampleRate);
    if (ma_decoder_init_file(path, &dcfg, &new_decoder) != MA_SUCCESS) {
        return -1;
    }

    ma_mutex_lock(&g_decoder_mutex);
    atomic_store(&g_state, PLAYER_STOPPED); /* belt-and-suspenders: skip the callback/decode-thread fast-path too */
    if (g_decoder_loaded) {
        ma_decoder_uninit(&g_decoder);
    }
    g_decoder = new_decoder;
    g_decoder_loaded = true;
    atomic_store(&g_finished_flag, false);
    atomic_store(&g_decoder_eof, false);
    atomic_store(&g_state, PLAYER_PLAYING);
    ma_mutex_unlock(&g_decoder_mutex);

    /* Drop anything the decode thread had queued up for the previous
     * track so playback of the new one starts immediately instead of
     * finishing out a stale buffer. */
    ring_flush();

    ma_mutex_lock(&g_window_mutex);
    g_window_pos = 0;
    g_running_peak = 1.0f;
    ma_mutex_unlock(&g_window_mutex);
    return 0;
}

void audio_toggle_pause(void) {
    int s = atomic_load(&g_state);
    if (s == PLAYER_PLAYING) {
        atomic_store(&g_state, PLAYER_PAUSED);
    } else if (s == PLAYER_PAUSED) {
        atomic_store(&g_state, PLAYER_PLAYING);
    }
}

void audio_stop(void) {
    atomic_store(&g_state, PLAYER_STOPPED);
    ma_mutex_lock(&g_decoder_mutex);
    if (g_decoder_loaded) {
        ma_decoder_seek_to_pcm_frame(&g_decoder, 0);
    }
    ma_mutex_unlock(&g_decoder_mutex);
    ring_flush();
}

void audio_seek_relative(double seconds) {
    ma_mutex_lock(&g_decoder_mutex);
    if (!g_decoder_loaded) { ma_mutex_unlock(&g_decoder_mutex); return; }
    ma_uint64 cursor = 0, len = 0;
    ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cursor);
    ma_decoder_get_length_in_pcm_frames(&g_decoder, &len);
    ma_int64 target = (ma_int64)cursor + (ma_int64)(seconds * g_device.sampleRate);
    if (target < 0) target = 0;
    if (len > 0 && (ma_uint64)target > len) target = (ma_int64)len - 1;
    ma_decoder_seek_to_pcm_frame(&g_decoder, (ma_uint64)target);
    atomic_store(&g_decoder_eof, false);
    ma_mutex_unlock(&g_decoder_mutex);

    /* Without this the ring buffer would still hold up to ~185ms of
     * frames decoded from the pre-seek position, so the audible seek
     * would appear to "lag" until that stale audio finished playing. */
    ring_flush();
}

void audio_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    atomic_store(&g_volume, vol);
}

float audio_get_volume(void) {
    return atomic_load(&g_volume);
}

player_state_t audio_get_state(void) {
    return (player_state_t)atomic_load(&g_state);
}

double audio_get_position_seconds(void) {
    ma_mutex_lock(&g_decoder_mutex);
    if (!g_decoder_loaded) { ma_mutex_unlock(&g_decoder_mutex); return 0.0; }
    ma_uint64 cursor = 0;
    ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cursor);
    ma_mutex_unlock(&g_decoder_mutex);

    /* The decoder cursor reflects how far the decode thread has read
     * *ahead*, not what's actually audible right now - it can be up to
     * RING_FRAMES frames in the future. Subtract what's still sitting in
     * the ring buffer, unplayed, to report the true audible position. */
    ma_mutex_lock(&g_ring_mutex);
    size_t buffered = g_ring_write - g_ring_read;
    ma_mutex_unlock(&g_ring_mutex);

    ma_uint64 audible_cursor = ((ma_uint64)buffered < cursor) ? (cursor - buffered) : 0;
    return (double)audible_cursor / (double)g_device.sampleRate;
}

double audio_get_duration_seconds(void) {
    ma_mutex_lock(&g_decoder_mutex);
    if (!g_decoder_loaded) { ma_mutex_unlock(&g_decoder_mutex); return 0.0; }
    ma_uint64 len = 0;
    ma_decoder_get_length_in_pcm_frames(&g_decoder, &len);
    ma_mutex_unlock(&g_decoder_mutex);
    return (double)len / (double)g_device.sampleRate;
}

bool audio_poll_and_clear_finished(void) {
    bool expected = true;
    return atomic_compare_exchange_strong(&g_finished_flag, &expected, false);
}

void audio_get_spectrum(float out_bands[VIS_BANDS]) {
    ma_mutex_lock(&g_bands_mutex);
    memcpy(out_bands, g_bands, sizeof(g_bands));
    ma_mutex_unlock(&g_bands_mutex);
    if (atomic_load(&g_state) != PLAYER_PLAYING) {
        /* decay bars towards zero while paused/stopped so they don't freeze mid-peak */
        for (int i = 0; i < VIS_BANDS; i++) out_bands[i] *= 0.0f;
    }
}
