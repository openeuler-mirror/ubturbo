%global version    1.0.0
%global release_version 23
%global __strip /bin/true

%global build_subdir %{name}-%{version}

Name:          ubturbo
Version:       %{version}
Release:       %{release_version}
Summary:       ubturbo - hierarchical memory management framework
License:       MulanPSL2
URL:           https://gitee.com/openeuler/ubturbo.git
Source0:       %{name}-%{version}.tar.gz
Provides:      %{name}
BuildRoot:     %{buildroot}

# ─── Main framework + rmrs user-space build ───
BuildRequires: make
BuildRequires: gcc
BuildRequires: cmake
BuildRequires: ninja-build
BuildRequires: libboundscheck
BuildRequires: rapidjson
BuildRequires: libvirt libvirt-devel

# ─── Build helpers ───
BuildRequires: chrpath

# ─── Base tools ───
BuildRequires: coreutils

# Main package runtime dependencies (systemd macros auto-expand Requires(post/preun/postun))
Requires:        coreutils
Requires:        libboundscheck
Requires:        libvirt-libs
Requires(post):  shadow-utils
%{?systemd_requires}

%description
ubturbo is based on the hardware-enhanced hot and cold identification
capabilities, providing hierarchical memory management, including memory
migration, hot and cold data flow, etc, and accelerating application
performance.

This package contains the ubturbo framework core: the main executable
(ub_turbo_exec), the client SDK runtime library (libubturbo_client.so),
framework configuration, and the systemd service unit.

# ─── Path macros ───
%define systemd_unit_dir  /usr/lib/systemd/system
%define ubturbo_dir       /opt/ubturbo
%define ubturbo_bin_dir   /opt/ubturbo/bin
%define ubturbo_conf_dir  /opt/ubturbo/conf
%define ubturbo_log_dir   /var/log/ubturbo

%define debug_package %{nil}

# =============================================================================
# Subpackages
# =============================================================================

%package rmrs
Summary: ubturbo rmrs plugin and auxiliary scripts
Requires: ubturbo = %{version}-%{release}

%description rmrs
This package contains the rmrs plugin (librmrs_ubturbo_plugin.so) and its
configuration files.

# =============================================================================
# Prep & Build
# =============================================================================

%prep
%setup -q -T -b 0 -n %{build_subdir}

%build
# Build ubturbo framework + rmrs plugin (user-space only)
cd %{_builddir}/%{build_subdir} && bash -x build.sh -c

%install
rm -rf ${RPM_BUILD_ROOT}

# ─── Main package: ubturbo framework ───
mkdir -p -m 0750 ${RPM_BUILD_ROOT}%{ubturbo_dir}
mkdir -p -m 0700 ${RPM_BUILD_ROOT}%{ubturbo_bin_dir}
mkdir -p -m 0700 ${RPM_BUILD_ROOT}%{ubturbo_conf_dir}
mkdir -p -m 0755 ${RPM_BUILD_ROOT}%{_libdir}
mkdir -p -m 0755 ${RPM_BUILD_ROOT}%{systemd_unit_dir}
mkdir -p -m 0700 ${RPM_BUILD_ROOT}%{ubturbo_log_dir}

# ub_turbo_exec: 0500 ubturbo:ubturbo
install -m 0500 %{_builddir}/%{build_subdir}/dist/release/bin/ub_turbo_exec \
    ${RPM_BUILD_ROOT}%{ubturbo_bin_dir}/
# /usr/lib64/libubturbo_client.so: 0550 ubturbo:ubturbo (r-xr-x---)
# Install real file with version, then create symlinks:
#   libubturbo_client.so -> libubturbo_client.so.1 -> libubturbo_client.so.1.0.0
install -m 0550 %{_builddir}/%{build_subdir}/dist/release/lib/libubturbo_client.so \
    ${RPM_BUILD_ROOT}%{_libdir}/libubturbo_client.so.%{version}
ln -s libubturbo_client.so.%{version} \
    ${RPM_BUILD_ROOT}%{_libdir}/libubturbo_client.so.1
ln -s libubturbo_client.so.1 \
    ${RPM_BUILD_ROOT}%{_libdir}/libubturbo_client.so
# ubturbo.conf: 0600 ubturbo:ubturbo (rw-------)
install -m 0600 %{_builddir}/%{build_subdir}/dist/release/conf/ubturbo.conf \
    ${RPM_BUILD_ROOT}%{ubturbo_conf_dir}/
# ubturbo_plugin_admission.conf: 0600 ubturbo:ubturbo
install -m 0600 %{_builddir}/%{build_subdir}/dist/release/conf/ubturbo_plugin_admission.conf \
    ${RPM_BUILD_ROOT}%{ubturbo_conf_dir}/
# ubturbo.service: 0644 root:root
install -m 0644 %{_builddir}/%{build_subdir}/build/rpm/ubturbo.service \
    ${RPM_BUILD_ROOT}%{systemd_unit_dir}/

# ─── rmrs subpackage ───
# Install real file with version, then create symlinks:
#   librmrs_ubturbo_plugin.so -> librmrs_ubturbo_plugin.so.1 -> librmrs_ubturbo_plugin.so.1.0.0
install -m 0500 %{_builddir}/%{build_subdir}/dist/release/lib/librmrs_ubturbo_plugin.so \
    ${RPM_BUILD_ROOT}%{_libdir}/librmrs_ubturbo_plugin.so.%{version}
ln -s librmrs_ubturbo_plugin.so.%{version} \
    ${RPM_BUILD_ROOT}%{_libdir}/librmrs_ubturbo_plugin.so.1
ln -s librmrs_ubturbo_plugin.so.1 \
    ${RPM_BUILD_ROOT}%{_libdir}/librmrs_ubturbo_plugin.so
install -m 0600 %{_builddir}/%{build_subdir}/dist/release/conf/plugin_rmrs.conf \
    ${RPM_BUILD_ROOT}%{ubturbo_conf_dir}/

%clean
rm -rf ${RPM_BUILD_ROOT}

%files
%attr(0750,ubturbo,ubturbo) %dir %{ubturbo_dir}
%attr(0500,ubturbo,ubturbo) %dir %{ubturbo_bin_dir}
%attr(0700,ubturbo,ubturbo) %dir %{ubturbo_conf_dir}
%attr(0700,ubturbo,ubturbo) %dir %{ubturbo_log_dir}
%attr(0500,ubturbo,ubturbo) %{ubturbo_bin_dir}/ub_turbo_exec
%attr(0600,ubturbo,ubturbo) %{ubturbo_conf_dir}/ubturbo.conf
%attr(0600,ubturbo,ubturbo) %{ubturbo_conf_dir}/ubturbo_plugin_admission.conf
%attr(0550,ubturbo,ubturbo) %{_libdir}/libubturbo_client.so.%{version}
%{_libdir}/libubturbo_client.so.1
%{_libdir}/libubturbo_client.so
%attr(0644,root,root)      %{systemd_unit_dir}/ubturbo.service

%files rmrs
%attr(0500,ubturbo,ubturbo) %{_libdir}/librmrs_ubturbo_plugin.so.%{version}
%{_libdir}/librmrs_ubturbo_plugin.so.1
%{_libdir}/librmrs_ubturbo_plugin.so
%attr(0600,ubturbo,ubturbo) %{ubturbo_conf_dir}/plugin_rmrs.conf

# ─── Main package ubturbo ───
%pre
if command -v groupadd >/dev/null 2>&1; then
    getent group ubturbo >/dev/null || groupadd -r ubturbo
    getent passwd ubturbo >/dev/null || \
        useradd -r -g ubturbo -s /sbin/nologin -d %{ubturbo_dir} ubturbo
fi

%post
%systemd_post ubturbo.service

%preun
%systemd_preun ubturbo.service

%postun
%systemd_postun_with_restart ubturbo.service

# ─── rmrs subpackage ───
%post rmrs
if [ "$1" -ge 1 ]; then
    %systemd_postun_with_restart ubturbo.service
fi

%postun rmrs
if [ "$1" -ge 1 ]; then
    %systemd_postun_with_restart ubturbo.service
fi
