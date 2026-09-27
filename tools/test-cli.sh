#!/bin/sh
set -eu
version=$(sed -n 's/^#define QUILLMOTE_VERSION "\(.*\)"/\1/p' app-info.h)
actual=$(env -u DISPLAY -u WAYLAND_DISPLAY ./quillmote --version)
test "$actual" = "Quillmote $version"
help=$(env -u DISPLAY -u WAYLAND_DISPLAY ./quillmote --help)
printf '%s\n' "$help" | grep -q -- '--version'
printf '%s\n' "$help" | grep -q 'FILE'
if env -u DISPLAY -u WAYLAND_DISPLAY ./quillmote --invalid-option >/dev/null 2>&1; then
    echo 'Unknown options must fail.' >&2
    exit 1
fi
printf '%s\n' 'Headless command-line checks passed.'
