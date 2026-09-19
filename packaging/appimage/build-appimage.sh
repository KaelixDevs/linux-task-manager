#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -n1)"
TOOLS="$ROOT/.packaging/tools"
BUILD="$ROOT/.packaging/appimage-build"
APPDIR="$ROOT/.packaging/AppDir"
OUT="$ROOT/dist/Linux-Task-Manager-$VERSION-x86_64.AppImage"
LINUXDEPLOY="$TOOLS/linuxdeploy-x86_64.AppImage"

mkdir -p "$TOOLS" "$ROOT/dist"
rm -rf "$BUILD" "$APPDIR"

if [[ ! -x "$LINUXDEPLOY" ]]; then
  curl -L --fail --output "$LINUXDEPLOY" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
  chmod +x "$LINUXDEPLOY"
fi

meson setup "$BUILD" "$ROOT" --prefix=/usr --buildtype=release --wipe
meson compile -C "$BUILD"
DESTDIR="$APPDIR" meson install -C "$BUILD"

export LDAI_OUTPUT="$OUT"
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"

"$LINUXDEPLOY" --appimage-extract-and-run \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/linux-task-manager" \
  --desktop-file "$APPDIR/usr/share/applications/io.github.linuxtaskmanager.TaskManager.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/io.github.linuxtaskmanager.TaskManager.svg" \
  --output appimage

echo "AppImage written to $OUT"
