#ifndef LDMP_MPRIS_H
#define LDMP_MPRIS_H

#include "audio.h"
#include <stdbool.h>

/* Callbacks the MPRIS layer invokes when a remote controller (desktop
 * widget, waybar/eww/ags module, playerctl, media keys, etc.) sends a
 * Player method call. All are optional (pass NULL to ignore that
 * control). `userdata` is whatever was passed to mpris_init(). */
typedef struct {
    void (*play)(void *userdata);
    void (*pause)(void *userdata);
    void (*play_pause)(void *userdata);
    void (*next)(void *userdata);
    void (*previous)(void *userdata);
    void (*stop)(void *userdata);
    /* Relative seek, in seconds (may be negative). Covers both the
     * MPRIS Seek() call and SetPosition() (translated to a relative
     * offset from the current position). */
    void (*seek_relative)(void *userdata, double offset_seconds);
    /* Writable properties: a controller (LovelyRice, playerctl shuffle/loop, ...) set them. */
    void (*set_shuffle)(void *userdata, bool on);
    void (*set_repeat)(void *userdata, bool on);   /* ldmp's repeat = repeat the current track */
    void (*set_volume)(void *userdata, double volume);   /* 0.0 - 1.0 */
    void *userdata;
} mpris_callbacks_t;

/* Connects to the session bus and registers ldmp as an MPRIS2 player
 * (org.mpris.MediaPlayer2.ldmp, or a PID-suffixed name if another ldmp
 * instance already owns it). Returns 0 on success. If there's no
 * reachable session bus (headless box, no dbus-daemon, etc.) this
 * returns -1 and every other mpris_* call is a safe no-op — ldmp runs
 * exactly as before, it just won't show up in MPRIS-aware widgets. */
int mpris_init(const mpris_callbacks_t *callbacks);
void mpris_shutdown(void);

/* What ldmp tells MPRIS each tick. `title`, `artist` and `path` may be NULL
 * when nothing is loaded; `path` is the absolute file path of the current
 * track (exported as xesam:url, and used to find cover art for mpris:artUrl). */
typedef struct {
    const char *title;
    const char *artist;
    const char *path;
    double duration_seconds;
    player_state_t state;
    float volume;
    bool shuffle;
    bool repeat;
} mpris_state_t;

/* Call once per main-loop tick with the current playback state.
 * Pumps the D-Bus connection (services incoming method calls) and
 * emits PropertiesChanged signals whenever the track, playback status,
 * volume, shuffle or repeat actually changed since the previous call.
 * Cheap to call even when nothing changed. */
void mpris_tick(const mpris_state_t *state);

#endif
