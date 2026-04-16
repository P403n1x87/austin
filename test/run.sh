#!/bin/bash

set -e

PY=${1:-}
if [ -n "$PY" ]; then
    shift
fi

source /tmp/austin-test/bin/activate
ulimit -c unlimited
AUSTIN_TESTS_PYTHON_VERSIONS=$PY pytest "$@"
deactivate
