#!/bin/bash

# help2man takes the continuation lines of a wrapped usage message as the start
# of the description, so we run it through a wrapper that unwraps the usage
# lines of the help output first.
AUSTIN_WRAPPER=$(mktemp -d)/austin
cat > "$AUSTIN_WRAPPER" <<EOF
#!/bin/sh
if [ "\$1" = "--help" ]; then
    "$(pwd)/src/austin" --help | awk '
        /^Usage:/ {
            usage = \$0
            while ((getline line) > 0 && line ~ /^[ \t]/) {
                sub(/^[ \t]+/, " ", line)
                usage = usage line
            }
            print usage
            print line
            next
        }
        { print }
    '
else
    exec "$(pwd)/src/austin" "\$@"
fi
EOF
chmod +x "$AUSTIN_WRAPPER"

help2man \
    -n "Frame stack sampler for CPython" \
    --no-info \
    -i doc/examples.troff \
    "$AUSTIN_WRAPPER" > src/austin.1

rm -rf "$(dirname "$AUSTIN_WRAPPER")"

VERSION=$(cat src/austin.h | sed -r -n "s/^#define VERSION[ ]+\"([0-9]+[.][0-9]+).*\"/\1/p")

# Update the version in the SVG file
if [[ $(uname) == "Darwin" ]]; then
    sed -E -i '' "s/for version [0-9]+[.][0-9]+/for version $VERSION/g" "doc/cheatsheet.svg"
else
    sed -E -i "s/for version [0-9]+[.][0-9]+/for version $VERSION/g" "doc/cheatsheet.svg"
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
