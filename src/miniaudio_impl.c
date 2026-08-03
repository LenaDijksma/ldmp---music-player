/* miniaudio_impl.c - compiles the miniaudio implementation exactly once.
 *
 * By default MA_ASSERT() calls assert(), which aborts the whole process
 * on failure. Most of these checks guard "should never happen" internal
 * invariants that already have a well-defined error-return fallback right
 * after them (e.g. ma_linear_resampler_process_pcm_frames returns
 * MA_INVALID_ARGS either way) - so on a rare internal edge case we'd
 * rather log it and let that one read/frame fail gracefully than crash
 * an otherwise-fine multi-hour listening session. Must be defined before
 * including miniaudio.h, which only applies its own assert() definition
 * if MA_ASSERT isn't already defined. Logs to a file rather than stderr
 * so it can't garble the ncurses display if it ever fires mid-session.
 *
 * This can fire from the real-time audio callback thread, potentially
 * on every single buffer if the same underlying condition keeps
 * recurring - so the file write is capped at once per process run.
 * Blocking disk I/O on every occurrence would turn a rare internal
 * hiccup into exactly the kind of sustained audio stutter we're trying
 * to avoid by not aborting in the first place. */
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

static atomic_flag g_assert_logged = ATOMIC_FLAG_INIT;

static void ldmp_ma_assert_log(const char *condition, const char *file, int line) {
    if (atomic_flag_test_and_set(&g_assert_logged)) {
        return; /* already logged once this run - skip, no I/O, real-time-safe */
    }
    const char *home = getenv("HOME");
    char path[512];
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.ldmp_audio_warnings.log", home);
    } else {
        snprintf(path, sizeof(path), "/tmp/.ldmp_audio_warnings.log");
    }
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "[ldmp] miniaudio internal check failed: %s (%s:%d)\n", condition, file, line);
        fclose(f);
    }
}

#define MA_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            ldmp_ma_assert_log(#condition, __FILE__, __LINE__); \
        } \
    } while (0)

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
