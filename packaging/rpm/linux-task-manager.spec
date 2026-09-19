Name:           linux-task-manager
Version:        0.3.1
Release:        1%{?dist}
Summary:        Native GTK4 task manager for Linux

License:        MIT
URL:            https://github.com/KaelixDevs/linux-task-manager
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(gtk4) >= 4.12
BuildRequires:  desktop-file-utils

Requires:       gtk4 >= 4.12
Recommends:     pciutils

%description
Linux Task Manager is a native GTK4 task manager written in C. It provides
process management and CPU, memory, GPU, network, disk, and system information.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.linuxtaskmanager.TaskManager.desktop

%files
%license LICENSE
%doc README.md CHANGELOG.md
%{_bindir}/linux-task-manager
%{_datadir}/applications/io.github.linuxtaskmanager.TaskManager.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.linuxtaskmanager.TaskManager.svg
%{_datadir}/metainfo/io.github.linuxtaskmanager.TaskManager.metainfo.xml

%changelog
* Sat Sep 19 2026 Linux Task Manager contributors - 0.3.1-1
- Initial RPM packaging
