/* mpris.c - MPRIS2 media-player D-Bus interface for ldmp.
 *
 * Lets desktop widgets, waybar/eww/ags "now playing" modules,
 * playerctl, and media keys see and control ldmp the same way they
 * would any GUI player (Spotify, mpv, etc). Registers
 * org.mpris.MediaPlayer2.ldmp on the session bus and implements the
 * standard org.mpris.MediaPlayer2 / org.mpris.MediaPlayer2.Player /
 * org.freedesktop.DBus.Properties interfaces by hand (no GLib
 * mainloop needed - the connection is pumped non-blockingly once per
 * ncurses tick from mpris_tick()).
 *
 * If ldmp was built without dbus (no libdbus-1-dev at build time,
 * see Makefile), or there's no session bus to connect to at runtime,
 * every function below is a harmless no-op and ldmp behaves exactly
 * as it did before this file existed. */

#include "mpris.h"
#include "cover.h"

#ifdef LDMP_MPRIS

#include <dbus/dbus.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IFACE_ROOT   "org.mpris.MediaPlayer2"
#define IFACE_PLAYER "org.mpris.MediaPlayer2.Player"
#define IFACE_PROPS  "org.freedesktop.DBus.Properties"
#define OBJ_PATH     "/org/mpris/MediaPlayer2"

static DBusConnection *conn = NULL;
static bool have_bus = false;
static mpris_callbacks_t cbs;

/* Cached state, so mpris_tick() only emits PropertiesChanged when
 * something actually changed, and Get/GetAll always have an answer. */
static char cur_title[256]  = "";
static char cur_artist[256] = "";
static double cur_duration  = 0;
static player_state_t cur_state = PLAYER_STOPPED;
static float cur_volume = -1.0f;
static float sent_volume = -1.0f;      /* the volume last announced in a PropertiesChanged */
static bool cur_shuffle = false;
static bool cur_repeat  = false;
static char cur_path[1024] = "";
static char cur_url[4096] = "";        /* xesam:url */
static char cur_art_url[4096] = "";    /* mpris:artUrl, empty when the track has no cover */
static long track_serial = 0;
static char cur_trackid[64] = "/org/ldmp/track/0";

/* D-Bus strings must be valid UTF-8, and libdbus ABORTS THE WHOLE PROCESS if one isn't (old MP3s with
 * Latin-1 tags, or file names that aren't UTF-8, are common). So everything text that comes from tags or
 * paths goes through this first: valid UTF-8 is copied as is, any other byte is taken as Latin-1
 * ("Caf\xe9" becomes "Café"). Output is always valid UTF-8 and NUL-terminated. */
static void to_valid_utf8(const char *in, char *out, size_t cap) {
    size_t w = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && w + 4 < cap) {
        unsigned c = *p;
        int n = 0;                              /* continuation bytes expected */
        unsigned cp = 0;
        if (c < 0x80) { out[w++] = (char)c; p++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        bool ok = n > 0;
        for (int i = 1; ok && i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (ok) {
            if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||   /* overlong */
                (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) ok = false;
        }
        if (ok) { for (int i = 0; i <= n; i++) out[w++] = (char)p[i]; p += n + 1; }
        else {                                  /* Latin-1 fallback for this byte */
            out[w++] = (char)(0xC0 | (c >> 6));
            out[w++] = (char)(0x80 | (c & 0x3F));
            p++;
        }
    }
    out[w] = '\0';
}

/* ldmp has no "stop at the end" mode: without repeat it moves on to the next track and wraps
 * around, which is MPRIS's "Playlist". With repeat on it replays the current track: "Track". */
static const char *loop_str(bool repeat) { return repeat ? "Track" : "Playlist"; }

static const char *status_str(player_state_t s) {
    switch (s) {
        case PLAYER_PLAYING: return "Playing";
        case PLAYER_PAUSED:  return "Paused";
        default:              return "Stopped";
    }
}

/* ---------------- low-level a{sv} / variant helpers ---------------- */

static void append_variant_string(DBusMessageIter *iter, const char *s) {
    DBusMessageIter v;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
    dbus_message_iter_close_container(iter, &v);
}
static void append_variant_bool(DBusMessageIter *iter, dbus_bool_t b) {
    DBusMessageIter v;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(iter, &v);
}
static void append_variant_double(DBusMessageIter *iter, double d) {
    DBusMessageIter v;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "d", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &d);
    dbus_message_iter_close_container(iter, &v);
}
static void append_variant_int64(DBusMessageIter *iter, dbus_int64_t x) {
    DBusMessageIter v;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "x", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT64, &x);
    dbus_message_iter_close_container(iter, &v);
}
static void append_variant_string_array_empty(DBusMessageIter *iter) {
    DBusMessageIter v, arr;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "as", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_close_container(&v, &arr);
    dbus_message_iter_close_container(iter, &v);
}

static void dict_entry_begin(DBusMessageIter *dict, DBusMessageIter *entry, const char *key) {
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, entry);
    dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &key);
}
static void dict_entry_end(DBusMessageIter *dict, DBusMessageIter *entry) {
    dbus_message_iter_close_container(dict, entry);
}

static void append_dict_sv_string(DBusMessageIter *dict, const char *key, const char *val) {
    DBusMessageIter e; dict_entry_begin(dict, &e, key); append_variant_string(&e, val); dict_entry_end(dict, &e);
}
static void append_dict_sv_bool(DBusMessageIter *dict, const char *key, dbus_bool_t val) {
    DBusMessageIter e; dict_entry_begin(dict, &e, key); append_variant_bool(&e, val); dict_entry_end(dict, &e);
}
static void append_dict_sv_double(DBusMessageIter *dict, const char *key, double val) {
    DBusMessageIter e; dict_entry_begin(dict, &e, key); append_variant_double(&e, val); dict_entry_end(dict, &e);
}
static void append_dict_sv_int64(DBusMessageIter *dict, const char *key, dbus_int64_t val) {
    DBusMessageIter e; dict_entry_begin(dict, &e, key); append_variant_int64(&e, val); dict_entry_end(dict, &e);
}
static void append_dict_sv_string_array_empty(DBusMessageIter *dict, const char *key) {
    DBusMessageIter e; dict_entry_begin(dict, &e, key); append_variant_string_array_empty(&e); dict_entry_end(dict, &e);
}
static void append_dict_sv_object_path(DBusMessageIter *dict, const char *key, const char *path) {
    DBusMessageIter e, v;
    dict_entry_begin(dict, &e, key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "o", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(&e, &v);
    dict_entry_end(dict, &e);
}
static void append_dict_sv_string_array1(DBusMessageIter *dict, const char *key, const char *item) {
    DBusMessageIter e, v, arr;
    dict_entry_begin(dict, &e, key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "as", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &item);
    dbus_message_iter_close_container(&v, &arr);
    dbus_message_iter_close_container(&e, &v);
    dict_entry_end(dict, &e);
}

/* Appends the mpris:trackid / xesam:title / xesam:artist / mpris:length
 * dict as a variant onto `iter` - works whether `iter` is the top-level
 * reply of Properties.Get (bare variant) or a dict-entry's value slot
 * (GetAll / PropertiesChanged), since both just need a{sv} written in. */
static void append_metadata_variant(DBusMessageIter *iter) {
    DBusMessageIter var, dict;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a{sv}", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "{sv}", &dict);
    append_dict_sv_object_path(&dict, "mpris:trackid", cur_trackid);
    if (cur_duration > 0)
        append_dict_sv_int64(&dict, "mpris:length", (dbus_int64_t)(cur_duration * 1000000.0));
    if (cur_title[0])
        append_dict_sv_string(&dict, "xesam:title", cur_title);
    if (cur_artist[0])
        append_dict_sv_string_array1(&dict, "xesam:artist", cur_artist);
    if (cur_url[0])
        append_dict_sv_string(&dict, "xesam:url", cur_url);
    if (cur_art_url[0])
        append_dict_sv_string(&dict, "mpris:artUrl", cur_art_url);
    dbus_message_iter_close_container(&var, &dict);
    dbus_message_iter_close_container(iter, &var);
}
static void append_metadata_prop(DBusMessageIter *dict) {
    DBusMessageIter e;
    dict_entry_begin(dict, &e, "Metadata");
    append_metadata_variant(&e);
    dict_entry_end(dict, &e);
}

static void append_player_props(DBusMessageIter *dict) {
    append_dict_sv_string(dict, "PlaybackStatus", status_str(cur_state));
    append_dict_sv_double(dict, "Rate", 1.0);
    append_dict_sv_double(dict, "MinimumRate", 1.0);
    append_dict_sv_double(dict, "MaximumRate", 1.0);
    append_dict_sv_double(dict, "Volume", cur_volume < 0 ? 1.0 : (double)cur_volume);
    append_dict_sv_int64(dict, "Position", (dbus_int64_t)(audio_get_position_seconds() * 1000000.0));
    append_metadata_prop(dict);
    append_dict_sv_bool(dict, "Shuffle", cur_shuffle ? TRUE : FALSE);
    append_dict_sv_string(dict, "LoopStatus", loop_str(cur_repeat));
    append_dict_sv_bool(dict, "CanGoNext", TRUE);
    append_dict_sv_bool(dict, "CanGoPrevious", TRUE);
    append_dict_sv_bool(dict, "CanPlay", TRUE);
    append_dict_sv_bool(dict, "CanPause", TRUE);
    append_dict_sv_bool(dict, "CanSeek", TRUE);
    append_dict_sv_bool(dict, "CanControl", TRUE);
}
static void append_root_props(DBusMessageIter *dict) {
    append_dict_sv_bool(dict, "CanQuit", FALSE);
    append_dict_sv_bool(dict, "CanRaise", FALSE);
    append_dict_sv_bool(dict, "HasTrackList", FALSE);
    append_dict_sv_string(dict, "Identity", "LDMP");
    append_dict_sv_string_array_empty(dict, "SupportedUriSchemes");
    append_dict_sv_string_array_empty(dict, "SupportedMimeTypes");
}

/* ---------------- method call handling ---------------- */

static void reply_empty(DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}
static void reply_error_unknown(DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod", "Not supported");
    if (!reply) return;
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void handle_get(DBusMessage *msg) {
    const char *iface_name = NULL, *prop = NULL;
    DBusError err; dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface_name,
                                DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        reply_error_unknown(msg);
        return;
    }

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    bool ok = true;

    if (!strcmp(iface_name, IFACE_PLAYER)) {
        if      (!strcmp(prop, "PlaybackStatus")) append_variant_string(&iter, status_str(cur_state));
        else if (!strcmp(prop, "Metadata"))       append_metadata_variant(&iter);
        else if (!strcmp(prop, "Volume"))         append_variant_double(&iter, cur_volume < 0 ? 1.0 : (double)cur_volume);
        else if (!strcmp(prop, "Position"))       append_variant_int64(&iter, (dbus_int64_t)(audio_get_position_seconds() * 1000000.0));
        else if (!strcmp(prop, "Rate") || !strcmp(prop, "MinimumRate") || !strcmp(prop, "MaximumRate"))
            append_variant_double(&iter, 1.0);
        else if (!strcmp(prop, "Shuffle"))        append_variant_bool(&iter, cur_shuffle ? TRUE : FALSE);
        else if (!strcmp(prop, "LoopStatus"))     append_variant_string(&iter, loop_str(cur_repeat));
        else if (!strncmp(prop, "Can", 3))        append_variant_bool(&iter, TRUE);
        else ok = false;
    } else if (!strcmp(iface_name, IFACE_ROOT)) {
        if      (!strcmp(prop, "Identity"))       append_variant_string(&iter, "LDMP");
        else if (!strcmp(prop, "CanQuit") || !strcmp(prop, "CanRaise") || !strcmp(prop, "HasTrackList"))
            append_variant_bool(&iter, FALSE);
        else if (!strcmp(prop, "SupportedUriSchemes") || !strcmp(prop, "SupportedMimeTypes"))
            append_variant_string_array_empty(&iter);
        else ok = false;
    } else ok = false;

    if (!ok) { dbus_message_unref(reply); reply_error_unknown(msg); return; }
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void handle_get_all(DBusMessage *msg) {
    const char *iface_name = NULL;
    DBusError err; dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface_name, DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        reply_error_unknown(msg);
        return;
    }
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (!strcmp(iface_name, IFACE_PLAYER)) append_player_props(&dict);
    else if (!strcmp(iface_name, IFACE_ROOT)) append_root_props(&dict);
    dbus_message_iter_close_container(&iter, &dict);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

/* Properties.Set(interface, property, value). Shuffle, LoopStatus and Volume drive ldmp;
 * anything else (Rate, unknown properties) is just acknowledged so clients don't hang
 * waiting for a reply. The new value is announced by the next mpris_tick(). */
static void handle_set(DBusMessage *msg) {
    DBusMessageIter it, var;
    const char *iface = NULL, *prop = NULL;
    if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&it, &iface);
        if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&it, &prop);
            if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT
                && iface && prop && !strcmp(iface, IFACE_PLAYER)) {
                dbus_message_iter_recurse(&it, &var);
                int t = dbus_message_iter_get_arg_type(&var);
                if (!strcmp(prop, "Shuffle") && t == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&var, &b);
                    if (cbs.set_shuffle) cbs.set_shuffle(cbs.userdata, b ? true : false);
                } else if (!strcmp(prop, "LoopStatus") && t == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    if (s && cbs.set_repeat) cbs.set_repeat(cbs.userdata, !strcmp(s, "Track"));
                } else if (!strcmp(prop, "Volume") && t == DBUS_TYPE_DOUBLE) {
                    double v = 1.0;
                    dbus_message_iter_get_basic(&var, &v);
                    if (cbs.set_volume && isfinite(v)) cbs.set_volume(cbs.userdata, v);
                }
            }
        }
    }
    reply_empty(msg);
}

static void handle_player_method(DBusMessage *msg, const char *member) {
    if      (!strcmp(member, "Next"))     { if (cbs.next)       cbs.next(cbs.userdata);       reply_empty(msg); }
    else if (!strcmp(member, "Previous")) { if (cbs.previous)   cbs.previous(cbs.userdata);   reply_empty(msg); }
    else if (!strcmp(member, "Pause"))    { if (cbs.pause)      cbs.pause(cbs.userdata);      reply_empty(msg); }
    else if (!strcmp(member, "PlayPause")){ if (cbs.play_pause) cbs.play_pause(cbs.userdata); reply_empty(msg); }
    else if (!strcmp(member, "Stop"))     { if (cbs.stop)       cbs.stop(cbs.userdata);       reply_empty(msg); }
    else if (!strcmp(member, "Play"))     { if (cbs.play)       cbs.play(cbs.userdata);       reply_empty(msg); }
    else if (!strcmp(member, "Seek")) {
        dbus_int64_t offset_us = 0;
        DBusError err; dbus_error_init(&err);
        if (dbus_message_get_args(msg, &err, DBUS_TYPE_INT64, &offset_us, DBUS_TYPE_INVALID)) {
            if (cbs.seek_relative) cbs.seek_relative(cbs.userdata, offset_us / 1000000.0);
        } else if (dbus_error_is_set(&err)) dbus_error_free(&err);
        reply_empty(msg);
    } else if (!strcmp(member, "SetPosition")) {
        const char *trackid = NULL; dbus_int64_t pos_us = 0;
        DBusError err; dbus_error_init(&err);
        if (dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &trackid,
                                   DBUS_TYPE_INT64, &pos_us, DBUS_TYPE_INVALID)) {
            if (cbs.seek_relative) {
                double delta = (pos_us / 1000000.0) - audio_get_position_seconds();
                cbs.seek_relative(cbs.userdata, delta);
            }
        } else if (dbus_error_is_set(&err)) dbus_error_free(&err);
        reply_empty(msg);
    } else if (!strcmp(member, "OpenUri")) {
        reply_empty(msg);
    } else {
        reply_error_unknown(msg);
    }
}

static const char *INTROSPECT_XML =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.DBus.Properties\">\n"
"    <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
"    <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
"    <method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>\n"
"    <signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>\n"
"  </interface>\n"
"  <interface name=\"org.mpris.MediaPlayer2\">\n"
"    <method name=\"Raise\"/><method name=\"Quit\"/>\n"
"    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
"    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
"    <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
"  </interface>\n"
"  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
"    <method name=\"Next\"/><method name=\"Previous\"/><method name=\"Pause\"/>\n"
"    <method name=\"PlayPause\"/><method name=\"Stop\"/><method name=\"Play\"/>\n"
"    <method name=\"Seek\"><arg name=\"Offset\" type=\"x\" direction=\"in\"/></method>\n"
"    <method name=\"SetPosition\"><arg name=\"TrackId\" type=\"o\" direction=\"in\"/><arg name=\"Position\" type=\"x\" direction=\"in\"/></method>\n"
"    <method name=\"OpenUri\"><arg name=\"Uri\" type=\"s\" direction=\"in\"/></method>\n"
"    <signal name=\"Seeked\"><arg type=\"x\"/></signal>\n"
"    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
"    <property name=\"Rate\" type=\"d\" access=\"readwrite\"/>\n"
"    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
"    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
"    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
"    <property name=\"Shuffle\" type=\"b\" access=\"readwrite\"/>\n"
"    <property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>\n"
"    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
"    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
"    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
"  </interface>\n"
"</node>\n";

static void handle_introspect(DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &INTROSPECT_XML, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void handle_message(DBusMessage *msg) {
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) return;
    if (!dbus_message_has_path(msg, OBJ_PATH)) return;

    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!iface || !member) { reply_error_unknown(msg); return; }

    if (!strcmp(iface, IFACE_PROPS)) {
        if      (!strcmp(member, "Get"))    handle_get(msg);
        else if (!strcmp(member, "GetAll")) handle_get_all(msg);
        else if (!strcmp(member, "Set"))    handle_set(msg);
        else reply_error_unknown(msg);
    } else if (!strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        handle_introspect(msg);
    } else if (!strcmp(iface, IFACE_ROOT)) {
        reply_empty(msg); /* Raise / Quit: nothing to do, just ack */
    } else if (!strcmp(iface, IFACE_PLAYER)) {
        handle_player_method(msg, member);
    } else {
        reply_error_unknown(msg);
    }
}

/* ---------------- PropertiesChanged emission ---------------- */

static void emit_properties_changed(bool track_changed, bool status_changed, bool volume_changed,
                                    bool shuffle_changed, bool loop_changed) {
    if (!track_changed && !status_changed && !volume_changed && !shuffle_changed && !loop_changed) return;
    DBusMessage *sig = dbus_message_new_signal(OBJ_PATH, IFACE_PROPS, "PropertiesChanged");
    if (!sig) return;

    DBusMessageIter iter, dict, empty_arr;
    dbus_message_iter_init_append(sig, &iter);
    const char *iface = IFACE_PLAYER;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (status_changed) append_dict_sv_string(&dict, "PlaybackStatus", status_str(cur_state));
    if (track_changed)  append_metadata_prop(&dict);
    if (volume_changed) append_dict_sv_double(&dict, "Volume", cur_volume < 0 ? 1.0 : (double)cur_volume);
    if (shuffle_changed) append_dict_sv_bool(&dict, "Shuffle", cur_shuffle ? TRUE : FALSE);
    if (loop_changed)   append_dict_sv_string(&dict, "LoopStatus", loop_str(cur_repeat));
    dbus_message_iter_close_container(&iter, &dict);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &empty_arr);
    dbus_message_iter_close_container(&iter, &empty_arr);

    dbus_connection_send(conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(conn);
}

/* ---------------- public API ---------------- */

int mpris_init(const mpris_callbacks_t *callbacks) {
    if (getenv("LDMP_NO_MPRIS")) return -1;      /* debugging switch: don't register on D-Bus at all */
    if (callbacks) cbs = *callbacks;
    else memset(&cbs, 0, sizeof(cbs));

    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        have_bus = false;
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    char name[128];
    snprintf(name, sizeof(name), "org.mpris.MediaPlayer2.ldmp");
    int ret = dbus_bus_request_name(conn, name, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        dbus_error_init(&err);
        /* Another ldmp is already running - claim a per-instance name
         * instead, per the MPRIS spec's suffixing convention. */
        snprintf(name, sizeof(name), "org.mpris.MediaPlayer2.ldmp.instance%d", (int)getpid());
        ret = dbus_bus_request_name(conn, name, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
        if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            if (dbus_error_is_set(&err)) dbus_error_free(&err);
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            conn = NULL;
            have_bus = false;
            return -1;
        }
    }

    have_bus = true;
    return 0;
}

void mpris_shutdown(void) {
    if (conn) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        conn = NULL;
    }
    have_bus = false;
}

void mpris_tick(const mpris_state_t *st) {
    if (!have_bus || !conn || !st) return;

    dbus_connection_read_write(conn, 0);
    DBusMessage *msg;
    while ((msg = dbus_connection_pop_message(conn)) != NULL) {
        handle_message(msg);
        dbus_message_unref(msg);
    }

    char t[256], a[256];
    to_valid_utf8(st->title ? st->title : "", t, sizeof(t));
    to_valid_utf8(st->artist ? st->artist : "", a, sizeof(a));
    const char *p = st->path ? st->path : "";
    bool track_changed = (strcmp(t, cur_title) != 0 || strcmp(a, cur_artist) != 0 || strcmp(p, cur_path) != 0);
    if (track_changed) {
        track_serial++;
        snprintf(cur_trackid, sizeof(cur_trackid), "/org/ldmp/track/%ld", track_serial);
        snprintf(cur_title, sizeof(cur_title), "%s", t);
        snprintf(cur_artist, sizeof(cur_artist), "%s", a);
        snprintf(cur_path, sizeof(cur_path), "%s", p);

        /* where the file is, and its cover art (looked up once per track, not per tick) */
        cur_url[0] = cur_art_url[0] = '\0';
        if (p[0]) {
            if (!cover_path_to_url(p, cur_url, sizeof(cur_url))) cur_url[0] = '\0';
            char art[1024];
            if (cover_find(p, art, sizeof(art)) && !cover_path_to_url(art, cur_art_url, sizeof(cur_art_url)))
                cur_art_url[0] = '\0';
        }
    }
    cur_duration = st->duration_seconds;

    bool status_changed = (st->state != cur_state);
    cur_state = st->state;
    cur_volume = st->volume;
    bool volume_changed = fabsf(cur_volume - sent_volume) > 0.001f;
    if (volume_changed) sent_volume = cur_volume;

    bool shuffle_changed = (st->shuffle != cur_shuffle);
    bool loop_changed = (st->repeat != cur_repeat);
    cur_shuffle = st->shuffle;
    cur_repeat = st->repeat;

    emit_properties_changed(track_changed, status_changed, volume_changed, shuffle_changed, loop_changed);
}

#else /* !LDMP_MPRIS: built without libdbus - everything below is a no-op */

int mpris_init(const mpris_callbacks_t *callbacks) { (void)callbacks; return -1; }
void mpris_shutdown(void) {}
void mpris_tick(const mpris_state_t *state) { (void)state; }

#endif
