#!/usr/bin/env python3
"""Minimal Icecast/Shoutcast stand-in for integration tests.

Accepts source connections using the HTTP (PUT/SOURCE), x-audiocast and ICY
protocols, records each mount's audio to <outdir>/<mount>.mp3, and logs every
event (connect headers, metadata updates, byte counts with timestamps) as JSON
lines to <outdir>/events.jsonl.

Usage: fake_icecast.py PORT OUTDIR [PASSWORD]
"""
import base64
import json
import os
import socketserver
import sys
import threading
import time
import urllib.parse

PORT = int(sys.argv[1])
OUTDIR = sys.argv[2]
PASSWORD = sys.argv[3] if len(sys.argv) > 3 else "hackme"
LOCK = threading.Lock()
os.makedirs(OUTDIR, exist_ok=True)


def event(kind, **kw):
    kw.update(kind=kind, t=time.time())
    with LOCK, open(os.path.join(OUTDIR, "events.jsonl"), "a") as f:
        f.write(json.dumps(kw) + "\n")


def mount_file(mount):
    name = mount.strip("/").replace("/", "_") or "root"
    return os.path.join(OUTDIR, name + ".mp3")


class Handler(socketserver.StreamRequestHandler):
    def read_headers(self):
        headers = {}
        while True:
            line = self.rfile.readline()
            if not line or line in (b"\r\n", b"\n"):
                break
            if b":" in line:
                k, v = line.decode("latin-1").split(":", 1)
                headers[k.strip().lower()] = v.strip()
        return headers

    def receive(self, mount, headers, proto):
        event("source", mount=mount, proto=proto, headers=headers)
        total = 0
        with open(mount_file(mount), "ab") as out:
            while True:
                data = self.rfile.read1(65536)
                if not data:
                    break
                out.write(data)
                total += len(data)
                event("data", mount=mount, bytes=len(data), total=total)
        event("disconnect", mount=mount, total=total)

    def metadata(self, query, proto):
        q = urllib.parse.parse_qs(query)
        event("metadata", proto=proto, mount=q.get("mount", [""])[0],
              song=q.get("song", [""])[0], mode=q.get("mode", [""])[0])

    def handle(self):
        first = self.rfile.readline()
        if not first:
            return
        line = first.decode("latin-1").rstrip("\r\n")
        parts = line.split(" ")

        if parts[0] in ("PUT", "SOURCE") and len(parts) >= 3 and parts[2].startswith("HTTP"):
            headers = self.read_headers()
            auth = headers.get("authorization", "")
            ok = auth.startswith("Basic ") and \
                base64.b64decode(auth[6:]).decode() == "source:" + PASSWORD
            if not ok:
                event("auth_fail", mount=parts[1])
                self.wfile.write(b"HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Icecast2 Server\"\r\n\r\n")
                return
            if headers.get("expect", "").lower() == "100-continue":
                self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
            else:
                self.wfile.write(b"HTTP/1.0 200 OK\r\n\r\n")
            self.wfile.flush()
            self.receive(parts[1], headers, "http")
        elif parts[0] == "SOURCE" and len(parts) >= 3:  # x-audiocast
            if parts[1] != PASSWORD:
                event("auth_fail", mount=parts[2])
                self.wfile.write(b"ERROR - Bad Password\n")
                return
            headers = self.read_headers()
            self.wfile.write(b"OK\n")
            self.wfile.flush()
            self.receive(parts[2], headers, "xaudiocast")
        elif parts[0] == "GET":
            target = parts[1]
            path, _, query = target.partition("?")
            headers = self.read_headers()
            if path == "/admin/metadata":
                if not headers.get("authorization"):
                    event("metadata_noauth")
                    self.wfile.write(b"HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Icecast2 Server\"\r\n\r\n")
                    return
                self.metadata(query, "http")
            elif path in ("/admin.cgi", "/admin"):
                self.metadata(query, "icy")
            body = b"<?xml version=\"1.0\"?>\n<iceresponse><message>Metadata update successful</message><return>1</return></iceresponse>\n"
            self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        elif line == PASSWORD:  # ICY: password line, then headers
            self.wfile.write(b"OK2\r\nicy-caps:11\r\n\r\n")
            self.wfile.flush()
            headers = self.read_headers()
            self.receive("/icy", headers, "icy")
        elif parts[0] == "OPTIONS":
            self.read_headers()
            self.wfile.write(b"HTTP/1.1 204 No Content\r\nAllow: GET, PUT, SOURCE, OPTIONS\r\n\r\n")
        else:
            event("bad_request", line=line)
            self.wfile.write(b"HTTP/1.0 400 Bad Request\r\n\r\n")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    # libshout's ICY mode connects to port + 1 (Shoutcast convention).
    icy = Server(("127.0.0.1", PORT + 1), Handler)
    threading.Thread(target=icy.serve_forever, daemon=True).start()
    with Server(("127.0.0.1", PORT), Handler) as srv:
        event("listening", port=PORT)
        srv.serve_forever()
