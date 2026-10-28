#!/bin/bash

if [ -z "$1" ]; then
    args="-r"
else
    args=$@
fi

cog -P $args \
    configure.ac \
    docs/usage.md \
    src/austin.h \
    snap/snapcraft.yaml
