#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v meson >/dev/null 2>&1; then
  echo "Meson is required. Install meson, ninja, gcc, pkg-config and GTK4 development headers first."
  exit 1
fi

# Refresh extracted timestamps so archives made in another timezone cannot trigger Meson clock-skew errors.
find "$SCRIPT_DIR" -type f -not -path '*/build/*' -exec touch {} +

meson setup build --prefix=/usr --buildtype=release --wipe
meson compile -C build
sudo meson install -C build

if command -v update-desktop-database >/dev/null 2>&1; then
  sudo update-desktop-database /usr/share/applications || true
fi
if command -v gtk4-update-icon-cache >/dev/null 2>&1; then
  sudo gtk4-update-icon-cache -f /usr/share/icons/hicolor || true
elif command -v gtk-update-icon-cache >/dev/null 2>&1; then
  sudo gtk-update-icon-cache -f /usr/share/icons/hicolor || true
fi

echo "Installed Linux Task Manager v0.3.1. Open your app menu and search for: Task Manager"
