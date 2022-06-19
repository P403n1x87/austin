#!/bin/bash

if [ -z "$1" ]; then
    args="-r"
else
    args=$@
fi

cog -P $args \
    README.md \
    src/argparse.c 
