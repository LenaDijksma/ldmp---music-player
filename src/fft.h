#ifndef LDMP_FFT_H
#define LDMP_FFT_H

#define FFT_MAX_SIZE 4096

/* Computes magnitude spectrum of `n` real samples (n must be a power of
 * two, <= FFT_MAX_SIZE). Writes n/2 magnitude values to out_mag. */
void fft_magnitude(const float *samples, int n, float *out_mag);

/* Buckets a linear magnitude spectrum (mag_len = fft_size/2 bins,
 * covering 0..sample_rate/2 Hz) into `num_bands` log-spaced bands,
 * normalized 0..1 against a running peak (auto-gain, cava-style). */
void fft_bands_log(const float *mag, int mag_len, float sample_rate,
                    float *bands, int num_bands, float *running_peak);

#endif
