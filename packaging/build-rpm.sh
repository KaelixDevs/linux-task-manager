#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -n1)"
NAME="linux-task-manager"
TOP="$ROOT/.packaging/rpmbuild"
SRC_DIR="$ROOT/.packaging/source/$NAME-$VERSION"

rm -rf "$TOP" "$ROOT/.packaging/source"
mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} "$SRC_DIR" "$ROOT/dist"

( cd "$ROOT" && tar --exclude='./.git' --exclude='./build' --exclude='./dist' --exclude='./.packaging' -cf - . ) \
  | tar -xf - -C "$SRC_DIR"

tar -C "$(dirname "$SRC_DIR")" -czf "$TOP/SOURCES/$NAME-$VERSION.tar.gz" "$NAME-$VERSION"
cp "$ROOT/packaging/rpm/linux-task-manager.spec" "$TOP/SPECS/"

rpmbuild --define "_topdir $TOP" -ba "$TOP/SPECS/linux-task-manager.spec"
find "$TOP/RPMS" -type f -name '*.rpm' -exec cp -v {} "$ROOT/dist/" \;

echo "RPM package(s) written to $ROOT/dist"
