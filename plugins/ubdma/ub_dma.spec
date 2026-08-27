%global ub_dma_version    1.0.0
%global release_version 1
%global os_version      oe2403sp1
%global release_os_version %{release_version}.%{os_version}

Name:           ub_dma
Version:        %{ub_dma_version}
Release:        %{release_os_version}
Summary:        openEuler UB-DMA driver
License:        GPLv2
URL:            https://www.openeuler.org
Source0:        ub_dma-1.0.0.tar.gz
Provides:       %{name}
Vendor:         openEuler Community
BuildRoot:      %{buildroot}
ExclusiveArch:  %arm64
BuildRequires:  kernel-devel >= 5.10.0-136.12.0.86 make >= 4.3 gcc >= 10.3.1
Requires:       kernel >= 5.10.0-136.12.0.86

%define ub_dma_dir /lib/modules/ub_dma

%description
This package contains the openEuler UB-DMA Driver

%prep
%setup -q -T -b 0 -c -n src

%build
cd %{_builddir}/src && make -j`nproc`

%install
echo "###########RPM_BUILD_ROOT=${RPM_BUILD_ROOT}"
rm -rf ${RPM_BUILD_ROOT}
mkdir -p -m755 ${RPM_BUILD_ROOT}/%{ub_dma_dir}
%{__install} -b -m 0500 %{_builddir}/src/ub_dma.ko ${RPM_BUILD_ROOT}/%{ub_dma_dir}

%clean
rm -rf ${RPM_BUILD_ROOT}

%files
%{ub_dma_dir}/ub_dma.ko

%preun
if [ "$1" = "0" ]; then
    modprobe -r ub_dma
fi

%postun
if [ "$1" = "0" ]; then
    rm -rf %{ub_dma_dir}
fi
depmod -a

%changelog