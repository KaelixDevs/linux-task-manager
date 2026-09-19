#!/usr/bin/env bash
set -euo pipefail

if [[ $EUID -eq 0 ]]; then
  echo "makepkg must be run as a normal user, not root."
  exit 1
fi

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -n1)"
NAME="linux-task-manager"
WORK="$ROOT/.packaging/arch"
SRC="$WORK/$NAME-$VERSION"

rm -rf "$WORK"
mkdir -p "$SRC" "$ROOT/dist"

( cd "$ROOT" && tar --exclude='./.git' --exclude='./build' --exclude='./dist' --exclude='./.packaging' -cf - . ) \
  | tar -xf - -C "$SRC"

tar -C "$WORK" -czf "$WORK/$NAME-$VERSION.tar.gz" "$NAME-$VERSION"
SHA="$(sha256sum "$WORK/$NAME-$VERSION.tar.gz" | awk '{print $1}')"
sed "s/__SHA256__/$SHA/" "$ROOT/packaging/arch/PKGBUILD.in" > "$WORK/PKGBUILD"

cd "$WORK"
makepkg --clean --cleanbuild --force
cp -v ./*.pkg.tar.zst "$ROOT/dist/"

echo "Arch package(s) written to $ROOT/dist"
