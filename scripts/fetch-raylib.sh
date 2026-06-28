#!/usr/bin/env bash
#
# Fetch the prebuilt raylib static libraries needed to build the graphical
# (GUI) version of SIDE HUSTLE. Headers are committed to the repo; the .a
# libraries are not, so run this once before `make gui` / `make windows-gui`.
#
set -euo pipefail

VER="${RAYLIB_VERSION:-5.5}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DST="$ROOT/third_party/raylib"
BASE="https://github.com/raysan5/raylib/releases/download/${VER}"

mkdir -p "$DST/include" "$DST/lib/linux" "$DST/lib/win64"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Fetching raylib ${VER} ..."
curl -fsSL -o "$tmp/win.zip"   "${BASE}/raylib-${VER}_win64_mingw-w64.zip"
curl -fsSL -o "$tmp/linux.tgz" "${BASE}/raylib-${VER}_linux_amd64.tar.gz"

( cd "$tmp" && unzip -q win.zip && tar xzf linux.tgz )

cp "$tmp"/raylib-${VER}_win64_mingw-w64/include/*.h "$DST/include/"
cp "$tmp"/raylib-${VER}_win64_mingw-w64/lib/libraylib.a "$DST/lib/win64/"
cp "$tmp"/raylib-${VER}_linux_amd64/lib/libraylib.a "$DST/lib/linux/"

echo "raylib ${VER} ready in $DST"
