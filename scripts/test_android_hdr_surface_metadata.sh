#!/usr/bin/env bash
# Compile the production HDR10 metadata forwarding of the Android EGL context.
# Accept an already patched mpv tree for offline use, otherwise fetch the
# pinned Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved mpv/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi

src="$source_dir/video/out/opengl/context_android.c"
python3 "$root/scripts/extract.py" symbol "$src" \
    "$workdir/android_hdr_surface_metadata.inc" --return EGLint --fn egl_metadata_value
python3 "$root/scripts/extract.py" symbol "$src" \
    "$workdir/android_hdr_surface_metadata.inc" --append --return bool --fn hdr_static_equal
python3 "$root/scripts/extract.py" symbol "$src" \
    "$workdir/android_hdr_surface_metadata.inc" --append --return void --fn update_hdr_metadata

# mpv itself builds without -Wsign-compare (`int i < MP_ARRAY_SIZE()` is its idiom).
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-sign-compare \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_android_hdr_surface_metadata.c" -lm
"$workdir/test"
