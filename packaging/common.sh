#!/usr/bin/env bash

ltm_version() {
  sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$1/meson.build" | head -n1
}

ltm_require() {
  local missing=0
  for cmd in "$@"; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      echo "Missing required command: $cmd" >&2
      missing=1
    fi
  done
  return "$missing"
}

ltm_normalize_times() {
  local root="$1"
  # Archive extraction can preserve timestamps from another timezone/clock.
  # Git ignores mtimes, so refreshing them is safe and prevents Meson/tar skew errors.
  find "$root" \
    -path "$root/.git" -prune -o \
    -path "$root/.packaging" -prune -o \
    -path "$root/dist" -prune -o \
    -type f -exec touch {} +
  find "$root" \
    -path "$root/.git" -prune -o \
    -path "$root/.packaging" -prune -o \
    -path "$root/dist" -prune -o \
    -type d -exec touch {} +
}

ltm_container_engine() {
  if command -v podman >/dev/null 2>&1; then
    printf '%s\n' podman
  elif command -v docker >/dev/null 2>&1; then
    printf '%s\n' docker
  else
    return 1
  fi
}

ltm_mount_suffix() {
  # Podman on SELinux hosts needs relabeling. Docker accepts plain :ro/:rw.
  if [[ "$1" == "podman" ]]; then
    printf '%s\n' ',Z'
  else
    printf '%s\n' ''
  fi
}
