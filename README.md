# LDMP (Lena Dijksma Music Player)

A terminal music player with a cava-style real-time spectrum visualizer,
written in plain C11. No Python, no runtime dependencies beyond standard
Linux audio + ncurses.

- **Playback:** [miniaudio](https://miniaudio.dev/) (single-header,
  vendored in `src/`) — decodes and plays `.mp3`, `.wav`, `.flac` via
  ALSA/PulseAudio/PipeWire-pulse, whichever is available. It dlopen's
  the audio backend at runtime, so you don't need any `-dev` audio
  packages installed to build.
- **Visualizer:** a real FFT (radix-2, in `src/fft.c`) run directly on
  the PCM samples as they're mixed to the output device — the same
  approach [cava](https://github.com/karlstav/cava) uses, just built
  in. Log-spaced bands, auto-gain, rendered with Unicode sub-character
  blocks (▁▂▃▄▅▆▇█) so bars move smoothly instead of snapping between
  whole rows.
- **UI:** ncursesw.
- **Tags:** a small built-in ID3v2 (TIT2/TPE1) reader for MP3s; other
  formats just show the filename.

## Build

```bash
make
```

You need `gcc`/`clang`, `libncursesw` (dev headers — `sudo apt install
libncurses-dev` on Debian/Ubuntu), and the standard C library. No ALSA
or PulseAudio dev packages required.

## Install

```bash
./install.sh
```

Builds the binary, puts it on your PATH (`~/.local/bin`), and adds a
`music` alias to your shell config. Restart your terminal (or `source
~/.bashrc` / `~/.zshrc`), then just run:

```bash
music              # opens ~/Music
music ~/Podcasts   # or any other folder
```

Safe to re-run — it won't duplicate PATH entries or the alias.

## Run

```bash
./ldmp ~/Music
```

Omit the folder and it defaults to `~/Music`. Scans recursively for
`.mp3 .wav .flac` and sorts everything alphabetically.

You browse one folder at a time, file-manager style: subfolders (that
contain audio, directly or nested) are listed first, then the tracks
in that folder, each group sorted alphabetically. `/` search always
searches the whole library regardless of which folder you're in.

## Keybindings

| Key         | Action                              |
|-------------|--------------------------------------|
| `↑` / `k`   | Move up in list                      |
| `↓` / `j`   | Move down in list                    |
| `Enter`     | Open folder / play track / open playlist |
| `Backspace` | Go up a folder, or back out of playlist views |
| `Space`     | Play / pause                         |
| `n` / `→`   | Next track                           |
| `p` / `←`   | Previous track                       |
| `s`         | Stop                                 |
| `[` / `]`   | Seek back / forward 5s               |
| `+` / `-`   | Volume up / down                     |
| `r`         | Toggle repeat                        |
| `x`         | Toggle shuffle                       |
| `/`         | Search whole library (Esc to return to browsing) |
| `a`         | Add the selected track to a playlist |
| `v`         | Open/close the playlists view        |
| `c`         | (in playlists view) create a new playlist |
| `d`         | Remove a track from an open playlist, or (press twice) delete a playlist |
| `q`         | Quit                                  |

## Playlists

Playlists don't copy audio — each one is a small JSON file that just
lists the paths of the tracks in it, stored under
`~/.config/ldmp/playlists/<name>.json`:

```json
{
  "name": "Road Trip",
  "tracks": [
    "/home/lena/Music/Artist/Song.mp3"
  ]
}
```

Press `a` on any track to add it to a playlist (or create a new one
on the spot with `c`), or `v` to browse your playlists directly.
Removing a track from a playlist (`d`) or deleting a playlist
(`d` twice) never touches the underlying audio file.

Once you play a track from inside a playlist, `n`/`p` (and shuffle/
repeat) stay scoped to that playlist's tracks instead of the whole
library, until you play something from browsing or search instead.

## Project layout

```
src/
  main.c            ncurses UI, input handling, playlist logic
  audio.c/.h         miniaudio wrapper: play/pause/seek/volume + spectrum extraction
  fft.c/.h            radix-2 FFT + log-band bucketing (cava-style auto-gain)
  library.c/.h        recursive folder scan, folder-browse listing, minimal ID3v2 tag reader
  playlist.c/.h       JSON playlists (reference tracks by path, never copy audio)
  miniaudio.h          vendored single-header playback/decode library
  miniaudio_impl.c    the one .c file that compiles miniaudio's implementation
Makefile
```

## Notes / limitations

- Only `.mp3`, `.wav`, `.flac` are decoded (miniaudio's built-in
  decoders). Ogg Vorbis and AAC/m4a aren't wired up — that would need
  an extra decoder backend (stb_vorbis / etc.), left out to keep the
  build dependency-free.
- The ID3v2 reader handles the common case (Latin-1/UTF-8/UTF-16 text
  frames) but isn't a full tag library — it's enough for title/artist
  display, not spec-complete.
- If the underlying audio decoder ever hits an internal edge case on a
  particular file, it logs a line to `~/.ldmp_audio_warnings.log` and
  skips to the next track instead of crashing the whole player.
