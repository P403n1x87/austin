#!/bin/bash

set -e

PY=${1:-}
if [ -n "$PY" ]; then
    shift
fi

source /tmp/austin-test/bin/activate
ulimit -c unlimited
export LD_LIBRARY_PATH="/Users/gabriele/Projects/libunwind/src/.libs/"
AUSTIN_TESTS_PYTHON_VERSIONS=$PY pytest "$@"
deactivate
