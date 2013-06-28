Name:       corewatcher
Summary:    A crash dump reporting tool
Version:    0.9.11
Release:    1
Group:      System/Base
License:	GPL-2.0
Source0:    corewatcher-%{version}.tar.gz
Source1001: corewatcher.manifest
Requires:   gdb
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(libsystemd-journal)
BuildRequires:  pkgconfig(libcurl)



%description
This tool sends application crash reports to the central crash
reporting database.


%prep
%setup -q -n %{name}-%{version}

%build
cp %{SOURCE1001} .
%reconfigure

make

%install
rm -rf %{buildroot}

%make_install

%install_service multi-user.target.wants corewatcher.service

%files
%manifest %{name}.manifest
%license COPYING
%defattr(-,root,root,-)
%manifest corewatcher.manifest
%config(noreplace) %{_sysconfdir}/corewatcher/corewatcher.conf
%config(noreplace) %{_sysconfdir}/security/limits.d/95-core.conf
%{_sysconfdir}/corewatcher/gdb.command
%{_sbindir}/corewatcher
%exclude /usr/share/man/man8/corewatcher.8.gz
%{_unitdir}/corewatcher.service
%{_unitdir}/multi-user.target.wants/corewatcher.service
