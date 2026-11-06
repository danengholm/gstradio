#!/bin/sh
# Wrapper used by "make check": run_tests.sh /path/to/gsradio
exec python3 "$(dirname "$0")/integration_test.py" "$@"
