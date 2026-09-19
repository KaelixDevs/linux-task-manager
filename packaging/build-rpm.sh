#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=packaging/common.sh
source "$ROOT/packaging/common.sh"

VERSION="$(ltm_version "$ROOT")"
NAME="linux-task-manager"
TOP="$ROOT/.packaging/rpmbuild"
STAGE="$ROOT/.packaging/rpm-source"
SRC_DIR="$STAGE/$NAME-$VERSION"
NOW="$(date +%s)"

if ! ltm_require rpmbuild tar gzip; then
  echo >&2
  echo "Fedora: sudo dnf install rpm-build gcc meson ninja-build pkgconf-pkg-config gtk4-devel desktop-file-utils" >&2
  exit 127
fi

ltm_normalize_times "$ROOT"
rm -rf "$TOP" "$STAGE"
mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} "$SRC_DIR" "$ROOT/dist"

# Normalize timestamps while staging so tar/Meson cannot report clock skew.
(
  cd "$ROOT"
  tar --mtime="@$NOW" \
    --exclude='./.git' \
    --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.packaging' \
    -cf - .
) | tar -xf - -C "$SRC_DIR"
find "$SRC_DIR" -exec touch -d "@$NOW" {} +

tar --sort=name --mtime="@$NOW" -C "$STAGE" -czf "$TOP/SOURCES/$NAME-$VERSION.tar.gz" "$NAME-$VERSION"
sed "s/^Version:.*/Version:        $VERSION/" \
  "$ROOT/packaging/rpm/linux-task-manager.spec" > "$TOP/SPECS/linux-task-manager.spec"

rpmbuild --define "_topdir $TOP" -ba "$TOP/SPECS/linux-task-manager.spec"
find "$TOP/RPMS" -type f -name '*.rpm' -exec cp -v {} "$ROOT/dist/" \;

echo
echo "RPM package(s) written to $ROOT/dist"
