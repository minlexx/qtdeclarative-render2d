Name:       qtdeclarative-render2d
Summary:    qtdeclarative-render2d
Version:    5.9
Release:    1
Group:      Qt/Qt
License:    LGPL
URL:        https://github.com/qt/qtdeclarative-render2d
Source0:    %{name}-%{version}.tar.bz2

Requires:	qt5-qt3d

BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  desktop-file-utils

%description


%package devel
Summary:        qtdeclarative-render2d - development files
Group:          Qt/Qt
Requires:       %{name} = %{version}-%{release}
Requires:       %{name}-devel = %{version}-%{release}

%description devel

%prep
%setup -q -n %{name}-%{version}

%build
%qmake5
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%qmake_install

%files
%defattr(-,root,root,-)
%{_libdir}/qt5/plugins/scenegraph/libsoftwarecontext.so

%files devel
%{_libdir}/cmake
 
