# Linux Task Manager

Linux Task Manager is a native GTK4 task manager for Linux, written in C.

The goal is simple: give Linux a familiar desktop task manager with a clean process view and useful performance information, without turning it into an Electron app or a giant system utility suite.

## What it does

- Shows running applications and processes
- Groups duplicate processes under one expandable entry
- Hides most system/background processes in the default view
- Includes an Advanced View for the full process list
- Ends user-owned tasks from the process list
- Protects critical processes from accidental termination
- Shows CPU, memory, GPU, network, disk, kernel, architecture, and uptime information
- Uses Linux `/proc` and `/sys` interfaces for most system data
- Runs as a native GTK4 application

GPU information is best-effort. The exact data available depends on the GPU driver and what it exposes through Linux sysfs or installed tools.

## Building from source

### Fedora

```bash
sudo dnf install gcc meson ninja-build pkgconf-pkg-config gtk4-devel
meson setup build --buildtype=release
meson compile -C build
./build/linux-task-manager
```

### Debian / Ubuntu

You need GTK 4.12 or newer.

```bash
sudo apt install build-essential meson ninja-build pkg-config libgtk-4-dev
meson setup build --buildtype=release
meson compile -C build
./build/linux-task-manager
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel meson ninja pkgconf gtk4
meson setup build --buildtype=release
meson compile -C build
./build/linux-task-manager
```

## Installing from source

```bash
chmod +x install.sh
./install.sh
```

The installer builds the application with Meson and installs it under `/usr` so it appears in your desktop application menu.

## Packages

Packaging files are included for:

- Fedora/RHEL-style RPM packages
- Debian/Ubuntu DEB packages
- Arch Linux packages
- AppImage

See [`PACKAGING.md`](PACKAGING.md) for the build commands.

## Safety behavior

The normal process view intentionally hides most system services and background infrastructure. Advanced View shows more of the system, but critical processes are still protected from the End Task action.

This is a guardrail, not a security boundary. Linux permissions still decide which processes a user is allowed to signal.

## Project status

Linux Task Manager is still early in development. The process and performance pages work, but the UI and hardware reporting are still being expanded.

## License

MIT. See [`LICENSE`](LICENSE).
