#ifndef LDMP_LIBRARY_H
#define LDMP_LIBRARY_H

#include <stdbool.h>

#define MAX_TRACKS 4096
#define PATH_MAXLEN 1024
#define TAG_MAXLEN 256
#define MAX_BROWSE_ENTRIES 512

typedef struct {
    char path[PATH_MAXLEN];
    char title[TAG_MAXLEN];
    char artist[TAG_MAXLEN];
    bool has_tags;
} track_t;

typedef struct {
    track_t tracks[MAX_TRACKS];
    int count;
} library_t;

typedef enum {
    BROWSE_FOLDER,
    BROWSE_TRACK
} browse_entry_type_t;

typedef struct {
    browse_entry_type_t type;
    char name[TAG_MAXLEN];   /* display name: folder name, or track display name */
    char path[PATH_MAXLEN];  /* full path: folder path, or track file path */
    int track_idx;           /* valid when type == BROWSE_TRACK: index into lib.tracks */
} browse_entry_t;

/* Recursively scans `folder` for supported audio files (.mp3 .wav .flac,
 * case-insensitive) and fills `lib`, sorted alphabetically (case-insensitive)
 * by full path. Attempts a light ID3v2 title/artist read for .mp3 files;
 * everything else just uses the filename. */
void library_scan(const char *folder, library_t *lib);

/* Returns a nice display string ("Artist - Title", or the bare filename
 * if no tags were found) into `out` (caller-provided buffer). */
void library_display_name(const track_t *t, char *out, int out_len);

/* Lists the immediate contents of `folder` (which must be `lib`'s scan
 * root, or a folder somewhere beneath it): subfolders that contain at
 * least one audio file (directly or nested), followed by audio files
 * that live directly inside `folder`. Each group is sorted alphabetically
 * (case-insensitive). Returns the number of entries written to `out`
 * (capped at max_entries). */
int library_list_folder(const library_t *lib, const char *folder, browse_entry_t *out, int max_entries);

/* Finds `path` in `lib` by exact match, or appends it (reading tags if
 * it's an mp3) if there's room and the file still exists on disk.
 * Used to resolve playlist entries that may point outside the folder
 * that was scanned at startup. Returns the track's index, or -1 if the
 * file is missing or the library is full. */
int library_find_or_add_track(library_t *lib, const char *path);

#endif
