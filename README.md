# gsradio — IceS 0.4 for the GStreamer era

`gsradio` streams a playlist to an [Icecast](https://icecast.org/) (or
Shoutcast) server as MP3. It is a C++23 reimplementation of **IceS 0.4**
("ices0"), the last release of the original IceS line. It keeps ices0's
on-the-fly **transcoding** of Ogg Vorbis, FLAC and MP3 into MP3, which the
newer IceS 2.x dropped. Decoding, resampling and LAME encoding are done with
GStreamer; the server connection uses libshout.

It has IceS 0.4's command line options, features and behaviour, under its own
name: the program, configuration file, XML namespace, playlist script hooks
and the files it writes are all called `gsradio`. See
[Migrating from IceS 0.4](#migrating-from-ices-04) to convert existing setups.

## Building

Requirements: a C++23 compiler (GCC 14+ or Clang 18+), a POSIX shell and
make (BSD or GNU), pkg-config, GStreamer ≥ 1.20 with gst-plugins-base and
gst-plugins-good (`lamemp3enc`, `flacdec`, `vorbisdec`, `mpg123audiodec`),
libshout ≥ 2.4.3 and libxml2. Optional: Python 3 (embed) and Perl for
scripted playlists, and liblame headers so default sample rates are chosen
exactly as LAME/ices0 did.

Debian/Ubuntu:

    sudo apt install build-essential pkg-config libxml2-dev libshout-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-good gstreamer1.0-plugins-base \
        libmp3lame-dev python3-dev libperl-dev

Then:

    ./configure
    make
    sudo make install        # optional; honours DESTDIR

`./configure --help` lists the options. The useful ones:

* `--prefix=DIR`, `--bindir=DIR`, `--mandir=DIR`: installation directories.
* `--sysconfdir=DIR`: where `gsradio.conf` is looked up (default `PREFIX/etc`).
* `--with-moduledir=DIR`: playlist module directory (default
  `SYSCONFDIR/gsradio/modules`).
* `--with-examplesdir=DIR`: install the sample configuration files into DIR
  instead of the two directories above, for packaging.
* `--disable-python` and `--disable-perl`: leave out scripted playlists
  (detected automatically by default). `--with-python-pkgconfig=NAME` picks the
  pkg-config module for embedding Python, and `--with-perl=PATH` picks the Perl
  interpreter.
* `--without-lame`: don't use liblame to choose default output sample rates.

The compiler and flags come from `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`,
`CPPFLAGS`, `LDFLAGS` and `PKG_CONFIG`, given in the environment or as
`VAR=value` arguments. Results are logged to `config.log`.

## Quick start

    ls ~/music/*.{mp3,ogg,flac} > playlist.txt
    gsradio -F playlist.txt -h icecast.example.com -P sourcepass -m /radio -R -b 128

`-R` turns on re-encoding, which is needed for Ogg Vorbis and FLAC files. Or
copy `conf/gsradio.conf.dist` to `gsradio.conf`, edit it, and run `gsradio -c gsradio.conf`.
See `man gsradio` (`doc/gsradio.1`) for every option.

Several streams can be fed from one playlist, e.g. a pass-through 128 kbps
stream plus a 32 kbps mono stream for narrowband listeners:

    gsradio -F playlist.txt -P pass -m /hi -b 128 -m /lo -b 32 -R -N 1

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
| XML config (`Configuration` root, `Playlist`/`Execution`/multiple `Stream` sections) | ✔ (gsradio namespace) |
| Config lookup: given path, then `SYSCONFDIR/<file>`; command line overrides config | ✔ |
| Multiple streams from one playlist, each with its own server/mount/encoding | ✔ |
| Protocols `http` (Icecast 2), `xaudiocast` (Icecast 1), `icy` (Shoutcast) | ✔ |
| Stream info: name, genre, description, URL, public flag, bitrate, server dumpfile | ✔ |
| MP3 pass-through with ID3v1/ID3v2 stripping, junk skipping, short-frame trimming | ✔ |
| Re-encoding to bitrate / sample rate / channels; LAME's default output rate | ✔ |
| Lazy re-encoding (matching MP3s are passed through unchanged) | ✔ |
| Transcoding Ogg Vorbis and FLAC to MP3 | ✔ (plus anything GStreamer decodes) |
| Crossfading (`-C`/`<Crossfade>`), linear, short-track rule | ✔ |
| Builtin playlist: randomize, reload on change, wrap around | ✔ |
| Python and Perl playlist modules (`init/shutdown/get_next/get_metadata/get_lineno` hooks) | ✔ (Python 3, `gsradio_` names) |
| Title streaming from script, ID3/Vorbis comment tags, or the file name; delayed update after connect | ✔ |
| Reading audio from stdin (`-` in the playlist) or a FIFO | ✔ |
| Log, cue (same 8-line format) and pid files in the base directory | ✔ (`gsradio.*`) |
| SIGINT/SIGTERM shutdown, SIGHUP log reopen + playlist reload, SIGUSR1 skip | ✔ |
| Daemon mode with the original messages | ✔ |
| Exits after more than 10 consecutive bad tracks or more than 10 consecutive stream errors | ✔ |

### Deliberate differences

These either fix bugs in ices0 or are unavoidable with modern libraries:

* Python modules run on **Python 3** (ices0 used Python 2). The sample module
  `conf/gsradio.py.dist` has been ported.
* **Crossfading requires every stream to re-encode.** ices0 fed pass-through
  streams unfaded data, so they drifted further ahead with every track. If any
  stream has `Reencode` 0, gsradio logs a warning and disables crossfading.
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

## Migrating from IceS 0.4

Everything IceS 0.4 could do works the same way, but the names have changed:

| IceS 0.4 | gsradio |
|---|---|
| `ices` program, `ices(1)` | `gsradio`, `gsradio(1)` |
| `ices.conf` | `gsradio.conf` |
| `xmlns:ices="http://www.icecast.org/projects/ices"`, `<ices:Configuration>` | `xmlns:gsradio="https://github.com/danengholm/gsradio"`, `<gsradio:Configuration>` |
| `ices.log`, `ices.cue`, `ices.pid` | `gsradio.log`, `gsradio.cue`, `gsradio.pid` |
| default playlist module `ices` (`ices.py`, `ices.pm`) | `gsradio` (`gsradio.py`, `gsradio.pm`) |
| module directory `…/etc/ices/modules` | `…/etc/gsradio/modules` |
| `ices_init`, `ices_shutdown`, `ices_get_next`, `ices_get_metadata`, `ices_get_lineno` | `gsradio_init`, `gsradio_shutdown`, `gsradio_get_next`, `gsradio_get_metadata`, `gsradio_get_lineno` |
| legacy `ices_python_*` / `ices_perl_*` hook names | not supported |
| default mount `/ices` | `/gsradio` |

The element names inside the configuration file are unchanged, so converting an
`ices.conf` only needs the namespace and prefix replaced:

    sed -e 's|http://www.icecast.org/projects/ices|https://github.com/danengholm/gsradio|' \
        -e 's|ices:Configuration|gsradio:Configuration|g' -e 's|xmlns:ices=|xmlns:gsradio=|' \
        ices.conf > gsradio.conf

Playlist scripts need their functions renamed from `ices_*` to `gsradio_*`.
Python modules also need porting to Python 3.

## Tests

    make check
    # or: python3 tests/integration_test.py ./gsradio [test-name ...]

The integration suite generates test media, runs `gsradio` against a fake Icecast
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

The function names `gsradio_init`, `gsradio_get_next`, … in the playlist modules are the
public scripting API and are kept in that form. The Perl glue (`src/perl_glue.c`) is C
and uses C naming.

## License

GPL-2.0-or-later (see `COPYING`). Parts of this program (MP3 header parsing
and trimming, the crossfader, configuration semantics) are ports of IceS 0.4,
© 2000 Alexander Haväng, 2001–2004 Brendan Cully and others.
