/* main.c - LDMP (Lena Dijksma Music Player): ncurses front-end tying
 * together library.c (scanning/tags/folder browsing), playlist.c
 * (JSON playlists that reference files, never copy them), and
 * audio.c (miniaudio playback + spectrum). */

#include "audio.h"
#include "library.h"
#include "playlist.h"
#include "mpris.h"

#include <ncurses.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdbool.h>

#define STATUS_MAXLEN 512

/* cava-style sub-character block glyphs (UTF-8), 8 fill levels + empty */
static const char *BLOCK_LEVELS[9] = {
    " ", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
    "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"
};
#define FULL_BLOCK "\xE2\x96\x88"

typedef enum {
    VIEW_BROWSE,          /* browsing folders under root_folder */
    VIEW_SEARCH,          /* flat search results across the whole library */
    VIEW_PLAYLISTS,       /* list of saved playlists (also used as an "add to..." picker) */
    VIEW_PLAYLIST_TRACKS  /* tracks inside one open playlist */
} view_t;

typedef enum {
    TEXT_NONE,
    TEXT_SEARCH,
    TEXT_PLAYLIST_NAME
} text_mode_t;

typedef struct {
    library_t lib;

    view_t view;
    view_t view_before_playlists; /* where to return to when leaving playlist views */

    int filtered[MAX_TRACKS];
    int filtered_count;
    char search_query[128];
    int search_len;

    char root_folder[PATH_MAXLEN];
    char current_folder[PATH_MAXLEN];
    browse_entry_t entries[MAX_BROWSE_ENTRIES];
    int entries_count;

    playlist_t playlists[MAX_PLAYLISTS];
    int playlist_count;
    playlist_t active_playlist;              /* currently open in VIEW_PLAYLIST_TRACKS */
    int playlist_track_idx[MAX_PLAYLIST_TRACKS]; /* lib.tracks index for each active_playlist track, -1 if missing */

    bool pending_add;                 /* 'a' was pressed: VIEW_PLAYLISTS is acting as a picker */
    char pending_add_path[PATH_MAXLEN];
    bool confirm_delete;              /* 'd' pressed once on a playlist; asking to confirm */

    text_mode_t text_mode;
    char name_input[PLAYLIST_NAME_MAXLEN];
    int name_input_len;

    int cursor;
    int scroll;
    int saved_cursor;   /* browse/search cursor+scroll to restore when leaving playlist views */
    int saved_scroll;

    int current_idx;      /* index into lib.tracks, -1 if none */
    bool playing_from_playlist;    /* true: n/p/auto-advance stay within playing_playlist */
    playlist_t playing_playlist;   /* snapshot of the playlist playback started from */
    int playing_playlist_pos;      /* current position within playing_playlist.tracks */
    bool shuffle;
    bool repeat;
    char status[STATUS_MAXLEN];
} app_t;

static void refresh_browse(app_t *app) {
    app->entries_count = library_list_folder(&app->lib, app->current_folder, app->entries, MAX_BROWSE_ENTRIES);
    app->cursor = 0;
    app->scroll = 0;
}

static void go_up_folder(app_t *app) {
    if (strcmp(app->current_folder, app->root_folder) == 0) return;
    char *slash = strrchr(app->current_folder, '/');
    if (slash && slash != app->current_folder) *slash = '\0';
    if (strlen(app->current_folder) < strlen(app->root_folder)) {
        snprintf(app->current_folder, sizeof(app->current_folder), "%s", app->root_folder);
    }
    refresh_browse(app);
}

static void apply_filter(app_t *app) {
    app->filtered_count = 0;
    if (app->search_len == 0) {
        for (int i = 0; i < app->lib.count; i++) app->filtered[app->filtered_count++] = i;
    } else {
        char q[128];
        for (int i = 0; i < app->search_len; i++) q[i] = (char)tolower((unsigned char)app->search_query[i]);
        q[app->search_len] = '\0';
        for (int i = 0; i < app->lib.count; i++) {
            char name[PATH_MAXLEN + TAG_MAXLEN];
            library_display_name(&app->lib.tracks[i], name, sizeof(name));
            char lname[sizeof(name)];
            int j = 0;
            for (; name[j]; j++) lname[j] = (char)tolower((unsigned char)name[j]);
            lname[j] = '\0';
            if (strstr(lname, q)) app->filtered[app->filtered_count++] = i;
        }
    }
    app->cursor = 0;
    app->scroll = 0;
}

/* Resolves every track path in app->active_playlist to a lib.tracks
 * index (adding it to the library if it wasn't already scanned),
 * caching the result so the draw loop doesn't redo this every frame. */
static void refresh_playlist_track_index(app_t *app) {
    for (int i = 0; i < app->active_playlist.track_count; i++) {
        app->playlist_track_idx[i] = library_find_or_add_track(&app->lib, app->active_playlist.tracks[i]);
    }
}

static void refresh_playlists(app_t *app) {
    app->playlist_count = playlist_list_all(app->playlists, MAX_PLAYLISTS);
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void play_track(app_t *app, int real_idx) {
    if (real_idx < 0 || real_idx >= app->lib.count) return;
    if (audio_play_file(app->lib.tracks[real_idx].path) == 0) {
        app->current_idx = real_idx;
        char name[PATH_MAXLEN + TAG_MAXLEN];
        library_display_name(&app->lib.tracks[real_idx], name, sizeof(name));
        if (app->playing_from_playlist) {
            snprintf(app->status, sizeof(app->status), "Playing: %s  (from \"%s\")", name, app->playing_playlist.name);
        } else {
            snprintf(app->status, sizeof(app->status), "Playing: %s", name);
        }
    } else {
        snprintf(app->status, sizeof(app->status), "Failed to open file (unsupported/corrupt?)");
    }
}

/* Plays a track from the flat library (browse/search) - next/prev will
 * from here on cycle the whole library, not any particular playlist. */
static void play_from_library(app_t *app, int real_idx) {
    app->playing_from_playlist = false;
    play_track(app, real_idx);
}

/* Plays track `pos` of playlist `pl` and remembers it as the active
 * playback queue - next/prev/repeat will stay within this playlist
 * until something outside it is played. */
static void play_from_playlist(app_t *app, const playlist_t *pl, int pos) {
    if (pos < 0 || pos >= pl->track_count) return;
    int li = library_find_or_add_track(&app->lib, pl->tracks[pos]);
    if (li < 0) {
        snprintf(app->status, sizeof(app->status), "File missing: %s", pl->tracks[pos]);
        return;
    }
    app->playing_from_playlist = true;
    app->playing_playlist = *pl;
    app->playing_playlist_pos = pos;
    play_track(app, li);
}

static void play_next(app_t *app) {
    if (app->playing_from_playlist) {
        int n = app->playing_playlist.track_count;
        if (n == 0) return;
        for (int tries = 0; tries < n; tries++) {
            int next_pos = app->shuffle ? (rand() % n) : (app->playing_playlist_pos + 1) % n;
            app->playing_playlist_pos = next_pos;
            int li = library_find_or_add_track(&app->lib, app->playing_playlist.tracks[next_pos]);
            if (li >= 0) { play_track(app, li); return; }
        }
        snprintf(app->status, sizeof(app->status), "No playable tracks left in \"%s\"", app->playing_playlist.name);
        return;
    }
    if (app->lib.count == 0) return;
    int next = app->shuffle ? rand() % app->lib.count : (app->current_idx + 1) % app->lib.count;
    play_track(app, next);
}

static void play_prev(app_t *app) {
    if (app->playing_from_playlist) {
        int n = app->playing_playlist.track_count;
        if (n == 0) return;
        for (int tries = 0; tries < n; tries++) {
            int prev_pos = (app->playing_playlist_pos - 1 + n) % n;
            app->playing_playlist_pos = prev_pos;
            int li = library_find_or_add_track(&app->lib, app->playing_playlist.tracks[prev_pos]);
            if (li >= 0) { play_track(app, li); return; }
        }
        snprintf(app->status, sizeof(app->status), "No playable tracks left in \"%s\"", app->playing_playlist.name);
        return;
    }
    if (app->lib.count == 0) return;
    int prev = (app->current_idx - 1 + app->lib.count) % app->lib.count;
    play_track(app, prev);
}

/* ---- MPRIS control callbacks: remote widgets/media-keys drive the
 * same app_t as the keyboard, via mpris_tick()'s pumped D-Bus calls. */
static void mpris_cb_play(void *ud) {
    (void)ud;
    if (audio_get_state() == PLAYER_PAUSED) audio_toggle_pause();
}
static void mpris_cb_pause(void *ud) {
    (void)ud;
    if (audio_get_state() == PLAYER_PLAYING) audio_toggle_pause();
}
static void mpris_cb_play_pause(void *ud) { (void)ud; audio_toggle_pause(); }
static void mpris_cb_next(void *ud) { play_next((app_t *)ud); }
static void mpris_cb_prev(void *ud) { play_prev((app_t *)ud); }
static void mpris_cb_stop(void *ud) { (void)ud; audio_stop(); }
static void mpris_cb_seek(void *ud, double offset_seconds) { (void)ud; audio_seek_relative(offset_seconds); }

static void fmt_time(double seconds, char *out, int out_len) {
    if (seconds < 0 || isnan(seconds)) seconds = 0;
    int total = (int)seconds;
    snprintf(out, out_len, "%02d:%02d", total / 60, total % 60);
}

static void draw_visualizer(WINDOW *win, int y, int x, int width, int height, float bands[VIS_BANDS]) {
    int col_w = width / VIS_BANDS;
    if (col_w < 1) col_w = 1;

    for (int b = 0; b < VIS_BANDS; b++) {
        int cx = x + b * col_w;
        if (cx >= x + width) break;
        float v = bands[b];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        float total_levels = v * height * 8.0f; /* 8 sub-levels per row */
        int full_rows = (int)(total_levels / 8.0f);
        int remainder = (int)total_levels % 8;
        if (full_rows > height) { full_rows = height; remainder = 0; }

        int color_pair = (v < 0.4f) ? 2 : (v < 0.72f) ? 3 : 4;
        wattron(win, COLOR_PAIR(color_pair));
        for (int row = 0; row < full_rows; row++) {
            int wy = y + height - 1 - row;
            for (int cc = 0; cc < col_w - 1 && cx + cc < x + width; cc++) {
                mvwaddstr(win, wy, cx + cc, FULL_BLOCK);
            }
        }
        if (remainder > 0 && full_rows < height) {
            int wy = y + height - 1 - full_rows;
            for (int cc = 0; cc < col_w - 1 && cx + cc < x + width; cc++) {
                mvwaddstr(win, wy, cx + cc, BLOCK_LEVELS[remainder]);
            }
        }
        wattroff(win, COLOR_PAIR(color_pair));
    }
}

static int visible_count_for(app_t *app) {
    switch (app->view) {
        case VIEW_SEARCH: return app->filtered_count;
        case VIEW_BROWSE: return app->entries_count;
        case VIEW_PLAYLISTS: return app->playlist_count;
        case VIEW_PLAYLIST_TRACKS: return app->active_playlist.track_count;
    }
    return 0;
}

static void draw(WINDOW *win, app_t *app) {
    werase(win);
    int h, w;
    getmaxyx(win, h, w);
    if (h < 14 || w < 50) {
        mvwaddstr(win, 0, 0, "Terminal too small - resize to at least 50x14");
        wrefresh(win);
        return;
    }

    const char *title = " LDMP \xE2\x99\xAA ";
    wattron(win, COLOR_PAIR(1) | A_BOLD);
    mvwaddstr(win, 0, (w - (int)strlen(title) + 2) / 2, title);
    wattroff(win, COLOR_PAIR(1) | A_BOLD);

    char breadcrumb[600];
    switch (app->view) {
        case VIEW_SEARCH:
            snprintf(breadcrumb, sizeof(breadcrumb), "Search results for \"%s\" (%d)", app->search_query, app->filtered_count);
            break;
        case VIEW_BROWSE: {
            const char *rel = app->current_folder + strlen(app->root_folder);
            snprintf(breadcrumb, sizeof(breadcrumb), "%s%s", rel[0] ? rel : "/",
                     strcmp(app->current_folder, app->root_folder) == 0 ? "" : "  (Backspace: up)");
            break;
        }
        case VIEW_PLAYLISTS:
            if (app->pending_add) {
                snprintf(breadcrumb, sizeof(breadcrumb), "Add to playlist  (Enter: pick, c: new, Bksp: cancel)");
            } else {
                snprintf(breadcrumb, sizeof(breadcrumb), "Playlists  (Enter: open, c: new, dd: delete, Bksp: back)");
            }
            break;
        case VIEW_PLAYLIST_TRACKS:
            snprintf(breadcrumb, sizeof(breadcrumb), "Playlist: %s  (Enter: play, d: remove, Bksp: back)", app->active_playlist.name);
            break;
    }
    wattron(win, A_DIM);
    mvwaddstr(win, 1, 1, breadcrumb);
    wattroff(win, A_DIM);

    int list_h = h - 14;
    int list_w = w - 2;
    if (list_h < 1) list_h = 1;

    int visible_count = visible_count_for(app);
    if (app->cursor < app->scroll) app->scroll = app->cursor;
    else if (app->cursor >= app->scroll + list_h) app->scroll = app->cursor - list_h + 1;

    for (int row = 0; row < list_h; row++) {
        int fi = app->scroll + row;
        int y = 2 + row;
        if (fi >= visible_count) continue;

        char name[PATH_MAXLEN + TAG_MAXLEN];
        bool is_current = false;
        const char *prefix = "  ";

        switch (app->view) {
            case VIEW_SEARCH: {
                int real_idx = app->filtered[fi];
                track_t *t = &app->lib.tracks[real_idx];
                library_display_name(t, name, sizeof(name));
                is_current = (real_idx == app->current_idx) && audio_get_state() != PLAYER_STOPPED;
                if (is_current) prefix = "\xE2\x96\xB6 ";
                break;
            }
            case VIEW_BROWSE: {
                browse_entry_t *e = &app->entries[fi];
                if (e->type == BROWSE_FOLDER) {
                    snprintf(name, sizeof(name), "%s/", e->name);
                } else {
                    snprintf(name, sizeof(name), "%s", e->name);
                    is_current = (e->track_idx == app->current_idx) && audio_get_state() != PLAYER_STOPPED;
                    if (is_current) prefix = "\xE2\x96\xB6 ";
                }
                break;
            }
            case VIEW_PLAYLISTS: {
                playlist_t *p = &app->playlists[fi];
                snprintf(name, sizeof(name), "%s  (%d track%s)", p->name, p->track_count, p->track_count == 1 ? "" : "s");
                break;
            }
            case VIEW_PLAYLIST_TRACKS: {
                int li = app->playlist_track_idx[fi];
                if (li >= 0) {
                    library_display_name(&app->lib.tracks[li], name, sizeof(name));
                    is_current = (li == app->current_idx) && audio_get_state() != PLAYER_STOPPED;
                    if (is_current) prefix = "\xE2\x96\xB6 ";
                } else {
                    snprintf(name, sizeof(name), "%s (missing)", basename_of(app->active_playlist.tracks[fi]));
                }
                break;
            }
        }

        char line[800];
        snprintf(line, sizeof(line), "%s%s", prefix, name);

        bool is_cursor = (fi == app->cursor);
        int attr = 0;
        if (is_cursor) attr |= COLOR_PAIR(5);
        if (is_current) attr |= COLOR_PAIR(1) | A_BOLD;

        wattron(win, attr);
        mvwaddstr(win, y, 1, line);
        /* pad the rest of the line so selection highlight fills the row */
        int printed = (int)strlen(name) + 2;
        for (int p = printed; p < list_w; p++) mvwaddch(win, y, 1 + p, ' ');
        wattroff(win, attr);
    }

    int divider_y = h - 12;
    wattron(win, COLOR_PAIR(1));
    for (int i = 0; i < w; i++) mvwaddch(win, divider_y, i, ACS_HLINE);
    wattroff(win, COLOR_PAIR(1));

    int ny = divider_y + 1;
    if (app->current_idx >= 0) {
        track_t *t = &app->lib.tracks[app->current_idx];
        char name[PATH_MAXLEN + TAG_MAXLEN];
        library_display_name(t, name, sizeof(name));
        char info[900];
        if (app->playing_from_playlist) {
            snprintf(info, sizeof(info), "Now Playing: %s  (%s)", name, app->playing_playlist.name);
        } else {
            snprintf(info, sizeof(info), "Now Playing: %s", name);
        }
        wattron(win, A_BOLD);
        mvwaddstr(win, ny, 1, info);
        wattroff(win, A_BOLD);

        double pos = audio_get_position_seconds();
        double dur = audio_get_duration_seconds();
        float frac = dur > 0 ? (float)(pos / dur) : 0.0f;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;

        int bar_w = w - 20;
        if (bar_w < 1) bar_w = 1;
        int filled = (int)(bar_w * frac);
        wattron(win, COLOR_PAIR(2));
        for (int i = 0; i < bar_w; i++) {
            mvwaddstr(win, ny + 1, 1 + i, i < filled ? FULL_BLOCK : "\xE2\x94\x80");
        }
        wattroff(win, COLOR_PAIR(2));

        char pstr[16], dstr[16], tstr[40];
        fmt_time(pos, pstr, sizeof(pstr));
        fmt_time(dur, dstr, sizeof(dstr));
        snprintf(tstr, sizeof(tstr), "%s / %s", pstr, dstr);
        mvwaddstr(win, ny + 1, bar_w + 3, tstr);
    } else {
        wattron(win, A_DIM);
        mvwaddstr(win, ny, 1, "Nothing playing - press Enter on a track");
        wattroff(win, A_DIM);
    }

    /* Visualizer */
    int vis_y = ny + 3;
    int vis_h = h - vis_y - 3;
    if (vis_h < 1) vis_h = 1;
    float bands[VIS_BANDS];
    audio_get_spectrum(bands);
    draw_visualizer(win, vis_y, 1, w - 2, vis_h, bands);

    /* status + help */
    int status_y = h - 2;
    player_state_t st = audio_get_state();
    const char *state_str = st == PLAYER_PLAYING ? "PLAYING" : st == PLAYER_PAUSED ? "PAUSED" : "STOPPED";
    int vol_pct = (int)(audio_get_volume() * 100 + 0.5f);
    char left[STATUS_MAXLEN + 64];
    snprintf(left, sizeof(left), "[%s] vol:%d%%%s%s  %s", state_str, vol_pct,
              app->shuffle ? " shuffle" : "", app->repeat ? " repeat" : "", app->status);
    wattron(win, A_DIM);
    mvwaddstr(win, status_y, 1, left);
    wattroff(win, A_DIM);

    int help_y = h - 1;
    wattron(win, A_DIM);
    if (app->text_mode == TEXT_SEARCH) {
        char sline[256];
        snprintf(sline, sizeof(sline), "Search: %s_", app->search_query);
        mvwaddstr(win, help_y, 1, sline);
    } else if (app->text_mode == TEXT_PLAYLIST_NAME) {
        char sline[256];
        snprintf(sline, sizeof(sline), "New playlist name: %s_", app->name_input);
        mvwaddstr(win, help_y, 1, sline);
    } else {
        mvwaddstr(win, help_y, 1,
            "\xE2\x86\x91/\xE2\x86\x93 nav  Enter open/play  Bksp back  Space pause  n/p next/prev  "
            "r repeat  x shuffle  a add-to-playlist  v playlists  / search  q quit");
    }
    wattroff(win, A_DIM);

    wrefresh(win);
}

static void handle_input(app_t *app, int ch, bool *running) {
    if (app->text_mode != TEXT_NONE) {
        text_mode_t mode = app->text_mode;
        if (ch == 10 || ch == 13 || ch == KEY_ENTER) {
            app->text_mode = TEXT_NONE;
            if (mode == TEXT_SEARCH) {
                if (app->search_len > 0) {
                    apply_filter(app);
                    app->view = VIEW_SEARCH;
                } else {
                    app->view = VIEW_BROWSE;
                    refresh_browse(app);
                }
            } else { /* TEXT_PLAYLIST_NAME */
                playlist_t newpl;
                bool created = (app->name_input_len > 0) && playlist_create(app->name_input, &newpl);
                app->name_input_len = 0;
                app->name_input[0] = '\0';

                if (created && app->pending_add) {
                    playlist_add_track(&newpl, app->pending_add_path);
                    snprintf(app->status, sizeof(app->status), "Added to new playlist \"%s\"", newpl.name);
                    app->pending_add = false;
                    app->view = app->view_before_playlists;
                    app->cursor = app->saved_cursor;
                    app->scroll = app->saved_scroll;
                } else if (created) {
                    snprintf(app->status, sizeof(app->status), "Created playlist \"%s\"", newpl.name);
                    app->view = VIEW_PLAYLISTS;
                    refresh_playlists(app);
                    app->cursor = 0;
                    app->scroll = 0;
                    for (int i = 0; i < app->playlist_count; i++) {
                        if (strcmp(app->playlists[i].file_path, newpl.file_path) == 0) { app->cursor = i; break; }
                    }
                } else {
                    snprintf(app->status, sizeof(app->status), "Could not create playlist (empty or duplicate name)");
                    app->view = VIEW_PLAYLISTS;
                    refresh_playlists(app);
                    app->cursor = 0;
                    app->scroll = 0;
                }
            }
        } else if (ch == 27) {
            app->text_mode = TEXT_NONE;
            if (mode == TEXT_SEARCH) {
                app->search_len = 0;
                app->search_query[0] = '\0';
                app->view = VIEW_BROWSE;
                refresh_browse(app);
            } else {
                app->name_input_len = 0;
                app->name_input[0] = '\0';
                app->view = VIEW_PLAYLISTS;
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (mode == TEXT_SEARCH) {
                if (app->search_len > 0) app->search_query[--app->search_len] = '\0';
            } else if (app->name_input_len > 0) {
                app->name_input[--app->name_input_len] = '\0';
            }
        } else if (ch >= 32 && ch <= 126) {
            if (mode == TEXT_SEARCH && app->search_len < (int)sizeof(app->search_query) - 1) {
                app->search_query[app->search_len++] = (char)ch;
                app->search_query[app->search_len] = '\0';
            } else if (mode == TEXT_PLAYLIST_NAME && app->name_input_len < (int)sizeof(app->name_input) - 1) {
                app->name_input[app->name_input_len++] = (char)ch;
                app->name_input[app->name_input_len] = '\0';
            }
        }
        return;
    }

    if (ch != 'd') app->confirm_delete = false;

    switch (ch) {
        case 'q': case 'Q': *running = false; break;
        case '/':
            app->text_mode = TEXT_SEARCH;
            app->search_len = 0;
            app->search_query[0] = '\0';
            break;
        case KEY_UP: case 'k': if (app->cursor > 0) app->cursor--; break;
        case KEY_DOWN: case 'j': {
            int count = visible_count_for(app);
            if (app->cursor < count - 1) app->cursor++;
            break;
        }
        case 10: case 13: case KEY_ENTER:
            switch (app->view) {
                case VIEW_SEARCH:
                    if (app->filtered_count > 0) play_from_library(app, app->filtered[app->cursor]);
                    break;
                case VIEW_BROWSE:
                    if (app->entries_count > 0) {
                        browse_entry_t *e = &app->entries[app->cursor];
                        if (e->type == BROWSE_FOLDER) {
                            snprintf(app->current_folder, sizeof(app->current_folder), "%s", e->path);
                            refresh_browse(app);
                        } else {
                            play_from_library(app, e->track_idx);
                        }
                    }
                    break;
                case VIEW_PLAYLISTS:
                    if (app->playlist_count > 0) {
                        playlist_t *p = &app->playlists[app->cursor];
                        if (app->pending_add) {
                            if (playlist_add_track(p, app->pending_add_path)) {
                                snprintf(app->status, sizeof(app->status), "Added to \"%s\"", p->name);
                            } else {
                                snprintf(app->status, sizeof(app->status), "\"%s\" is full", p->name);
                            }
                            app->pending_add = false;
                            app->view = app->view_before_playlists;
                            app->cursor = app->saved_cursor;
                            app->scroll = app->saved_scroll;
                        } else {
                            app->active_playlist = *p;
                            refresh_playlist_track_index(app);
                            app->view = VIEW_PLAYLIST_TRACKS;
                            app->cursor = 0;
                            app->scroll = 0;
                        }
                    }
                    break;
                case VIEW_PLAYLIST_TRACKS:
                    if (app->active_playlist.track_count > 0) {
                        play_from_playlist(app, &app->active_playlist, app->cursor);
                    }
                    break;
            }
            break;
        case KEY_BACKSPACE: case 127: case 8:
            switch (app->view) {
                case VIEW_BROWSE: go_up_folder(app); break;
                case VIEW_SEARCH: break;
                case VIEW_PLAYLISTS:
                    app->pending_add = false;
                    app->view = app->view_before_playlists;
                    app->cursor = app->saved_cursor;
                    app->scroll = app->saved_scroll;
                    break;
                case VIEW_PLAYLIST_TRACKS:
                    refresh_playlists(app);
                    app->view = VIEW_PLAYLISTS;
                    app->cursor = 0;
                    app->scroll = 0;
                    break;
            }
            break;
        case 'v':
            if (app->view == VIEW_PLAYLISTS || app->view == VIEW_PLAYLIST_TRACKS) {
                app->pending_add = false;
                app->view = app->view_before_playlists;
                app->cursor = app->saved_cursor;
                app->scroll = app->saved_scroll;
            } else {
                app->view_before_playlists = app->view;
                app->saved_cursor = app->cursor;
                app->saved_scroll = app->scroll;
                app->pending_add = false;
                refresh_playlists(app);
                app->view = VIEW_PLAYLISTS;
                app->cursor = 0;
                app->scroll = 0;
            }
            break;
        case 'a': {
            const char *path = NULL;
            if (app->view == VIEW_BROWSE && app->entries_count > 0 && app->entries[app->cursor].type == BROWSE_TRACK) {
                path = app->lib.tracks[app->entries[app->cursor].track_idx].path;
            } else if (app->view == VIEW_SEARCH && app->filtered_count > 0) {
                path = app->lib.tracks[app->filtered[app->cursor]].path;
            } else if (app->view == VIEW_PLAYLIST_TRACKS && app->active_playlist.track_count > 0) {
                path = app->active_playlist.tracks[app->cursor];
            }
            if (path) {
                snprintf(app->pending_add_path, sizeof(app->pending_add_path), "%s", path);
                app->pending_add = true;
                app->view_before_playlists = app->view;
                app->saved_cursor = app->cursor;
                app->saved_scroll = app->scroll;
                refresh_playlists(app);
                app->view = VIEW_PLAYLISTS;
                app->cursor = 0;
                app->scroll = 0;
                snprintf(app->status, sizeof(app->status), "Add to which playlist?");
            }
            break;
        }
        case 'c':
            if (app->view == VIEW_PLAYLISTS) {
                app->text_mode = TEXT_PLAYLIST_NAME;
                app->name_input_len = 0;
                app->name_input[0] = '\0';
            }
            break;
        case 'd':
            if (app->view == VIEW_PLAYLISTS && app->playlist_count > 0) {
                if (app->confirm_delete) {
                    snprintf(app->status, sizeof(app->status), "Deleted playlist \"%s\"", app->playlists[app->cursor].name);
                    playlist_delete(&app->playlists[app->cursor]);
                    app->confirm_delete = false;
                    refresh_playlists(app);
                    if (app->cursor >= app->playlist_count) app->cursor = app->playlist_count > 0 ? app->playlist_count - 1 : 0;
                } else {
                    app->confirm_delete = true;
                    snprintf(app->status, sizeof(app->status), "Press d again to delete \"%s\"", app->playlists[app->cursor].name);
                }
            } else if (app->view == VIEW_PLAYLIST_TRACKS && app->active_playlist.track_count > 0) {
                playlist_remove_track(&app->active_playlist, app->cursor);
                refresh_playlist_track_index(app);
                if (app->cursor >= app->active_playlist.track_count) {
                    app->cursor = app->active_playlist.track_count > 0 ? app->active_playlist.track_count - 1 : 0;
                }
                snprintf(app->status, sizeof(app->status), "Removed from playlist");
            }
            break;
        case ' ': audio_toggle_pause(); break;
        case 'n': case KEY_RIGHT: play_next(app); break;
        case 'p': case KEY_LEFT: play_prev(app); break;
        case 's': audio_stop(); break;
        case '+': case '=': audio_set_volume(audio_get_volume() + 0.05f); break;
        case '-': case '_': audio_set_volume(audio_get_volume() - 0.05f); break;
        case ']': audio_seek_relative(5.0); break;
        case '[': audio_seek_relative(-5.0); break;
        case 'r':
            app->repeat = !app->repeat;
            snprintf(app->status, sizeof(app->status), "Repeat: %s", app->repeat ? "on" : "off");
            break;
        case 'x':
            app->shuffle = !app->shuffle;
            snprintf(app->status, sizeof(app->status), "Shuffle: %s", app->shuffle ? "on" : "off");
            break;
        default: break;
    }
}

int main(int argc, char **argv) {
    const char *folder = argc > 1 ? argv[1] : getenv("HOME");
    char default_music[PATH_MAXLEN];
    if (argc <= 1 && folder) {
        snprintf(default_music, sizeof(default_music), "%s/Music", folder);
        folder = default_music;
    }

    struct stat st_check;
    if (stat(folder, &st_check) != 0 || !S_ISDIR(st_check.st_mode)) {
        fprintf(stderr, "Folder not found: %s\nUsage: %s [music_folder]\n", folder, argv[0]);
        return 1;
    }

    if (audio_init() != 0) {
        fprintf(stderr, "Failed to initialize audio device.\n");
        return 1;
    }

    setlocale(LC_ALL, "");
    srand((unsigned)time(NULL));

    static app_t app;
    memset(&app, 0, sizeof(app));
    app.current_idx = -1;
    app.view = VIEW_BROWSE;
    library_scan(folder, &app.lib);
    snprintf(app.root_folder, sizeof(app.root_folder), "%s", folder);
    /* strip a trailing slash so prefix comparisons in library_list_folder line up */
    size_t rl = strlen(app.root_folder);
    while (rl > 1 && app.root_folder[rl - 1] == '/') app.root_folder[--rl] = '\0';
    snprintf(app.current_folder, sizeof(app.current_folder), "%s", app.root_folder);
    refresh_browse(&app);
    snprintf(app.status, sizeof(app.status), "Loaded %d tracks from %s", app.lib.count, folder);

    mpris_callbacks_t mpris_cbs = {
        .play = mpris_cb_play,
        .pause = mpris_cb_pause,
        .play_pause = mpris_cb_play_pause,
        .next = mpris_cb_next,
        .previous = mpris_cb_prev,
        .stop = mpris_cb_stop,
        .seek_relative = mpris_cb_seek,
        .userdata = &app,
    };
    mpris_init(&mpris_cbs);

    WINDOW *win = initscr();
    noecho();
    cbreak();
    keypad(win, TRUE);
    curs_set(0);
    timeout(60);

    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_RED, -1);
    init_pair(5, COLOR_BLACK, COLOR_CYAN);

    bool running = true;
    while (running) {
        if (audio_poll_and_clear_finished()) {
            if (app.repeat) play_track(&app, app.current_idx);
            else play_next(&app);
        }

        const char *mp_title = NULL, *mp_artist = NULL;
        double mp_duration = 0;
        if (app.current_idx >= 0) {
            track_t *ct = &app.lib.tracks[app.current_idx];
            mp_title = (ct->has_tags && ct->title[0]) ? ct->title : basename_of(ct->path);
            mp_artist = (ct->has_tags && ct->artist[0]) ? ct->artist : NULL;
            mp_duration = audio_get_duration_seconds();
        }
        mpris_tick(mp_title, mp_artist, mp_duration, audio_get_state(), audio_get_volume());

        draw(win, &app);
        int ch = wgetch(win);
        if (ch != ERR) handle_input(&app, ch, &running);
    }

    endwin();
    mpris_shutdown();
    audio_shutdown();
    return 0;
}
