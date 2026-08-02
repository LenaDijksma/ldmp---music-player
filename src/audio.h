#ifndef LDMP_AUDIO_H
#define LDMP_AUDIO_H

#include <stdbool.h>

#define VIS_BANDS 32

typedef enum {
    PLAYER_STOPPED,
    PLAYER_PLAYING,
    PLAYER_PAUSED
} player_state_t;

int  audio_init(void);
void audio_shutdown(void);

/* Loads and starts playing a file immediately. Returns 0 on success. */
int  audio_play_file(const char *path);

void audio_toggle_pause(void);
void audio_stop(void);
void audio_seek_relative(double seconds);
void audio_set_volume(float vol);          /* 0.0 - 1.0 */
float audio_get_volume(void);

player_state_t audio_get_state(void);
double audio_get_position_seconds(void);
double audio_get_duration_seconds(void);

/* Returns true (once) if the current track played through to the end
 * since the last call. Poll this from the main loop each tick. */
bool audio_poll_and_clear_finished(void);

/* Thread-safe snapshot of the latest visualizer bands (0..1 each). */
void audio_get_spectrum(float out_bands[VIS_BANDS]);

#endif
