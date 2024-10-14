#!/bin/bash

help2man \
    -n "Frame stack sampler for CPython" \
    -i doc/examples.troff \
    src/austin > src/austin.1

VERSION=$(cat src/austin.h | sed -r -n "s/^#define VERSION[ ]+\"([0-9]+[.][0-9]+).*\"/\1/p")

# Update the version in the SVG file
if [[ $(uname) == "Darwin" ]]; then
    sed -E -i '' "s/for version [0-9]+[.][0-9]+/for version $VERSION/g" "doc/cheatsheet.svg"
else
    sed -i "s/for version [0-9]+[.][0-9]+/for version $VERSION/g" "doc/cheatsheet.svg"
fi

inkscape \
    --export-type="png" \
    --export-filename="doc/cheatsheet.png" \
    --export-dpi=192 \
    doc/cheatsheet.svg

inkscape \
    --export-type="pdf" \
    --export-filename="doc/cheatsheet.pdf" \
    doc/cheatsheet.svg
