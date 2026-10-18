/* cover.c - finds album art for a track: embedded in the file (MP3 ID3v2 APIC,
 * FLAC PICTURE) or as an image next to it. Only libc; used by the MPRIS layer so
 * desktop widgets can show covers (mpris:artUrl). Like the rest of ldmp this is
 * deliberately small, not a full tag library: anything it doesn't understand just
 * means "no embedded art". */

#include "cover.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_TAG_BYTES   (16u * 1024u * 1024u)   /* ID3 tags bigger than this are ignored */
#define MAX_IMAGE_BYTES (16u * 1024u * 1024u)

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && strcasecmp(name + n - e, ext) == 0;
}

/* ---------------- images: format sniffing, cache ---------------- */

/* Returns the file extension for the image bytes, or NULL if it isn't a format we serve. */
static const char *image_ext(const unsigned char *d, size_t n) {
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpg";
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "png";
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) return "webp";
    return NULL;
}

static uint64_t fnv1a64(const unsigned char *d, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

static bool cache_dir(char *out, size_t out_len) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char base[1024];
    if (xdg && xdg[0] == '/') snprintf(base, sizeof(base), "%s", xdg);
    else if (home && home[0]) snprintf(base, sizeof(base), "%s/.cache", home);
    else return false;

    char p1[1100], p2[1200];
    snprintf(p1, sizeof(p1), "%s/ldmp", base);
    snprintf(p2, sizeof(p2), "%s/covers", p1);
    mkdir(base, 0755); mkdir(p1, 0755); mkdir(p2, 0755);
    struct stat st;
    if (stat(p2, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if ((size_t)snprintf(out, out_len, "%s", p2) >= out_len) return false;
    return true;
}

/* Stores image bytes in the cache (once per distinct image) and returns the file path. */
static bool store_image(const unsigned char *d, size_t n, char *out, size_t out_len) {
    const char *ext = image_ext(d, n);
    if (!ext || n == 0 || n > MAX_IMAGE_BYTES) return false;
    char dir[1200];
    if (!cache_dir(dir, sizeof(dir))) return false;

    char path[1300];
    snprintf(path, sizeof(path), "%s/%016llx.%s", dir, (unsigned long long)fnv1a64(d, n), ext);
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != n) {
        char tmp[1340];
        snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path, (long)getpid());
        FILE *f = fopen(tmp, "wb");
        if (!f) return false;
        bool ok = fwrite(d, 1, n, f) == n;
        ok = (fclose(f) == 0) && ok;
        if (!ok || rename(tmp, path) != 0) { remove(tmp); return false; }
    }
    return (size_t)snprintf(out, out_len, "%s", path) < out_len;
}

/* ---------------- MP3: ID3v2.3 / 2.4 APIC ---------------- */

static unsigned be32(const unsigned char *b) {
    return ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | b[3];
}
static unsigned syncsafe32(const unsigned char *b) {
    return ((unsigned)(b[0] & 0x7F) << 21) | ((unsigned)(b[1] & 0x7F) << 14) |
           ((unsigned)(b[2] & 0x7F) << 7)  |  (unsigned)(b[3] & 0x7F);
}

/* Undo ID3 "unsynchronisation" (FF 00 -> FF) in place; returns the new length. */
static size_t un_unsync(unsigned char *d, size_t n) {
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        d[w++] = d[r];
        if (d[r] == 0xFF && r + 1 < n && d[r + 1] == 0x00) r++;
    }
    return w;
}

/* Picks the image out of an APIC frame body. Returns the picture type (or -1) and sets the
 * image pointer and length through img and img_len. */
static int apic_image(const unsigned char *f, size_t n, const unsigned char **img, size_t *img_len) {
    if (n < 4) return -1;
    unsigned enc = f[0];
    size_t i = 1;
    while (i < n && f[i] != 0) i++;          /* MIME type, Latin-1, NUL-terminated */
    if (i + 2 >= n) return -1;
    i++;                                      /* NUL */
    int ptype = f[i++];
    if (enc == 1 || enc == 2) {               /* UTF-16 description: terminated by 00 00 on a 2-byte step */
        while (i + 1 < n && !(f[i] == 0 && f[i + 1] == 0)) i += 2;
        i += 2;
    } else {                                  /* Latin-1 / UTF-8: single NUL */
        while (i < n && f[i] != 0) i++;
        i++;
    }
    if (i >= n) return -1;
    *img = f + i;
    *img_len = n - i;
    return ptype;
}

static bool cover_from_mp3(FILE *fp, char *out, size_t out_len) {
    unsigned char h[10];
    if (fread(h, 1, 10, fp) != 10 || memcmp(h, "ID3", 3) != 0) return false;
    unsigned ver = h[3], flags = h[5];
    if (ver != 3 && ver != 4) return false;   /* ID3v2.2 uses 3-letter frame ids; not handled */
    size_t tag_size = syncsafe32(h + 6);
    if (tag_size == 0 || tag_size > MAX_TAG_BYTES) return false;

    unsigned char *tag = malloc(tag_size);
    if (!tag) return false;
    if (fread(tag, 1, tag_size, fp) != tag_size) { free(tag); return false; }
    if (ver == 3 && (flags & 0x80)) tag_size = un_unsync(tag, tag_size);   /* v2.3: whole tag */

    size_t pos = 0;
    if (flags & 0x40) {                       /* extended header */
        if (tag_size < 4) { free(tag); return false; }
        pos = ver == 4 ? syncsafe32(tag) : be32(tag) + 4;
    }

    bool found = false;
    const unsigned char *best = NULL; size_t best_len = 0; int best_type = -1;

    while (pos + 10 <= tag_size) {
        const unsigned char *fh = tag + pos;
        if (fh[0] == 0) break;                /* padding */
        size_t fsz = ver == 4 ? syncsafe32(fh + 4) : be32(fh + 4);
        unsigned fflags = ((unsigned)fh[8] << 8) | fh[9];
        size_t body = pos + 10;
        if (fsz > tag_size - body) break;     /* corrupt size */
        pos = body + fsz;
        if (memcmp(fh, "APIC", 4) != 0) continue;

        unsigned char *data = tag + body;
        size_t len = fsz;
        bool compressed = ver == 4 ? (fflags & 0x0008) : (fflags & 0x0080);
        bool encrypted  = ver == 4 ? (fflags & 0x0004) : (fflags & 0x0040);
        if (compressed || encrypted) continue;
        if (ver == 4 && (fflags & 0x0001)) {  /* data length indicator */
            if (len < 4) continue;
            data += 4; len -= 4;
        }
        if (ver == 4 && (fflags & 0x0002)) len = un_unsync(data, len);   /* per-frame unsync (edits the buffer in place) */

        const unsigned char *img = NULL; size_t img_len = 0;
        int t = apic_image(data, len, &img, &img_len);
        if (t < 0 || !image_ext(img, img_len)) continue;
        if (best == NULL || (t == 3 && best_type != 3)) { best = img; best_len = img_len; best_type = t; }
        if (t == 3) break;                    /* front cover: done */
    }
    if (best) found = store_image(best, best_len, out, out_len);
    free(tag);
    return found;
}

/* ---------------- FLAC: PICTURE metadata block ---------------- */

static bool cover_from_flac(FILE *fp, char *out, size_t out_len) {
    unsigned char sig[4];
    if (fread(sig, 1, 4, fp) != 4 || memcmp(sig, "fLaC", 4) != 0) return false;

    unsigned char *best = NULL; size_t best_len = 0; int best_type = -1;
    for (;;) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, fp) != 4) break;
        bool last = bh[0] & 0x80;
        unsigned type = bh[0] & 0x7F;
        size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | bh[3];
        if (type == 6 && len >= 32 && len <= MAX_IMAGE_BYTES + 4096) {
            unsigned char *b = malloc(len);
            if (!b) break;
            if (fread(b, 1, len, fp) != len) { free(b); break; }
            size_t p = 0;
            unsigned ptype = be32(b + p); p += 4;
            unsigned mlen = be32(b + p); p += 4;
            if (mlen > len - p || len - p - mlen < 4) { free(b); goto next; }
            p += mlen;
            unsigned dlen = be32(b + p); p += 4;
            if (dlen > len - p || len - p - dlen < 20) { free(b); goto next; }
            p += dlen + 16;                   /* width, height, depth, colours */
            unsigned ilen = be32(b + p); p += 4;
            if (ilen > len - p || !image_ext(b + p, ilen)) { free(b); goto next; }
            if (best == NULL || (ptype == 3 && best_type != 3)) {
                free(best);
                best = malloc(ilen);
                if (best) { memcpy(best, b + p, ilen); best_len = ilen; best_type = (int)ptype; }
            }
            free(b);
            if (best_type == 3) break;
        } else if (fseek(fp, (long)len, SEEK_CUR) != 0) break;
    next:
        if (last) break;
    }
    bool ok = best && store_image(best, best_len, out, out_len);
    free(best);
    return ok;
}

/* ---------------- an image next to the file ---------------- */

static bool cover_from_folder(const char *track_path, char *out, size_t out_len) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", track_path);
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    if (slash == dir) dir[1] = '\0'; else *slash = '\0';

    static const char *stems[] = { "cover", "folder", "front", "album", "albumart" };
    static const char *exts[]  = { ".jpg", ".jpeg", ".png", ".webp" };
    DIR *d = opendir(dir);
    if (!d) return false;

    int best = 1000;
    char best_name[512] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        for (size_t s = 0; s < sizeof(stems) / sizeof(*stems); s++) {
            size_t sl = strlen(stems[s]);
            if (strncasecmp(e->d_name, stems[s], sl) != 0) continue;
            for (size_t x = 0; x < sizeof(exts) / sizeof(*exts); x++) {
                if (strlen(e->d_name) == sl + strlen(exts[x]) && has_ext(e->d_name, exts[x])) {
                    int rank = (int)(s * 10 + x);
                    if (rank < best) { best = rank; snprintf(best_name, sizeof(best_name), "%s", e->d_name); }
                }
            }
        }
    }
    closedir(d);
    if (best_name[0] == '\0') return false;
    size_t need = (size_t)snprintf(out, out_len, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, best_name);
    return need < out_len;
}

/* ---------------- public ---------------- */

bool cover_find(const char *track_path, char *out, size_t out_len) {
    if (!track_path || !track_path[0] || !out || out_len == 0) return false;
    out[0] = '\0';
    if (getenv("LDMP_NO_COVER")) return false;   /* debugging switch: behave as if no cover art exists */

    FILE *fp = fopen(track_path, "rb");
    if (fp) {
        bool ok = false;
        if (has_ext(track_path, ".mp3")) ok = cover_from_mp3(fp, out, out_len);
        else if (has_ext(track_path, ".flac")) ok = cover_from_flac(fp, out, out_len);
        fclose(fp);
        if (ok) return true;
    }
    return cover_from_folder(track_path, out, out_len);
}

bool cover_path_to_url(const char *path, char *out, size_t out_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    char abs[PATH_MAX];
    if (path[0] != '/' && realpath(path, abs)) path = abs;   /* ldmp may have been started with a relative folder */
    const char *prefix = "file://";
    if (out_len < strlen(prefix) + 1) return false;
    memcpy(out, prefix, strlen(prefix)); w = strlen(prefix);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        bool plain = isalnum(*p) || *p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (w + (plain ? 1 : 3) + 1 > out_len) return false;
        if (plain) out[w++] = (char)*p;
        else { out[w++] = '%'; out[w++] = hex[*p >> 4]; out[w++] = hex[*p & 15]; }
    }
    out[w] = '\0';
    return true;
}
