#!/bin/sh
# Wrapper used by "make check": run_tests.sh /path/to/ices
exec python3 "$(dirname "$0")/integration_test.py" "$@"
