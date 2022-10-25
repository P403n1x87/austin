#!/bin/bash

if [ -z "$1" ]; then
    args="-r"
else
    args=$@
fi

cog -P $args \
    configure.ac \
    README.md \
    src/austin.h \
    src/argparse.c \
    snap/snapcraft.yaml
