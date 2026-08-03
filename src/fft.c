/* fft.c - minimal iterative radix-2 Cooley-Tukey FFT, real-valued input.
 * Small and dependency-free on purpose: we only need magnitude spectra
 * for the visualizer, not a general-purpose transform. */

#include "fft.h"
#include <math.h>
#include <string.h>

typedef struct { float re, im; } cplx;

static void fft_inplace(cplx *buf, int n) {
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            cplx tmp = buf[i];
            buf[i] = buf[j];
            buf[j] = tmp;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        cplx wlen = { (float)cos(ang), (float)sin(ang) };
        for (int i = 0; i < n; i += len) {
            cplx w = { 1.0f, 0.0f };
            for (int k = 0; k < len / 2; k++) {
                cplx u = buf[i + k];
                cplx v = {
                    buf[i + k + len / 2].re * w.re - buf[i + k + len / 2].im * w.im,
                    buf[i + k + len / 2].re * w.im + buf[i + k + len / 2].im * w.re
                };
                buf[i + k].re = u.re + v.re;
                buf[i + k].im = u.im + v.im;
                buf[i + k + len / 2].re = u.re - v.re;
                buf[i + k + len / 2].im = u.im - v.im;
                cplx nw = { w.re * wlen.re - w.im * wlen.im, w.re * wlen.im + w.im * wlen.re };
                w = nw;
            }
        }
    }
}

void fft_magnitude(const float *samples, int n, float *out_mag /* n/2 entries */) {
    static cplx buf[FFT_MAX_SIZE];
    if (n > FFT_MAX_SIZE) n = FFT_MAX_SIZE;
    for (int i = 0; i < n; i++) {
        /* Hann window to reduce spectral leakage */
        double w = 0.5 * (1.0 - cos(2.0 * M_PI * i / (n - 1)));
        buf[i].re = (float)(samples[i] * w);
        buf[i].im = 0.0f;
    }
    fft_inplace(buf, n);
    for (int i = 0; i < n / 2; i++) {
        out_mag[i] = sqrtf(buf[i].re * buf[i].re + buf[i].im * buf[i].im);
    }
}

/* Groups a linear magnitude spectrum into `bands` log-spaced buckets
 * (bass gets more resolution than treble, cava-style) and normalizes
 * against a slowly-decaying peak so the display auto-gains. */
void fft_bands_log(const float *mag, int mag_len, float sample_rate,
                    float *bands, int num_bands, float *running_peak) {
    float nyquist = sample_rate / 2.0f;
    float min_f = 30.0f;
    float max_f = nyquist * 0.95f;
    if (max_f <= min_f) max_f = min_f + 1.0f;

    float log_min = log10f(min_f);
    float log_max = log10f(max_f);

    for (int b = 0; b < num_bands; b++) {
        float f0 = powf(10.0f, log_min + (log_max - log_min) * b / num_bands);
        float f1 = powf(10.0f, log_min + (log_max - log_min) * (b + 1) / num_bands);
        int i0 = (int)(f0 / nyquist * mag_len);
        int i1 = (int)(f1 / nyquist * mag_len);
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > mag_len) i1 = mag_len;
        float sum = 0.0f;
        int count = 0;
        for (int i = i0; i < i1; i++) {
            sum += mag[i];
            count++;
        }
        bands[b] = count > 0 ? sum / count : 0.0f;
    }

    float frame_peak = 0.0f;
    for (int b = 0; b < num_bands; b++) {
        if (bands[b] > frame_peak) frame_peak = bands[b];
    }
    /* slow-decaying automatic gain, like cava's sensitivity smoothing */
    if (frame_peak > *running_peak) {
        *running_peak = frame_peak;
    } else {
        *running_peak = *running_peak * 0.985f + frame_peak * 0.015f;
    }
    /* Floor it here, not just in the local `peak` below: during a long
     * quiet/silent stretch this decay would otherwise keep dividing
     * running_peak by ~1.015 forever, walking it down through the
     * subnormal float range (< ~1.18e-38) before it ever gets near
     * true zero. Subnormal float math is 10-100x slower on most CPUs,
     * and this runs on the real-time audio thread every FFT window -
     * that's what turns "quiet part of the song" into audible stutter
     * that gets worse the longer the quiet section lasts. Clamping the
     * stored value (not just the local copy used for normalizing)
     * keeps it out of that range for good instead of only masking it
     * on this call. */
    if (*running_peak < 1e-6f) *running_peak = 1e-6f;
    float peak = *running_peak;
    for (int b = 0; b < num_bands; b++) {
        bands[b] = bands[b] / peak;
        if (bands[b] > 1.0f) bands[b] = 1.0f;
    }
}
