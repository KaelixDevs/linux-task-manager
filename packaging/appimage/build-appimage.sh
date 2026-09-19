#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=packaging/common.sh
source "$ROOT/packaging/common.sh"

VERSION="$(ltm_version "$ROOT")"

build_native() {
  local tools="$ROOT/.packaging/tools"
  local build="$ROOT/.packaging/appimage-build"
  local appdir="$ROOT/.packaging/AppDir"
  local linuxdeploy="$tools/linuxdeploy-x86_64.AppImage"
  local out="$ROOT/dist/Linux-Task-Manager-$VERSION-x86_64.AppImage"

  if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "The current AppImage builder is configured for x86_64 only." >&2
    exit 1
  fi
  if ! ltm_require meson ninja gcc pkg-config curl file patchelf; then
    echo >&2
    echo "Ubuntu: sudo apt install gcc meson ninja-build pkg-config libgtk-4-dev curl file patchelf" >&2
    echo "Fedora: use Podman: sudo dnf install podman" >&2
    exit 127
  fi

  ltm_normalize_times "$ROOT"
  mkdir -p "$tools" "$ROOT/dist"
  rm -rf "$build" "$appdir"
  rm -f "$out"

  if [[ ! -x "$linuxdeploy" ]]; then
    curl -L --fail --retry 3 --output "$linuxdeploy" \
      https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x "$linuxdeploy"
  fi

  meson setup "$build" "$ROOT" --prefix=/usr --buildtype=release
  meson compile -C "$build"
  DESTDIR="$appdir" meson install -C "$build"

  export APPIMAGE_EXTRACT_AND_RUN=1
  export LDAI_OUTPUT="$(basename "$out")"
  export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"

  (
    cd "$ROOT/dist"
    "$linuxdeploy" \
      --appdir "$appdir" \
      --executable "$appdir/usr/bin/linux-task-manager" \
      --desktop-file "$appdir/usr/share/applications/io.github.linuxtaskmanager.TaskManager.desktop" \
      --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.linuxtaskmanager.TaskManager.svg" \
      --output appimage
  )

  if [[ ! -f "$out" ]]; then
    echo "linuxdeploy finished but $out was not created." >&2
    echo "Files currently in dist/:" >&2
    ls -la "$ROOT/dist" >&2
    exit 1
  fi

  chmod +x "$out"
  echo
  echo "AppImage written to $out"
}

build_container() {
  local engine suffix
  engine="$(ltm_container_engine)" || {
    echo "For a portable AppImage, install Podman or Docker." >&2
    echo "Fedora: sudo dnf install podman" >&2
    exit 127
  }
  suffix="$(ltm_mount_suffix "$engine")"
  mkdir -p "$ROOT/dist"

  echo "Building AppImage inside Ubuntu 24.04 using $engine..."
  "$engine" run --rm \
    -e LTM_CONTAINER_BUILD=1 \
    -v "$ROOT:/src:ro${suffix}" \
    -v "$ROOT/dist:/out:rw${suffix}" \
    ubuntu:24.04 bash -lc '
      set -e
      export DEBIAN_FRONTEND=noninteractive
      apt-get update
      apt-get install -y --no-install-recommends gcc meson ninja-build pkg-config libgtk-4-dev curl ca-certificates file patchelf desktop-file-utils
      mkdir -p /work
      cp -a /src/. /work/
      cd /work
      ./packaging/appimage/build-appimage.sh
      cp -v dist/*.AppImage /out/
    '
}

if [[ "${LTM_CONTAINER_BUILD:-0}" == "1" ]]; then
  build_native
elif [[ -f /etc/os-release ]] && grep -Eq '^(ID|ID_LIKE)=.*(debian|ubuntu)' /etc/os-release; then
  build_native
else
  build_container
fi
