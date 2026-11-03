# Playlist module for the integration tests: plays the files listed in
# $ICES_TEST_LIST (separated by ':') once, then returns '' so ices exits.
import os

files = [f for f in os.environ.get("ICES_TEST_LIST", "").split(":") if f]
pos = 0


def ices_get_next():
    global pos
    if pos >= len(files):
        return ""
    pos += 1
    return files[pos - 1]


def ices_get_lineno():
    return pos
