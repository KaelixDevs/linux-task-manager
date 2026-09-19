#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -n1)"
ORIG="$ROOT/../linux-task-manager_${VERSION}.orig.tar.gz"
mkdir -p "$ROOT/dist"

# Keep a matching upstream tarball around so Debian's 3.0 (quilt) source format
# is also happy if a full source build is requested later.
if [[ ! -f "$ORIG" ]]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  mkdir -p "$TMP/linux-task-manager-$VERSION"
  ( cd "$ROOT" && tar --exclude='./.git' --exclude='./build' --exclude='./dist' --exclude='./.packaging' --exclude='./debian' -cf - . ) \
    | tar -xf - -C "$TMP/linux-task-manager-$VERSION"
  tar -C "$TMP" -czf "$ORIG" "linux-task-manager-$VERSION"
fi

cd "$ROOT"
dpkg-buildpackage -us -uc -b

find "$ROOT/.." -maxdepth 1 -type f -name 'linux-task-manager_*.deb' -exec cp -v {} "$ROOT/dist/" \;
echo "DEB package(s) written to $ROOT/dist"
