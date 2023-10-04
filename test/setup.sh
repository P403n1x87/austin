#!/bin/bash

set -e

if [ -d "/tmp/austin-test" ]; then
    rm -rf /tmp/austin-test
fi

python3.10 -m venv /tmp/austin-test
source /tmp/austin-test/bin/activate
pip install -r test/requirements.txt
deactivate
