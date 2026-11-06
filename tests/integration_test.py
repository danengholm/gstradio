#!/usr/bin/env python3
"""End-to-end tests for gsradio against a fake Icecast server.

Usage: integration_test.py /path/to/gsradio [test-name ...]

Requires gst-launch-1.0 (with lame, vorbis, flac plugins), ffmpeg and ffprobe.
Media is generated into a temporary directory.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GSRADIO = os.path.abspath(sys.argv[1])
SELECTED = sys.argv[2:]
WORK = tempfile.mkdtemp(prefix="gsradio-test-")
MEDIA = os.path.join(WORK, "media")
MODULES = os.path.join(HERE, "modules")
PASSWORD = "hackme"
failures = []

# ---------------------------------------------------------------- helpers


def sh(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw)


def free_port():
    # The server also needs port + 1 for ICY.
    while True:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        try:
            with socket.socket() as s:
                s.bind(("127.0.0.1", port + 1))
            return port
        except OSError:
            continue


class Server:

    def __init__(self, name):
        self.port = free_port()
        self.dir = os.path.join(WORK, name)
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "fake_icecast.py"),
             str(self.port), self.dir, PASSWORD])
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.1).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("fake server did not start")

    def stop(self):
        self.proc.terminate()
        self.proc.wait()

    def events(self, kind=None):
        path = os.path.join(self.dir, "events.jsonl")
        if not os.path.exists(path):
            return []
        evs = [json.loads(l) for l in open(path)]
        return [e for e in evs if kind is None or e["kind"] == kind]

    def titles(self, mount):
        return [e["song"] for e in self.events("metadata") if e["mount"] == mount]

    def file(self, mount):
        return os.path.join(self.dir, mount.strip("/") + ".mp3")


def run_gsradio(args, timeout=60, env=None, cwd=None, sig_after=None):
    full_env = dict(os.environ, GSRADIO_TEST_MEDIA=MEDIA, **(env or {}))
    p = subprocess.Popen([GSRADIO] + args,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,
                         text=True,
                         env=full_env,
                         cwd=cwd or WORK)
    if sig_after:
        delay, sig = sig_after
        time.sleep(delay)
        p.send_signal(sig)
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGINT)
        out, _ = p.communicate(timeout=10)
    return p.returncode, out


def probe(path):
    """(sample_rate, channels, bit_rate, decoded seconds, decode error lines)"""
    info = json.loads(
        sh("ffprobe", "-v", "error", "-show_entries", "stream=sample_rate,channels,bit_rate", "-of", "json",
           path).stdout)["streams"][0]
    dec = subprocess.run(["ffmpeg", "-v", "error", "-stats", "-i", path, "-f", "null", "-"],
                         capture_output=True,
                         text=True)
    times = [t for t in dec.stderr.replace("\r", "\n").split() if t.startswith("time=")]
    h, m, s = times[-1][5:].split(":")
    errors = [l for l in dec.stderr.splitlines() if l and "time=" not in l and "size=" not in l]
    return int(info["sample_rate"]), int(info["channels"]), \
        int(info.get("bit_rate", 0)), \
        int(h) * 3600 + int(m) * 60 + float(s), errors


def check(cond, message):
    if not cond:
        raise AssertionError(message)


def media(name):
    return os.path.join(MEDIA, name)


def scripted(files):
    """Arguments + env for the 'play these files once' python playlist."""
    return ["-S", "python", "-M", "listpl"], {
        "GSRADIO_TEST_LIST": ":".join(media(f) for f in files),
        "PYTHONPATH": MODULES
    }


# ------------------------------------------------------------------ media


def make_media():
    os.makedirs(MEDIA)

    def gst(pipeline, out):
        sh("gst-launch-1.0", "-q", *pipeline.split(), f"location={media(out)}")

    def tag(name, artist, title):
        tmp = media("tmp_" + name)
        sh("ffmpeg", "-v", "error", "-y", "-i", media(name), "-c", "copy", "-metadata", f"artist={artist}", "-metadata",
           f"title={title}", tmp)
        os.replace(tmp, media(name))

    src = ("audiotestsrc num-buffers={n} freq={f} ! "
           "audio/x-raw,rate={r},channels={c}")
    gst(
        src.format(n=130, f=440, r=44100, c=2) + " ! lamemp3enc target=bitrate bitrate=128 cbr=true ! filesink",
        "mp3_128.mp3")
    tag("mp3_128.mp3", "Artist A", "Sine MP3")
    gst(src.format(n=130, f=550, r=48000, c=2) + " ! audioconvert ! vorbisenc ! oggmux ! filesink", "vorbis.ogg")
    tag("vorbis.ogg", "Artist C", "Vorbis Tone")
    gst(src.format(n=130, f=660, r=44100, c=1) + " ! audioconvert ! flacenc ! filesink", "mono.flac")
    tag("mono.flac", "Artist D", "FLAC Mono")
    gst(
        src.format(n=65, f=330, r=22050, c=2) + " ! lamemp3enc target=quality quality=4 ! xingmux ! filesink",
        "vbr22k.mp3")
    for i in (1, 2):
        gst(src.format(n=560, f=200 * i, r=44100, c=2) + " ! audioconvert ! flacenc ! filesink", f"long{i}.flac")
    with open(media("junk.mp3"), "w") as f:
        f.write("this is not audio\n" * 20)


# ------------------------------------------------------------------ tests


def test_transcode_formats():
    """MP3, Ogg Vorbis and FLAC transcoded into one continuous MP3 stream."""
    srv = Server("transcode")
    try:
        args, env = scripted(["mp3_128.mp3", "vorbis.ogg", "mono.flac"])
        t0 = time.time()
        rc, out = run_gsradio(
            args + ["-D", WORK, "-p", str(srv.port), "-P", PASSWORD, "-m", "/t", "-R", "-b", "96"], env=env)
        elapsed = time.time() - t0
    finally:
        srv.stop()
    check(rc == 0, f"exit status {rc}\n{out}")
    check("Playlist file name is empty, shutting down." in out, out)
    check(
        srv.titles("/t") == ["Artist A - Sine MP3", "Artist C - Vorbis Tone", "Artist D - FLAC Mono"],
        f"titles {srv.titles('/t')}")
    rate, ch, br, secs, errors = probe(srv.file("/t"))
    # 96 kbps stereo: LAME (and so IceS) resamples 44.1 kHz input to 32 kHz.
    check((rate, ch, br) == (32000, 2, 96000), f"format {(rate, ch, br)}")
    check(abs(secs - 9.0) < 0.6, f"decoded length {secs}")
    check(not errors, f"decode errors {errors[:3]}")
    check(7.5 < elapsed < 12, f"not paced in real time: {elapsed:.1f}s")


def test_multi_stream_protocols():
    """Pass-through, lazy reencoding and mono low bitrate; three protocols."""
    srv = Server("multi")
    conf = os.path.join(WORK, "multi.conf")
    server = (f"<Server><Hostname>127.0.0.1</Hostname>"
              f"<Port>{srv.port}</Port><Password>{PASSWORD}</Password>"
              "{protocol}</Server>")

    def stream(protocol, body):
        proto = f"<Protocol>{protocol}</Protocol>" if protocol else ""
        return f"<Stream>{server.format(protocol=proto)}{body}</Stream>\n"

    with open(conf, "w") as f:
        f.write('<?xml version="1.0"?>\n'
                '<gsradio:Configuration '
                'xmlns:gsradio="https://github.com/danengholm/gsradio">\n'
                "<Playlist><Type>python</Type><Module>listpl</Module></Playlist>\n"
                f"<Execution><BaseDirectory>{WORK}</BaseDirectory>"
                "<Verbose>1</Verbose></Execution>\n" + stream(
                    None, "<Mountpoint>/raw</Mountpoint>"
                    "<Name>Raw &amp; Ready</Name><Bitrate>128</Bitrate>"
                    "<Reencode>0</Reencode>") +
                stream("xaudiocast", "<Mountpoint>/lazy</Mountpoint>"
                       "<Bitrate>128</Bitrate><Reencode>1</Reencode>") +
                stream("icy", "<Bitrate>32</Bitrate><Reencode>1</Reencode>"
                       "<Channels>1</Channels>") + "</gsradio:Configuration>\n")
    try:
        _, env = scripted(["mp3_128.mp3", "vorbis.ogg", "vbr22k.mp3", "mp3_128.mp3"])
        rc, out = run_gsradio(["-c", conf], env=env)
    finally:
        srv.stop()
    check(rc == 0, f"exit status {rc}\n{out}")
    check("Cannot play " + media("vorbis.ogg") + " without reencoding" in out, out)
    check("MPEG-2 LSF layer III, VBR, 22050 Hz" in out, "VBR not detected")
    src = {e["mount"]: e for e in srv.events("source")}
    check(src["/raw"]["headers"].get("ice-name") == "Raw & Ready", src["/raw"]["headers"])
    check(src["/lazy"]["proto"] == "xaudiocast" and src["/icy"]["proto"] == "icy", src.keys())
    # The raw stream carries the original bytes of both matching MP3 files.
    raw = os.path.getsize(srv.file("/raw"))
    mp3 = os.path.getsize(media("mp3_128.mp3"))
    check(2 * mp3 - 1000 < raw < 2 * mp3 + os.path.getsize(media("vbr22k.mp3")),
          f"raw stream size {raw}, expected about {2 * mp3} + vbr22k")
    for mount, expect in (("/lazy", (44100, 2)), ("/icy", (22050, 1))):
        rate, ch, br, secs, errors = probe(srv.file(mount))
        check((rate, ch) == expect, f"{mount} format {(rate, ch)}")
        check(abs(secs - 9.0) < 0.8, f"{mount} length {secs}")
        check(not errors, f"{mount} decode errors {errors[:3]}")
    _, _, br, _, _ = probe(srv.file("/icy"))
    check(br == 32000, f"/icy bitrate {br}")


def test_crossfade():
    """Linear crossfade between long tracks."""
    srv = Server("xfade")
    try:
        args, env = scripted(["long1.flac", "long2.flac"])
        rc, out = run_gsradio(
            args + ["-D", WORK, "-p", str(srv.port), "-P", PASSWORD, "-m", "/xf", "-R", "-C", "3"], env=env)
    finally:
        srv.stop()
    check(rc == 0, f"exit status {rc}\n{out}")
    wav = os.path.join(WORK, "xf.raw")
    sh("ffmpeg", "-v", "error", "-y", "-i", srv.file("/xf"), "-ac", "1", "-ar", "8000", "-f", "s16le", wav)
    try:
        import numpy as np
    except ImportError:
        print("    (numpy missing, spectral check skipped)")
        return
    x = np.fromfile(wav, dtype=np.int16).astype(float) / 32768
    sr = 8000

    def amp(t, f):
        seg = x[int(t * sr):int(t * sr) + sr // 2]
        n = np.arange(len(seg)) / sr
        return 2 * abs(np.dot(seg, np.exp(-2j * np.pi * f * n))) / len(seg)

    # Track 1 (200 Hz) alone, then both during the fade, then track 2 alone.
    full = amp(5, 200)
    check(full > 0.3 and amp(5, 400) < 0.02 * full, "track 1 not clean")
    # Halfway through the 3 s fade (which starts at 10 s) both are at ~50%.
    mid = (amp(11.25, 200), amp(11.25, 400))
    check(all(0.35 * full < a < 0.65 * full for a in mid), f"no crossfade mix at 11.25s: {mid}")
    check(amp(14, 400) > 0.9 * full and amp(14, 200) < 0.02 * full, "track 2 not clean after fade")


def test_perl_playlist():
    """Perl module: title override, undef falls back to tags, XS loading."""
    srv = Server("perl")
    try:
        rc, out = run_gsradio(
            ["-D", WORK, "-S", "perl", "-M", "testpl", "-p",
             str(srv.port), "-P", PASSWORD, "-m", "/pl", "-R"],
            cwd=MODULES,
            timeout=4.5,
            env={"GSRADIO_TEST_LIST": media("mono.flac") + ":" + media("mp3_128.mp3")})
    finally:
        srv.stop()
    check("pl: gsradio_init" in out and "pl: gsradio_shutdown" in out, out)
    check(srv.titles("/pl")[:2] == ["Perl Override 1", "Artist A - Sine MP3"], f"titles {srv.titles('/pl')}")


def test_signals_and_daemon():
    """-B daemonizes with a pid file; SIGUSR1 skips; SIGHUP reopens the log."""
    srv = Server("signals")
    base = os.path.join(WORK, "daemon")
    os.makedirs(base)
    pl = os.path.join(WORK, "signals.txt")
    with open(pl, "w") as f:
        f.write(f"{media('long1.flac')}\n{media('mp3_128.mp3')}\n")
    try:
        rc, out = run_gsradio(
            ["-B", "-v", "-D", base, "-F", pl, "-p",
             str(srv.port), "-P", PASSWORD, "-m", "/sig", "-R"])
        check(rc == 0 and "Into the land of the dreaded daemons" in out, out)
        time.sleep(1.5)
        pid = int(open(os.path.join(base, "gsradio.pid")).read())
        os.kill(pid, signal.SIGUSR1)
        time.sleep(1)
        os.rename(os.path.join(base, "gsradio.log"), os.path.join(base, "gsradio.log.1"))
        os.kill(pid, signal.SIGHUP)
        time.sleep(1)
        cue = open(os.path.join(base, "gsradio.cue")).read().splitlines()
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
                time.sleep(0.1)
            except ProcessLookupError:
                break
        else:
            raise AssertionError("daemon did not exit on SIGTERM")
    finally:
        srv.stop()
    log1 = open(os.path.join(base, "gsradio.log.1")).read()
    check("skipping to next track" in log1, log1)
    check(srv.titles("/sig")[:2] == ["long1", "Artist A - Sine MP3"], srv.titles("/sig"))
    check(os.path.exists(os.path.join(base, "gsradio.log")), "log not reopened after SIGHUP")
    check("gsradio exiting..." in open(os.path.join(base, "gsradio.log")).read(), "no exit message")
    check(cue[0] == media("mp3_128.mp3") and cue[6:8] == ["Artist A", "Sine MP3"], f"cue {cue}")
    check(not os.path.exists(os.path.join(base, "gsradio.pid")), "pid file left behind")
    check(not os.path.exists(os.path.join(base, "gsradio.cue")), "cue file left behind")


def test_error_handling():
    """Bad files count as consecutive errors; an unreachable server is fatal."""
    pl = os.path.join(WORK, "errors.txt")
    with open(pl, "w") as f:
        f.write(f"{media('junk.mp3')}\n/nonexistent.mp3\n")
    srv = Server("errors")
    try:
        rc, out = run_gsradio(["-D", WORK, "-F", pl, "-p", str(srv.port), "-P", PASSWORD, "-R"])
    finally:
        srv.stop()
    check(rc == 1 and "Exiting after 10 consecutive errors." in out, out)
    check("Error opening /nonexistent.mp3" in out, out)
    check("Encountered error while transfering " + media("junk.mp3") in out, out)

    with open(pl, "w") as f:
        f.write(f"{media('mp3_128.mp3')}\n")
    rc, out = run_gsradio(["-D", WORK, "-F", pl, "-p", "1", "-R"])
    check(rc == 1 and "Too many stream errors, giving up" in out, out)


def test_stdin():
    """'-' in the playlist streams audio from stdin."""
    srv = Server("stdin")
    pl = os.path.join(WORK, "stdin.txt")
    with open(pl, "w") as f:
        f.write("-\n")
    try:
        with open(media("vorbis.ogg"), "rb") as stdin:
            p = subprocess.run([GSRADIO, "-D", WORK, "-F", pl, "-p",
                                str(srv.port), "-P", PASSWORD, "-m", "/in", "-R"],
                               stdin=stdin,
                               capture_output=True,
                               text=True,
                               timeout=60)
    finally:
        srv.stop()
    check(srv.titles("/in") == ["Artist C - Vorbis Tone"], srv.titles("/in"))
    _, _, _, secs, errors = probe(srv.file("/in"))
    check(abs(secs - 3.0) < 0.5 and not errors, f"stdin stream {secs}s {errors[:2]}")


def test_command_line():
    rc, out = run_gsradio(["-V"])
    check(rc == 0 and out.startswith("gsradio 0.3"), out)
    rc, out = run_gsradio(["-Z"])
    check(rc == 1 and "Unknown option: -Z" in out, out)
    rc, out = run_gsradio(["-c", "/nonexistent.conf"])
    check(rc == 1, out)


TESTS = [v for k, v in sorted(globals().items()) if k.startswith("test_")]

if __name__ == "__main__":
    for tool in ("gst-launch-1.0", "ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            print(f"SKIP: {tool} not found")
            sys.exit(77)
    print(f"work dir: {WORK}")
    make_media()
    for test in TESTS:
        name = test.__name__[5:]
        if SELECTED and name not in SELECTED:
            continue
        t0 = time.time()
        try:
            test()
            print(f"PASS  {name} ({time.time() - t0:.1f}s)")
        except Exception as e:  # noqa: BLE001
            failures.append(name)
            print(f"FAIL  {name}: {e}")
    if failures:
        print(f"\n{len(failures)} failed: {' '.join(failures)} "
              f"(artifacts in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("\nall tests passed")
