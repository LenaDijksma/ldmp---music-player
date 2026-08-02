/* library.c - recursive folder scan for audio files, plus a minimal
 * ID3v2 TIT2/TPE1 (title/artist) frame reader. This is intentionally
 * not a full tag library: it handles the common case (ID3v2.3/2.4,
 * text frames encoded as Latin-1 or UTF-8) and silently falls back to
 * the filename for anything it doesn't understand. */

#include "library.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

static bool has_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen) return false;
    return strcasecmp(name + nlen - elen, ext) == 0;
}

/* Case-insensitive comparison that treats runs of digits as numbers, so
 * "Rule #2" sorts before "Rule #10" instead of after it. */
static int natural_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *a_digits = a, *b_digits = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            size_t alen = (size_t)(a - a_digits), blen = (size_t)(b - b_digits);
            if (alen != blen) return alen < blen ? -1 : 1;
            int cmp = strncmp(a_digits, b_digits, alen);
            if (cmp != 0) return cmp;
            continue;
        }
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool is_audio_file(const char *name) {
    return has_ext(name, ".mp3") || has_ext(name, ".wav") || has_ext(name, ".flac");
}

/* Reads a 4-byte synchsafe (ID3v2.4) or plain (ID3v2.3) big-endian size. */
static unsigned read_frame_size(const unsigned char *b, bool synchsafe) {
    if (synchsafe) {
        return ((unsigned)(b[0] & 0x7F) << 21) | ((unsigned)(b[1] & 0x7F) << 14) |
               ((unsigned)(b[2] & 0x7F) << 7)  | (unsigned)(b[3] & 0x7F);
    }
    return ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | (unsigned)b[3];
}

static void copy_text_frame(const unsigned char *data, unsigned len, char *out, int out_len) {
    if (len == 0) { out[0] = '\0'; return; }
    unsigned char encoding = data[0];
    const unsigned char *text = data + 1;
    unsigned text_len = len - 1;
    int oi = 0;

    if (encoding == 1 || encoding == 2) {
        /* UTF-16 (with or without BOM) - do a crude downsample to ASCII
         * by taking every other byte. Good enough for display purposes. */
        unsigned start = 0;
        if (text_len >= 2 && ((text[0] == 0xFF && text[1] == 0xFE) || (text[0] == 0xFE && text[1] == 0xFF))) {
            start = 2;
        }
        bool little_endian = !(text_len >= 2 && text[0] == 0xFE && text[1] == 0xFF);
        for (unsigned i = start; i + 1 < text_len && oi < out_len - 1; i += 2) {
            unsigned char lo = little_endian ? text[i] : text[i + 1];
            unsigned char hi = little_endian ? text[i + 1] : text[i];
            if (hi == 0 && lo >= 0x20 && lo < 0x7F) {
                out[oi++] = (char)lo;
            } else if (hi != 0) {
                out[oi++] = '?';
            }
        }
    } else {
        /* Latin-1 or UTF-8: copy as-is (UTF-8 passes through fine for display) */
        for (unsigned i = 0; i < text_len && oi < out_len - 1; i++) {
            if (text[i] == 0) break;
            out[oi++] = (char)text[i];
        }
    }
    out[oi] = '\0';
    /* trim trailing whitespace */
    while (oi > 0 && (out[oi - 1] == ' ' || out[oi - 1] == '\t')) out[--oi] = '\0';
}

static void read_id3v2_tags(const char *path, char *title, int title_len, char *artist, int artist_len, bool *found_any) {
    *found_any = false;
    FILE *f = fopen(path, "rb");
    if (!f) return;

    unsigned char header[10];
    if (fread(header, 1, 10, f) != 10 || memcmp(header, "ID3", 3) != 0) {
        fclose(f);
        return;
    }
    unsigned major_version = header[3];
    unsigned tag_size = read_frame_size(header + 6, true);
    bool has_extended_header = (header[5] & 0x40) != 0;

    unsigned char *buf = (unsigned char *)malloc(tag_size);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, tag_size, f);
    fclose(f);
    if (got != tag_size) { free(buf); return; }

    unsigned pos = 0;
    if (has_extended_header && pos + 4 <= tag_size) {
        unsigned ext_size = read_frame_size(buf + pos, major_version >= 4);
        pos += (ext_size > 0 && ext_size < tag_size) ? ext_size : 0;
    }

    while (pos + 10 <= tag_size) {
        char frame_id[5] = { (char)buf[pos], (char)buf[pos + 1], (char)buf[pos + 2], (char)buf[pos + 3], 0 };
        if (frame_id[0] == 0) break; /* padding reached */
        unsigned frame_size = read_frame_size(buf + pos + 4, major_version >= 4);
        pos += 10;
        if (pos + frame_size > tag_size || frame_size == 0) break;

        if (strcmp(frame_id, "TIT2") == 0) {
            copy_text_frame(buf + pos, frame_size, title, title_len);
            *found_any = true;
        } else if (strcmp(frame_id, "TPE1") == 0) {
            copy_text_frame(buf + pos, frame_size, artist, artist_len);
            *found_any = true;
        }
        pos += frame_size;
    }
    free(buf);
}

static void add_track_from_path(library_t *lib, const char *full) {
    if (lib->count >= MAX_TRACKS) return;
    track_t *t = &lib->tracks[lib->count];
    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", full);
    if (has_ext(full, ".mp3")) {
        read_id3v2_tags(full, t->title, sizeof(t->title), t->artist, sizeof(t->artist), &t->has_tags);
    }
    lib->count++;
}

static void scan_dir_recursive(const char *folder, library_t *lib) {
    DIR *d = opendir(folder);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full[PATH_MAXLEN];
        snprintf(full, sizeof(full), "%s/%s", folder, entry->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(full, lib);
        } else if (S_ISREG(st.st_mode) && is_audio_file(entry->d_name)) {
            add_track_from_path(lib, full);
        }
    }
    closedir(d);
}

static int compare_tracks(const void *a, const void *b) {
    return natural_strcasecmp(((const track_t *)a)->path, ((const track_t *)b)->path);
}

void library_scan(const char *folder, library_t *lib) {
    lib->count = 0;
    scan_dir_recursive(folder, lib);
    qsort(lib->tracks, lib->count, sizeof(track_t), compare_tracks);
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void library_display_name(const track_t *t, char *out, int out_len) {
    if (t->has_tags && t->title[0] != '\0') {
        if (t->artist[0] != '\0') {
            snprintf(out, out_len, "%s - %s", t->artist, t->title);
        } else {
            snprintf(out, out_len, "%s", t->title);
        }
    } else {
        snprintf(out, out_len, "%s", basename_of(t->path));
    }
}

static int compare_browse_entries(const void *a, const void *b) {
    const browse_entry_t *ea = (const browse_entry_t *)a;
    const browse_entry_t *eb = (const browse_entry_t *)b;
    if (ea->type != eb->type) return ea->type == BROWSE_FOLDER ? -1 : 1;
    return natural_strcasecmp(ea->name, eb->name);
}

int library_list_folder(const library_t *lib, const char *folder, browse_entry_t *out, int max_entries) {
    int count = 0;
    size_t flen = strlen(folder);
    /* Normalize away a trailing slash so prefix matching below is exact. */
    while (flen > 1 && folder[flen - 1] == '/') flen--;

    for (int i = 0; i < lib->count && count < max_entries; i++) {
        const char *path = lib->tracks[i].path;
        size_t plen = strlen(path);
        if (plen <= flen + 1) continue;
        if (strncmp(path, folder, flen) != 0 || path[flen] != '/') continue;
        const char *rest = path + flen + 1;

        const char *slash = strchr(rest, '/');
        if (slash) {
            /* Subfolder: dedupe by comparing the child folder path. */
            size_t child_len = (size_t)(slash - rest);
            char child_path[PATH_MAXLEN];
            snprintf(child_path, sizeof(child_path), "%.*s", (int)flen, folder);
            snprintf(child_path + flen, sizeof(child_path) - flen, "/%.*s", (int)child_len, rest);

            bool seen = false;
            for (int j = 0; j < count; j++) {
                if (out[j].type == BROWSE_FOLDER && strcmp(out[j].path, child_path) == 0) { seen = true; break; }
            }
            if (seen) continue;

            browse_entry_t *e = &out[count++];
            e->type = BROWSE_FOLDER;
            snprintf(e->name, sizeof(e->name), "%.*s", (int)child_len, rest);
            snprintf(e->path, sizeof(e->path), "%s", child_path);
            e->track_idx = -1;
        } else {
            browse_entry_t *e = &out[count++];
            e->type = BROWSE_TRACK;
            library_display_name(&lib->tracks[i], e->name, sizeof(e->name));
            snprintf(e->path, sizeof(e->path), "%s", path);
            e->track_idx = i;
        }
    }

    qsort(out, count, sizeof(browse_entry_t), compare_browse_entries);
    return count;
}

int library_find_or_add_track(library_t *lib, const char *path) {
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->tracks[i].path, path) == 0) return i;
    }
    if (lib->count >= MAX_TRACKS) return -1;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    int idx = lib->count;
    add_track_from_path(lib, path);
    return (lib->count > idx) ? idx : -1;
}
