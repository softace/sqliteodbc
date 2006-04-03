%define name sqliteodbc
%define version 0.67
%define release 1

Name: %{name}
Summary: ODBC driver for SQLite
Version: %{version}
Release: %{release}
Source: http://www.ch-werner.de/sqliteodbc/%{name}-%{version}.tar.gz
Group: System/Libraries
URL: http://www.ch-werner.de/sqliteodbc
License: BSD
BuildRoot: %{_tmppath}/%{name}-%{version}-root

%description
ODBC driver for SQLite interfacing SQLite 2.x and/or 3.x using
unixODBC or iODBC. See http://www.sqlite.org for a description of
SQLite, http://www.unixodbc.org for a description of unixODBC.

%prep
%setup -q

%build
CFLAGS="%optflags" ./configure --prefix=%{_prefix}
make

%install
mkdir -p $RPM_BUILD_ROOT%{_prefix}/lib
make install prefix=$RPM_BUILD_ROOT%{_prefix}
rm -f $RPM_BUILD_ROOT%{_prefix}/lib/libsqliteodbc*.{a,la}
rm -f $RPM_BUILD_ROOT%{_prefix}/lib/libsqlite3odbc*.{a,la}

%clean
rm -rf $RPM_BUILD_ROOT

%post
if [ -x /usr/bin/odbcinst ] ; then
   INST=/tmp/sqliteinst$$
   if [ -r %{_libdir}/libsqliteodbc.so ] ; then
      cat > $INST << 'EOD'
[SQLITE]
Description=SQLite ODBC 2.X
Driver=%{_prefix}/lib/libsqliteodbc.so
Setup=%{_prefix}/lib/libsqliteodbc.so
FileUsage=1
EOD
      /usr/bin/odbcinst -q -d -n SQLITE | grep '^\[SQLITE\]' >/dev/null || {
	 /usr/bin/odbcinst -i -d -n SQLITE -f $INST || true
      }
      cat > $INST << 'EOD'
[SQLite Datasource]
Driver=SQLITE
EOD
      /usr/bin/odbcinst -q -s -n "SQLite Datasource" | \
	 grep '^\[SQLite Datasource\]' >/dev/null || {
	 /usr/bin/odbcinst -i -l -s -n "SQLite Datasource" -f $INST || true
      }
   fi
   if [ -r %{_libdir}/libsqlite3odbc.so ] ; then
      cat > $INST << 'EOD'
[SQLITE3]
Description=SQLite ODBC 3.X
Driver=%{_prefix}/lib/libsqlite3odbc.so
Setup=%{_prefix}/lib/libsqlite3odbc.so
FileUsage=1
EOD
      /usr/bin/odbcinst -q -d -n SQLITE3 | grep '^\[SQLITE3\]' >/dev/null || {
	 /usr/bin/odbcinst -i -d -n SQLITE3 -f $INST || true
      }
      cat > $INST << 'EOD'
[SQLite3 Datasource]
Driver=SQLITE3
EOD
      /usr/bin/odbcinst -q -s -n "SQLite3 Datasource" | \
	 grep '^\[SQLite3 Datasource\]' >/dev/null || {
	 /usr/bin/odbcinst -i -l -s -n "SQLite3 Datasource" -f $INST || true
      }
   fi
   rm -f $INST || true
fi

%preun
if [ "$1" = "0" ] ; then
    test -x /usr/bin/odbcinst && {
	/usr/bin/odbcinst -u -d -n SQLITE || true
	/usr/bin/odbcinst -u -l -s -n "SQLite Datasource" || true
	/usr/bin/odbcinst -u -d -n SQLITE3 || true
	/usr/bin/odbcinst -u -l -s -n "SQLite3 Datasource" || true
    }
    true
fi

%files
%defattr(-, root, root)
%doc README license.terms ChangeLog
%{_libdir}/*.so*

