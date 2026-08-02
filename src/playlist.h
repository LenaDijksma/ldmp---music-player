#ifndef LDMP_PLAYLIST_H
#define LDMP_PLAYLIST_H

#include "library.h"
#include <stdbool.h>

#define PLAYLIST_NAME_MAXLEN 128
#define MAX_PLAYLIST_TRACKS 512
#define MAX_PLAYLISTS 64

/* A playlist is just a name plus a list of absolute file paths - it
 * never copies audio, only references it. Stored on disk as one JSON
 * file per playlist under ~/.config/ldmp/playlists/<name>.json:
 *
 *   { "name": "Road Trip", "tracks": ["/home/lena/Music/a.mp3", ...] }
 */
typedef struct {
    char name[PLAYLIST_NAME_MAXLEN];
    char file_path[PATH_MAXLEN];   /* the backing .json file */
    char tracks[MAX_PLAYLIST_TRACKS][PATH_MAXLEN];
    int track_count;
} playlist_t;

/* Fills `out` with the playlist storage directory
 * (~/.config/ldmp/playlists, or $XDG_CONFIG_HOME/ldmp/playlists),
 * creating it and its parents if needed. */
void playlist_dir(char *out, int out_len);

/* Loads every *.json playlist found in the playlist directory into
 * `out` (capped at max_playlists), sorted alphabetically by name.
 * Returns the number loaded. */
int playlist_list_all(playlist_t *out, int max_playlists);

/* Loads a single playlist from its .json file. Returns false on error
 * (missing file, unreadable, or no "name" field found). */
bool playlist_load(const char *file_path, playlist_t *out);

/* (Re)writes `pl` to pl->file_path as JSON. Returns false on error. */
bool playlist_save(const playlist_t *pl);

/* Creates a new, empty playlist file named after `name` (sanitized for
 * the filesystem) and fills `out`. Returns false if the name is empty
 * or a playlist with that (sanitized) name already exists. */
bool playlist_create(const char *name, playlist_t *out);

/* Appends `track_path` to `pl` (no-op, still returns true, if it's
 * already in there) and saves. Returns false if the playlist is full
 * or the save fails. */
bool playlist_add_track(playlist_t *pl, const char *track_path);

/* Removes the track at `index` from `pl` and saves. */
bool playlist_remove_track(playlist_t *pl, int index);

/* Deletes `pl`'s backing .json file from disk. Does not touch the
 * audio files it referenced. */
bool playlist_delete(const playlist_t *pl);

#endif
