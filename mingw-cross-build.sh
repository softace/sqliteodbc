#!/bin/sh
#
# Build script for cross compiling and packaging SQLite
# ODBC drivers using MinGW and NSIS.
# Tested on Fedora Core 3.
#
# Cross toolchain and NSIS for Linux/i386 can be fetched from
#  http://www.ch-werner.de/xtools/cross-mingw32-3.1-3.i386.rpm
#  http://www.ch-werner.de/xtools/nsis-2.11-1.i386.rpm

VER2=2.8.17
VER3=3.3.4

echo "===================="
echo "Preparing sqlite ..."
echo "===================="
test -r sqlite-${VER2}.tar.gz || \
    wget -c http://www.sqlite.org/sqlite-${VER2}.tar.gz
test -r sqlite-${VER2}.tar.gz || exit 1

rm -f sqlite
tar xzf sqlite-${VER2}.tar.gz
ln -sf sqlite-${VER2} sqlite

patch sqlite/main.mk <<'EOD'
--- sqlite/main.mk.orig	2005-04-24 00:43:23.000000000 +0200
+++ sqlite/main.mk	2006-03-16 14:29:55.000000000 +0100
@@ -55,7 +55,7 @@
 # Object files for the SQLite library.
 #
 LIBOBJ = attach.o auth.o btree.o btree_rb.o build.o copy.o date.o delete.o \
-         expr.o func.o hash.o insert.o \
+         expr.o func.o hash.o insert.o encode.o \
          main.o opcodes.o os.o pager.o parse.o pragma.o printf.o random.o \
          select.o table.o tokenize.o trigger.o update.o util.o \
          vacuum.o vdbe.o vdbeaux.o where.o tclsqlite.o
EOD
patch sqlite/src/shell.c <<'EOD'
--- sqlite/src/shell.c.orig	2005-04-24 00:43:22.000000000 +0200
+++ sqlite/src/shell.c	2006-03-18 09:07:30.000000000 +0100
@@ -1297,7 +1297,7 @@
     }else if( strcmp(z,"-echo")==0 ){
       data.echoOn = 1;
     }else if( strcmp(z,"-version")==0 ){
-      printf("%s\n", sqlite_version);
+      printf("%s\n", sqlite_libversion());
       return 1;
     }else if( strcmp(z,"-help")==0 ){
       usage(1);
@@ -1332,7 +1332,7 @@
       printf(
         "SQLite version %s\n"
         "Enter \".help\" for instructions\n",
-        sqlite_version
+        sqlite_libversion()
       );
       zHome = find_home_dir();
       if( zHome && (zHistory = malloc(strlen(zHome)+20))!=0 ){
EOD

echo "====================="
echo "Preparing sqlite3 ..."
echo "====================="
test -r sqlite-${VER3}.tar.gz || \
    wget -c http://www.sqlite.org/sqlite-${VER3}.tar.gz
test -r sqlite-${VER3}.tar.gz || exit 1

rm -f sqlite3
tar xzf sqlite-${VER3}.tar.gz
ln -sf sqlite-${VER3} sqlite3

echo "========================"
echo "Cleanup before build ..."
echo "========================"
make -f Makefile.mingw-cross clean
make -C sqlite -f ../mf-sqlite.mingw-cross clean
make -C sqlite3 -f ../mf-sqlite3.mingw-cross clean

echo "============================="
echo "Building SQLite 2 ... ISO8859"
echo "============================="
make -C sqlite -f ../mf-sqlite.mingw-cross

echo "====================="
echo "Building SQLite 3 ..."
echo "====================="
make -C sqlite3 -f ../mf-sqlite3.mingw-cross

echo "==============================="
echo "Building ODBC drivers and utils"
echo "==============================="
make -f Makefile.mingw-cross

echo "=========================="
echo "Building SQLite 2 ... UTF8"
echo "=========================="
make -C sqlite -f ../mf-sqlite.mingw-cross clean
make -C sqlite -f ../mf-sqlite.mingw-cross ENCODING=UTF8

echo "========================="
echo "Building drivers ... UTF8"
echo "========================="
make -f Makefile.mingw-cross sqliteodbcu.dll sqliteu.exe

echo "======================="
echo "Cleanup after build ..."
echo "======================="
make -C sqlite -f ../mf-sqlite.mingw-cross clean
rm -f sqlite/sqlite.exe
make -C sqlite3 -f ../mf-sqlite3.mingw-cross clean
rm -f sqlite/sqlite3.exe

echo "==========================="
echo "Creating NSIS installer ..."
echo "==========================="
cp -p README readme.txt
unix2dos < license.terms > license.txt
makensis sqliteodbc.nsi
