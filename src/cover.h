#ifndef LDMP_COVER_H
#define LDMP_COVER_H

#include <stdbool.h>
#include <stddef.h>

/* Finds cover art for the audio file at `track_path`.
 *
 * Looks, in order, for
 *   1. a picture embedded in the file (ID3v2 APIC in .mp3, PICTURE block in .flac) -
 *      it is written once to ~/.cache/ldmp/covers/<content hash>.<ext>
 *      ($XDG_CACHE_HOME is honoured), so identical album art is stored only once;
 *   2. a cover/folder/front/album/albumart image (.jpg .jpeg .png .webp, any case)
 *      in the same folder as the track.
 *
 * On success writes the absolute path of an image file into `out` and returns true.
 * Never writes outside the cache directory; returns false when there is no art. */
bool cover_find(const char *track_path, char *out, size_t out_len);

/* Writes "file://" + the percent-encoded absolute `path` into `out` (for MPRIS
 * xesam:url / mpris:artUrl). Returns false if the buffer is too small. */
bool cover_path_to_url(const char *path, char *out, size_t out_len);

#endif
