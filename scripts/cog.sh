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
    src/linux/py_proc.h \
    snap/snapcraft.yaml
