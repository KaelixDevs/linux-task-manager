#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=packaging/common.sh
source "$ROOT/packaging/common.sh"

VERSION="$(ltm_version "$ROOT")"
NAME="linux-task-manager"

build_native() {
  local work="$ROOT/.packaging/arch"
  local stage="$ROOT/.packaging/arch-source"
  local src="$stage/$NAME-$VERSION"
  local sha now

  if [[ $EUID -eq 0 ]]; then
    echo "makepkg must run as a normal user." >&2
    exit 1
  fi
  if ! ltm_require makepkg meson ninja pkg-config gcc tar sha256sum; then
    echo >&2
    echo "Arch: sudo pacman -S --needed base-devel meson ninja pkgconf gtk4" >&2
    exit 127
  fi

  ltm_normalize_times "$ROOT"
  rm -rf "$work" "$stage"
  mkdir -p "$work" "$src" "$ROOT/dist"
  now="$(date +%s)"

  (
    cd "$ROOT"
    tar --mtime="@$now" \
      --exclude='./.git' \
      --exclude='./build' \
      --exclude='./dist' \
      --exclude='./.packaging' \
      -cf - .
  ) | tar -xf - -C "$src"
  find "$src" -exec touch -d "@$now" {} +

  tar --sort=name --mtime="@$now" -C "$stage" -czf "$work/$NAME-$VERSION.tar.gz" "$NAME-$VERSION"
  sha="$(sha256sum "$work/$NAME-$VERSION.tar.gz" | awk '{print $1}')"
  sed -e "s/__VERSION__/$VERSION/" -e "s/__SHA256__/$sha/" \
    "$ROOT/packaging/arch/PKGBUILD.in" > "$work/PKGBUILD"

  cd "$work"
  makepkg --cleanbuild --force --noconfirm
  cp -v ./*.pkg.tar.zst "$ROOT/dist/"
  echo
  echo "Arch package(s) written to $ROOT/dist"
}

build_container() {
  local engine suffix
  engine="$(ltm_container_engine)" || {
    echo "Building an Arch package outside Arch requires Podman or Docker." >&2
    echo "Fedora: sudo dnf install podman" >&2
    exit 127
  }
  suffix="$(ltm_mount_suffix "$engine")"
  mkdir -p "$ROOT/dist"

  echo "Building Arch package inside archlinux:latest using $engine..."
  "$engine" run --rm \
    -e LTM_CONTAINER_BUILD=1 \
    -v "$ROOT:/src:ro${suffix}" \
    -v "$ROOT/dist:/out:rw${suffix}" \
    archlinux:latest bash -lc '
      set -e
      pacman -Syu --noconfirm --needed base-devel meson ninja pkgconf gtk4
      useradd -m builder
      mkdir -p /work
      cp -a /src/. /work/
      chown -R builder:builder /work
      su builder -c "cd /work && ./packaging/build-arch.sh"
      cp -v /work/dist/*.pkg.tar.zst /out/
    '
}

if [[ "${LTM_CONTAINER_BUILD:-0}" == "1" ]] || command -v makepkg >/dev/null 2>&1; then
  build_native
else
  build_container
fi
