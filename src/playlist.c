/* playlist.c - playlists are just a name + a list of file paths
 * (references, never copies), persisted as one small JSON file per
 * playlist. This hand-rolls just enough JSON to read back exactly
 * what it writes; it's not a general-purpose parser. */

#include "playlist.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void playlist_dir(char *out, int out_len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char base[PATH_MAXLEN];
    if (xdg && xdg[0]) {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = getenv("HOME");
        snprintf(base, sizeof(base), "%s/.config", home ? home : ".");
    }
    char ldmp_dir[PATH_MAXLEN];
    snprintf(ldmp_dir, sizeof(ldmp_dir), "%s/ldmp", base);

    mkdir(base, 0755);
    mkdir(ldmp_dir, 0755);
    snprintf(out, out_len, "%s/playlists", ldmp_dir);
    mkdir(out, 0755);
}

static void sanitize_filename(const char *name, char *out, int out_len) {
    int oi = 0;
    for (const char *p = name; *p && oi < out_len - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == ' ') {
            out[oi++] = (char)c;
        } else {
            out[oi++] = '_';
        }
    }
    out[oi] = '\0';
    while (oi > 0 && out[oi - 1] == ' ') out[--oi] = '\0';
    int start = 0;
    while (out[start] == ' ') start++;
    if (start > 0) memmove(out, out + start, strlen(out + start) + 1);
}

static void write_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}

bool playlist_save(const playlist_t *pl) {
    FILE *f = fopen(pl->file_path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"name\": ");
    write_json_string(f, pl->name);
    fprintf(f, ",\n  \"tracks\": [\n");
    for (int i = 0; i < pl->track_count; i++) {
        fprintf(f, "    ");
        write_json_string(f, pl->tracks[i]);
        fprintf(f, "%s\n", (i < pl->track_count - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return true;
}

bool playlist_create(const char *name, playlist_t *out) {
    while (*name == ' ') name++;
    if (*name == '\0') return false;

    char dir[PATH_MAXLEN];
    playlist_dir(dir, sizeof(dir));

    char safe[PLAYLIST_NAME_MAXLEN];
    sanitize_filename(name, safe, sizeof(safe));
    if (safe[0] == '\0') return false;

    char file_path[PATH_MAXLEN];
    snprintf(file_path, sizeof(file_path), "%s/%s.json", dir, safe);

    struct stat st;
    if (stat(file_path, &st) == 0) return false; /* already exists */

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->file_path, sizeof(out->file_path), "%s", file_path);
    out->track_count = 0;
    return playlist_save(out);
}

bool playlist_add_track(playlist_t *pl, const char *track_path) {
    for (int i = 0; i < pl->track_count; i++) {
        if (strcmp(pl->tracks[i], track_path) == 0) return true;
    }
    if (pl->track_count >= MAX_PLAYLIST_TRACKS) return false;
    snprintf(pl->tracks[pl->track_count], sizeof(pl->tracks[0]), "%s", track_path);
    pl->track_count++;
    return playlist_save(pl);
}

bool playlist_remove_track(playlist_t *pl, int index) {
    if (index < 0 || index >= pl->track_count) return false;
    int remaining = pl->track_count - index - 1;
    if (remaining > 0) {
        memmove(pl->tracks[index], pl->tracks[index + 1], (size_t)remaining * sizeof(pl->tracks[0]));
    }
    pl->track_count--;
    return playlist_save(pl);
}

bool playlist_delete(const playlist_t *pl) {
    return remove(pl->file_path) == 0;
}

static bool playlist_has_track(const playlist_t *pl, const char *track_path) {
    for (int i = 0; i < pl->track_count; i++) {
        if (strcmp(pl->tracks[i], track_path) == 0) return true;
    }
    return false;
}

int playlist_add_paths(playlist_t *pl, char paths[][PATH_MAXLEN], int count) {
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (pl->track_count >= MAX_PLAYLIST_TRACKS) break;
        if (playlist_has_track(pl, paths[i])) continue;
        snprintf(pl->tracks[pl->track_count], sizeof(pl->tracks[0]), "%s", paths[i]);
        pl->track_count++;
        added++;
    }
    if (added > 0 && !playlist_save(pl)) return -1;
    return added;
}

int playlist_merge(playlist_t *dest, const playlist_t *src) {
    if (dest == src || strcmp(dest->file_path, src->file_path) == 0) return 0;
    int added = 0;
    for (int i = 0; i < src->track_count; i++) {
        if (dest->track_count >= MAX_PLAYLIST_TRACKS) break;
        if (playlist_has_track(dest, src->tracks[i])) continue;
        snprintf(dest->tracks[dest->track_count], sizeof(dest->tracks[0]), "%s", src->tracks[i]);
        dest->track_count++;
        added++;
    }
    if (added > 0 && !playlist_save(dest)) return -1;
    return added;
}

/* Reads the whole file into a malloc'd, NUL-terminated buffer. Caller
 * frees. Returns NULL on error. */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Parses a JSON string literal starting at the opening quote `*p`.
 * Writes the unescaped value to `out` and returns a pointer just past
 * the closing quote, or NULL on malformed input. */
static const char *parse_json_string(const char *p, char *out, int out_len) {
    if (*p != '"') return NULL;
    p++;
    int oi = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'u': {
                    /* skip \uXXXX; not needed for our own paths, just
                     * don't choke on it if present */
                    if (strlen(p) >= 5) p += 4;
                    c = '?';
                    break;
                }
                default: c = *p; break;
            }
        }
        if (oi < out_len - 1) out[oi++] = c;
        p++;
    }
    out[oi] = '\0';
    if (*p != '"') return NULL;
    return p + 1;
}

/* Finds `"key"` in `json` and returns a pointer to the ':' that
 * follows it, or NULL if not found. */
static const char *find_json_key(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *found = strstr(json, pattern);
    if (!found) return NULL;
    const char *colon = strchr(found, ':');
    return colon;
}

bool playlist_load(const char *file_path, playlist_t *out) {
    char *buf = read_whole_file(file_path);
    if (!buf) return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->file_path, sizeof(out->file_path), "%s", file_path);

    const char *p = find_json_key(buf, "name");
    if (!p) { free(buf); return false; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!parse_json_string(p, out->name, sizeof(out->name))) { free(buf); return false; }

    p = find_json_key(buf, "tracks");
    if (p) {
        p++;
        while (*p && *p != '[') p++;
        if (*p == '[') {
            p++;
            while (out->track_count < MAX_PLAYLIST_TRACKS) {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') p++;
                if (*p == ']' || *p == '\0') break;
                const char *next = parse_json_string(p, out->tracks[out->track_count], sizeof(out->tracks[0]));
                if (!next) break;
                out->track_count++;
                p = next;
            }
        }
    }

    free(buf);
    return true;
}

static int compare_playlists(const void *a, const void *b) {
    return strcasecmp(((const playlist_t *)a)->name, ((const playlist_t *)b)->name);
}

int playlist_list_all(playlist_t *out, int max_playlists) {
    char dir[PATH_MAXLEN];
    playlist_dir(dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < max_playlists) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 6 || strcasecmp(entry->d_name + nlen - 5, ".json") != 0) continue;

        char full[PATH_MAXLEN];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        if (playlist_load(full, &out[count])) count++;
    }
    closedir(d);

    qsort(out, count, sizeof(playlist_t), compare_playlists);
    return count;
}
