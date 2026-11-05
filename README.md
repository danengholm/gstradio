# gsradio — IceS 0.4 for the GStreamer era

`ices` streams a playlist to an [Icecast](https://icecast.org/) (or
Shoutcast) server as MP3. It is a C++23 reimplementation of **IceS 0.4**
("ices0"), the last release of the original IceS line. It keeps ices0's
on-the-fly **transcoding** of Ogg Vorbis, FLAC and MP3 into MP3, which the
newer IceS 2.x dropped. Decoding, resampling and LAME encoding are done with
GStreamer; the server connection uses libshout.

It is a drop-in replacement. The binary is called `ices` and accepts the same
command line options, XML configuration file, playlist scripts, signals,
log/cue/pid files and exit behaviour.

## Building

Requirements: a C++23 compiler (GCC 14+ or Clang 18+), CMake ≥ 3.28,
GStreamer ≥ 1.20 with gst-plugins-base and gst-plugins-good (`lamemp3enc`,
`flacdec`, `vorbisdec`, `mpg123audiodec`), libshout ≥ 2.4.3 and libxml2.
Optional: Python 3 (embed) and Perl for scripted playlists, and liblame
headers so default sample rates are chosen exactly as LAME/ices0 did.

Debian/Ubuntu:

    sudo apt install build-essential cmake pkg-config libxml2-dev libshout-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-good gstreamer1.0-plugins-base \
        libmp3lame-dev python3-dev libperl-dev

Then:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    sudo cmake --install build        # optional

CMake options: `-DICES_WITH_PYTHON=OFF`, `-DICES_WITH_PERL=OFF`,
`-DICES_ETCDIR=...` (where `ices.conf` is looked up, default
`$prefix/etc`) and `-DICES_MODULEDIR=...` (playlist module directory).

## Quick start

    ls ~/music/*.{mp3,ogg,flac} > playlist.txt
    ices -F playlist.txt -h icecast.example.com -P sourcepass -m /radio -R -b 128

`-R` turns on re-encoding, which is needed for Ogg Vorbis and FLAC files. Or
copy `conf/ices.conf.dist` to `ices.conf`, edit it, and run `ices -c ices.conf`.
See `man ices` (`doc/ices.1`) for every option.

Several streams can be fed from one playlist, e.g. a pass-through 128 kbps
stream plus a 32 kbps mono stream for narrowband listeners:

    ices -F playlist.txt -P pass -m /hi -b 128 -m /lo -b 32 -R -N 1

(Each `-m` starts a new stream; options that follow it apply to that stream.)

## How it works

For each track the file is read once and every stream takes one of two paths:

* **raw** – the file's MP3 frames are relayed byte for byte (ID3 tags and
  trailing junk are stripped, as in ices0). This is used when the stream does
  not re-encode, or when the MP3 already matches the stream's bitrate, sample
  rate and channel count ("lazy re-encoding").
* **encode** – the same bytes feed a GStreamer pipeline
  (`appsrc ! decodebin ! audioconvert ! audioresample ! appsink`). The PCM
  optionally passes through the crossfader and then goes to a per-stream,
  long-lived `lamemp3enc` CBR encoder, so consecutive re-encoded tracks join
  without gaps.

libshout paces the output in real time. Song titles are pushed from a
background thread, in place of ices0's fork, so a slow server never interrupts
the audio.

## Feature parity with IceS 0.4

| Feature | Status |
|---|---|
| All command line options (`-B -b -C -c -D -d -F -f -g -H -h -i -M -m -N -n -P -p -R -r -S -s -t -u -V -v`) | ✔ |
| XML config (`ices:Configuration` namespace, `Playlist`/`Execution`/multiple `Stream` sections) | ✔ |
| Config lookup: given path, then `$ICES_ETCDIR/<file>`; command line overrides config | ✔ |
| Multiple streams from one playlist, each with its own server/mount/encoding | ✔ |
| Protocols `http` (Icecast 2), `xaudiocast` (Icecast 1), `icy` (Shoutcast) | ✔ |
| Stream info: name, genre, description, URL, public flag, bitrate, server dumpfile | ✔ |
| MP3 pass-through with ID3v1/ID3v2 stripping, junk skipping, short-frame trimming | ✔ |
| Re-encoding to bitrate / sample rate / channels; LAME's default output rate | ✔ |
| Lazy re-encoding (matching MP3s are passed through unchanged) | ✔ |
| Transcoding Ogg Vorbis and FLAC to MP3 | ✔ (plus anything GStreamer decodes) |
| Crossfading (`-C`/`<Crossfade>`), linear, short-track rule | ✔ |
| Builtin playlist: randomize, reload on change, wrap around | ✔ |
| Python and Perl playlist modules (`ices_init/shutdown/get_next/get_metadata/get_lineno` and legacy names) | ✔ (Python 3) |
| Title streaming from script, ID3/Vorbis comment tags, or the file name; delayed update after connect | ✔ |
| Reading audio from stdin (`-` in the playlist) or a FIFO | ✔ |
| `ices.log`, `ices.cue` (same 8-line format), `ices.pid` in the base directory | ✔ |
| SIGINT/SIGTERM shutdown, SIGHUP log reopen + playlist reload, SIGUSR1 skip | ✔ |
| Daemon mode with the original messages | ✔ |
| Exits after more than 10 consecutive bad tracks or more than 10 consecutive stream errors | ✔ |

### Deliberate differences

These either fix bugs in ices0 or are unavoidable with modern libraries:

* Python modules run on **Python 3** (ices0 used Python 2). The sample module
  `conf/ices.py.dist` has been ported.
* **Crossfading requires every stream to re-encode.** ices0 fed pass-through
  streams unfaded data, so they drifted further ahead with every track. If any
  stream has `Reencode` 0, ices logs a warning and disables crossfading.
* Builtin playlist: blank lines and `#` comments (for example M3U headers) are
  skipped. In ices0 a blank line could stop the stream. Randomized playlists
  are reshuffled on every pass (ices0 shuffled once), and SIGHUP also re-reads
  the builtin playlist.
* A `-c` file that doesn't exist, or a malformed config file, is an error
  (ices0 silently fell back to defaults). Unknown options are rejected.
* `-i` sets the ICY protocol, as ices0's usage text promised (the option was
  documented but did nothing there).
* Log *file* lines are timestamped; console output is unchanged.
* Exit status is 0 for a normal shutdown and 1 on errors (ices0 always exited
  with 1). The daemon's parent process exits with 0.
* Perl modules are loaded with `.` in `@INC`, as old Perls did implicitly,
  and STDOUT is autoflushed.
* Undecodable data in a `.mp3` file is passed to GStreamer instead of being
  rejected outright, so other formats with misleading extensions still play
  when re-encoding.

## Tests

    ctest --test-dir build --output-on-failure
    # or: python3 tests/integration_test.py build/ices [test-name ...]

The integration suite generates test media, runs `ices` against a fake Icecast
server (`tests/fake_icecast.py`, which speaks HTTP, x-audiocast and ICY),
and checks titles, formats, durations, real-time pacing, crossfade shape,
signals, daemon mode, stdin input and error handling. It needs
`gst-launch-1.0`, `ffmpeg`/`ffprobe` and, for the crossfade check, numpy.

## Code style

The code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
and the [Google Python Style Guide](https://google.github.io/styleguide/pyguide.html), with two
project-wide changes: **4-space indentation** and a **120 column** line limit. C++23 library
features such as `std::expected` and `std::print` are used.

The settings live in `.clang-format`, `.style.yapf` and `CPPLINT.cfg`:

    clang-format -i src/*.cc src/*.h src/*.c
    cpplint src/*
    yapf -i tests/*.py tests/modules/*.py

`CPPLINT.cfg` turns off two checks that misfire on modern C++: cpplint reads
clang-format's aligned array initializers as namespace indentation, and it reports
`if (init; condition) statement;` as having multiple statements.

The function names `ices_init`, `ices_get_next`, … in the playlist modules are IceS's
public scripting API and are kept as they are. The Perl glue (`src/perl_glue.c`) is C
and uses C naming.

## License

GPL-2.0-or-later (see `COPYING`). Parts of this program (MP3 header parsing
and trimming, the crossfader, configuration semantics) are ports of IceS 0.4,
© 2000 Alexander Haväng, 2001–2004 Brendan Cully and others.
