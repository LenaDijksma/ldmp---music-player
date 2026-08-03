/* audio.c - thin wrapper around miniaudio for decode+playback, plus a
 * real-time spectrum analyzer computed straight from the PCM frames as
 * they're mixed to the output device (cava-style: analyze what's
 * actually playing, not a pre-decoded copy). */

#include "audio.h"
#include "fft.h"
#include "miniaudio.h"

#include <string.h>
#include <stdatomic.h>

#define FFT_SIZE 2048  /* power of 2 */

static ma_decoder   g_decoder;
static ma_device    g_device;
static bool         g_decoder_loaded = false;
static bool         g_device_started = false;
static atomic_int   g_state = PLAYER_STOPPED; /* player_state_t */
static atomic_bool  g_finished_flag = false;
static _Atomic float g_volume = 0.8f;

/* g_decoder is touched from two threads: the real-time audio callback
 * (reads PCM frames every buffer) and the main thread (swaps decoders
 * on track change, seeks, reads position/length). miniaudio decoders
 * aren't safe to use concurrently from multiple threads without this -
 * flipping g_state alone is not enough, since the callback can already
 * be mid-read when the main thread frees/reinits the decoder. Kept
 * separate from g_window_mutex below so the UI thread's frequent (every
 * draw frame) position/duration polls can never make the audio thread
 * wait behind FFT work, or vice versa - both threads should only ever
 * hold either lock for a very short, bounded time. */
static ma_mutex g_decoder_mutex;

/* Guards g_window/g_window_pos/g_running_peak, which the audio callback
 * writes continuously and audio_play_file resets on track change. Not
 * g_decoder_mutex - that mutex needs to stay cheap and rarely contended
 * since the UI thread grabs it on every draw frame just to read the
 * playback position. */
static ma_mutex g_window_mutex;

/* Rolling mono window feeding the FFT, filled by the audio callback. */
static float  g_window[FFT_SIZE];
static int    g_window_pos = 0;
static float  g_running_peak = 1.0f;

static float  g_bands[VIS_BANDS];
static ma_mutex g_bands_mutex;

static void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
    (void)pInput;
    float *out = (float *)pOutput;
    ma_uint32 channels = pDevice->playback.channels;

    ma_mutex_lock(&g_decoder_mutex);
    if (!g_decoder_loaded || atomic_load(&g_state) != PLAYER_PLAYING) {
        ma_mutex_unlock(&g_decoder_mutex);
        memset(out, 0, frameCount * channels * sizeof(float));
        return;
    }

    ma_uint64 framesRead = 0;
    ma_result r = ma_decoder_read_pcm_frames(&g_decoder, out, frameCount, &framesRead);
    ma_mutex_unlock(&g_decoder_mutex);

    float vol = atomic_load(&g_volume);
    for (ma_uint64 i = 0; i < framesRead * channels; i++) {
        out[i] *= vol;
    }
    if (framesRead < frameCount) {
        /* ran out of frames -> silence the remainder, flag end-of-track */
        memset(out + framesRead * channels, 0, (frameCount - framesRead) * channels * sizeof(float));
        if (r == MA_AT_END || framesRead == 0) {
            atomic_store(&g_finished_flag, true);
        }
    }

    /* Feed mono-summed samples into the rolling FFT window. g_window_pos
     * is also reset by audio_play_file on track change, so it needs the
     * same lock - a dedicated one, not g_decoder_mutex (see comment up
     * top on why that one needs to stay cheap). */
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
    memset(g_bands, 0, sizeof(g_bands));
    memset(g_window, 0, sizeof(g_window));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
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
    return 0;
}

void audio_shutdown(void) {
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
}

int audio_play_file(const char *path) {
    ma_decoder new_decoder;
    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, g_device.playback.channels, g_device.sampleRate);
    if (ma_decoder_init_file(path, &dcfg, &new_decoder) != MA_SUCCESS) {
        return -1;
    }

    ma_mutex_lock(&g_decoder_mutex);
    atomic_store(&g_state, PLAYER_STOPPED); /* belt-and-suspenders: skip the callback fast-path too */
    if (g_decoder_loaded) {
        ma_decoder_uninit(&g_decoder);
    }
    g_decoder = new_decoder;
    g_decoder_loaded = true;
    atomic_store(&g_finished_flag, false);
    atomic_store(&g_state, PLAYER_PLAYING);
    ma_mutex_unlock(&g_decoder_mutex);

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
    ma_mutex_unlock(&g_decoder_mutex);
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
    return (double)cursor / (double)g_device.sampleRate;
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
