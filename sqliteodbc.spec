%define name sqliteodbc
%define version 0.42
%define release 1

Name: %{name}
Summary: ODBC driver for SQLite
Version: %{version}
Release: %{release}
Source: %{name}-%{version}.tar.gz
Group: System/Libraries
URL: http://www.ch-werner.de/sqliteodbc
License: BSD
BuildRoot: %{_tmppath}/%{name}-%{version}-root

%description
ODBC driver for SQLite interfacing SQLite 2.x using unixODBC
or iODBC. See http://www.hwaci.com/sw/sqlite for a description of
SQLite, http://www.unixodbc.org for a description of unixODBC.

%prep
%setup -q

%build
CFLAGS="%optflags" ./configure --prefix=%{_prefix} --enable-threads
make

%install
mkdir -p $RPM_BUILD_ROOT%{_prefix}/lib
make install prefix=$RPM_BUILD_ROOT%{_prefix}

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-, root, root)
%doc README license.terms
%{_libdir}/*.so*

