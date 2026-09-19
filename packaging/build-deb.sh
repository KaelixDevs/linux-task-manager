#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=packaging/common.sh
source "$ROOT/packaging/common.sh"

VERSION="$(ltm_version "$ROOT")"
NAME="linux-task-manager"

build_native() {
  local build="$ROOT/.packaging/deb-build"
  local pkgroot="$ROOT/.packaging/deb-root"
  local arch
  local out

  if ! ltm_require meson ninja dpkg-deb gcc pkg-config; then
    echo >&2
    echo "Debian/Ubuntu: sudo apt install build-essential meson ninja-build pkg-config libgtk-4-dev dpkg-dev" >&2
    echo "Fedora: install Podman with: sudo dnf install podman" >&2
    exit 127
  fi

  ltm_normalize_times "$ROOT"
  rm -rf "$build" "$pkgroot"
  mkdir -p "$pkgroot/DEBIAN" "$ROOT/dist"

  meson setup "$build" "$ROOT" --prefix=/usr --buildtype=release
  meson compile -C "$build"
  DESTDIR="$pkgroot" meson install -C "$build"

  arch="$(dpkg --print-architecture 2>/dev/null || true)"
  if [[ -z "$arch" ]]; then
    case "$(uname -m)" in
      x86_64) arch=amd64 ;;
      aarch64|arm64) arch=arm64 ;;
      *) echo "Unsupported Debian architecture: $(uname -m)" >&2; exit 1 ;;
    esac
  fi

  mkdir -p "$pkgroot/usr/share/doc/$NAME"
  install -m644 "$ROOT/README.md" "$pkgroot/usr/share/doc/$NAME/README.md"
  install -m644 "$ROOT/CHANGELOG.md" "$pkgroot/usr/share/doc/$NAME/changelog"
  install -m644 "$ROOT/LICENSE" "$pkgroot/usr/share/doc/$NAME/copyright"

  cat > "$pkgroot/DEBIAN/control" <<CONTROL
Package: $NAME
Version: $VERSION-1
Section: utils
Priority: optional
Architecture: $arch
Maintainer: Linux Task Manager contributors <noreply@example.com>
Depends: libc6, libgtk-4-1 (>= 4.12)
Recommends: pciutils, mesa-utils, vulkan-tools
Installed-Size: $(du -sk "$pkgroot/usr" | awk '{print $1}')
Homepage: https://github.com/YOUR_GITHUB_USERNAME/linux-task-manager
Description: Native GTK4 task manager for Linux
 Linux Task Manager is a GTK4 task manager written in C. It provides process
 management and CPU, memory, GPU, network, disk, and system information.
CONTROL

  chmod 0755 "$pkgroot/DEBIAN"
  chmod 0644 "$pkgroot/DEBIAN/control"

  out="$ROOT/dist/${NAME}_${VERSION}-1_${arch}.deb"
  dpkg-deb --root-owner-group --build "$pkgroot" "$out"
  dpkg-deb --info "$out" >/dev/null
  echo
  echo "DEB package written to $out"
}

build_container() {
  local engine suffix
  engine="$(ltm_container_engine)" || {
    echo "Building a portable .deb from Fedora requires Podman or Docker." >&2
    echo "Fedora: sudo dnf install podman" >&2
    exit 127
  }
  suffix="$(ltm_mount_suffix "$engine")"
  mkdir -p "$ROOT/dist"

  echo "Building DEB inside Ubuntu 24.04 using $engine..."
  "$engine" run --rm \
    -e LTM_CONTAINER_BUILD=1 \
    -v "$ROOT:/src:ro${suffix}" \
    -v "$ROOT/dist:/out:rw${suffix}" \
    ubuntu:24.04 bash -lc '
      set -e
      export DEBIAN_FRONTEND=noninteractive
      apt-get update
      apt-get install -y --no-install-recommends build-essential meson ninja-build pkg-config libgtk-4-dev dpkg-dev ca-certificates
      mkdir -p /work
      cp -a /src/. /work/
      cd /work
      ./packaging/build-deb.sh
      cp -v dist/*.deb /out/
    '
}

if [[ "${LTM_CONTAINER_BUILD:-0}" == "1" ]]; then
  build_native
elif [[ -f /etc/os-release ]] && grep -Eq '^(ID|ID_LIKE)=.*(debian|ubuntu)' /etc/os-release; then
  build_native
else
  build_container
fi
