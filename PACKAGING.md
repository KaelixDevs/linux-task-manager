# Packaging

All package output is written to `dist/` when using the included helper scripts.

## RPM

Build on Fedora or another RPM-based build environment:

```bash
sudo dnf install gcc meson ninja-build pkgconf-pkg-config gtk4-devel rpm-build desktop-file-utils
./packaging/build-rpm.sh
```

The resulting RPM will be copied into `dist/`.

## DEB

Build on Debian/Ubuntu with GTK 4.12 or newer:

```bash
sudo apt install build-essential debhelper meson ninja-build pkg-config libgtk-4-dev
./packaging/build-deb.sh
```

The resulting `.deb` will be copied into `dist/`.

## Arch Linux

```bash
sudo pacman -S --needed base-devel meson ninja pkgconf gtk4
./packaging/build-arch.sh
```

`makepkg` should be run as a normal user, not root.

The resulting `.pkg.tar.zst` will be copied into `dist/`.

## AppImage

The AppImage helper uses `linuxdeploy`. Run it on a recent x86_64 Linux system with GTK4 development files installed:

```bash
./packaging/appimage/build-appimage.sh
```

The script downloads `linuxdeploy` if it is not already present in `.packaging/tools/` and writes the AppImage to `dist/`.

AppImages bundle most userspace dependencies, but they still depend on some host system ABI compatibility. Test the AppImage on more than one distribution before publishing a release.

## GitHub releases

The repository includes a release workflow in `.github/workflows/release.yml`.

Create and push a version tag:

```bash
git tag v0.3.1
git push origin v0.3.1
```

GitHub Actions will build the package formats and attach them to a GitHub Release.
