# Playlist module for the integration tests: plays the files listed in
# $GSRADIO_TEST_LIST (separated by ':') once, then returns '' so gsradio exits.
import os

files = [f for f in os.environ.get("GSRADIO_TEST_LIST", "").split(":") if f]
pos = 0


def gsradio_get_next():
    global pos
    if pos >= len(files):
        return ""
    pos += 1
    return files[pos - 1]


def gsradio_get_lineno():
    return pos
