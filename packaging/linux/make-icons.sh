#!/bin/sh
# Builds the hicolor app icons in icons/ from docs/assets/scratter-logo.png,
# one PNG per size that desktops ask for, so none of them has to shrink the
# 256 px one itself. Run it again after changing the logo (needs ImageMagick:
# Arch `imagemagick`, Ubuntu `imagemagick`):
#
#   sh packaging/linux/make-icons.sh
set -eu
here=$(dirname "$0")
logo="$here/../../docs/assets/scratter-logo.png"
if command -v magick >/dev/null 2>&1; then convert="magick"; else convert="convert"; fi
mkdir -p "$here/icons"
for size in 16 22 24 32 48 64 128 256; do
    # -strip leaves out the timestamps ImageMagick would write, so running it
    # again on the same logo gives the same bytes.
    "$convert" "$logo" -filter Lanczos -resize "${size}x${size}" -strip \
        "$here/icons/${size}.png"
done
