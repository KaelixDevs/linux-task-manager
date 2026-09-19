# Packaging

All package outputs are written to `dist/`.

## Fedora / RPM

On Fedora:

```bash
sudo dnf install rpm-build gcc meson ninja-build pkgconf-pkg-config gtk4-devel desktop-file-utils
./packaging/build-rpm.sh
```

## Debian / Ubuntu package

On Fedora, the script automatically uses Podman/Docker with Ubuntu 24.04 so the binary is not linked against Fedora's newer userspace:

```bash
sudo dnf install podman
./packaging/build-deb.sh
```

On Debian/Ubuntu it can build natively after installing:

```bash
sudo apt install build-essential meson ninja-build pkg-config libgtk-4-dev dpkg-dev
./packaging/build-deb.sh
```

## Arch package

On Fedora or another non-Arch distro, install Podman and run:

```bash
sudo dnf install podman
./packaging/build-arch.sh
```

The script builds inside `archlinux:latest`. On Arch itself, install the normal build dependencies and run the same script:

```bash
sudo pacman -S --needed base-devel meson ninja pkgconf gtk4
./packaging/build-arch.sh
```

## AppImage

On Fedora, use Podman and let the script build inside Ubuntu 24.04:

```bash
sudo dnf install podman
./packaging/appimage/build-appimage.sh
```

On Ubuntu 24.04, it can build natively after installing the dependencies listed by the script.

## Clean outputs

```bash
rm -rf dist .packaging build
```
