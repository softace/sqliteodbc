#!/bin/sh
#
# Build script for cross compiling and packaging SQLite
# ODBC drivers and tools using MinGW and NSIS.
# Tested on Fedora Core 3 and 5.
#
# Cross toolchain and NSIS for Linux/i386 can be fetched from
#  http://www.ch-werner.de/xtools/cross-mingw32-3.1-3.i386.rpm
#  http://www.ch-werner.de/xtools/nsis-2.11-1.i386.rpm

set -e

VER2=2.8.17
VER3=3.3.13
TCCVER=0.9.23

echo "===================="
echo "Preparing sqlite ..."
echo "===================="
test -r sqlite-${VER2}.tar.gz || \
    wget -c http://www.sqlite.org/sqlite-${VER2}.tar.gz
test -r sqlite-${VER2}.tar.gz || exit 1

rm -f sqlite
tar xzf sqlite-${VER2}.tar.gz
ln -sf sqlite-${VER2} sqlite

# enable sqlite_encode_binary et.al.
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
# display encoding
patch sqlite/src/shell.c <<'EOD'
--- sqlite/src/shell.c.orig	2005-04-24 00:43:22.000000000 +0200
+++ sqlite/src/shell.c	2006-05-23 08:22:01.000000000 +0200
@@ -1180,6 +1180,7 @@
   "   -separator 'x'       set output field separator (|)\n"
   "   -nullvalue 'text'    set text string for NULL values\n"
   "   -version             show SQLite version\n"
+  "   -encoding            show SQLite encoding\n"
   "   -help                show this text, also show dot-commands\n"
 ;
 static void usage(int showDetail){
@@ -1297,7 +1298,10 @@
     }else if( strcmp(z,"-echo")==0 ){
       data.echoOn = 1;
     }else if( strcmp(z,"-version")==0 ){
-      printf("%s\n", sqlite_version);
+      printf("%s\n", sqlite_libversion());
+      return 1;
+    }else if( strcmp(z,"-encoding")==0 ){
+      printf("%s\n", sqlite_libencoding());
       return 1;
     }else if( strcmp(z,"-help")==0 ){
       usage(1);
@@ -1330,9 +1334,9 @@
       char *zHome;
       char *zHistory = 0;
       printf(
-        "SQLite version %s\n"
+        "SQLite version %s encoding %s\n"
         "Enter \".help\" for instructions\n",
-        sqlite_version
+        sqlite_libversion(), sqlite_libencoding()
       );
       zHome = find_home_dir();
       if( zHome && (zHistory = malloc(strlen(zHome)+20))!=0 ){
EOD
# use open file dialog when no database name given
# need to link with -lcomdlg32 when enabled
true || patch sqlite/src/shell.c <<'EOD'
--- sqlite/src/shell.c.orig        2006-07-23 11:18:13.000000000 +0200
+++ sqlite/src/shell.c     2006-07-23 11:30:26.000000000 +0200
@@ -20,6 +20,10 @@
 #include "sqlite.h"
 #include <ctype.h>
 
+#if defined(_WIN32) && defined(DRIVER_VER_INFO)
+# include <windows.h>
+#endif
+
 #if !defined(_WIN32) && !defined(WIN32) && !defined(__MACOS__)
 # include <signal.h>
 # include <pwd.h>
@@ -1246,6 +1250,17 @@
   if( i<argc ){
     data.zDbFilename = argv[i++];
   }else{
+#if defined(_WIN32) && defined(DRIVER_VER_INFO)
+    static OPENFILENAME ofn;
+    static char zDbFn[1024];
+    ofn.lStructSize = sizeof(ofn);
+    ofn.lpstrFile = (LPTSTR) zDbFn;
+    ofn.nMaxFile = sizeof(zDbFn);
+    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
+    if( GetOpenFileName(&ofn) ){
+      data.zDbFilename = zDbFn;
+    } else
+#endif
     data.zDbFilename = ":memory:";
   }
   if( i<argc ){
EOD

# same but new module libshell.c
patch sqlite/main.mk <<'EOD'
--- sqlite/main.mk.orig        2007-01-10 19:30:52.000000000 +0100
+++ sqlite/main.mk     2007-01-10 19:33:39.000000000 +0100
@@ -54,7 +54,7 @@
 
 # Object files for the SQLite library.
 #
-LIBOBJ = attach.o auth.o btree.o btree_rb.o build.o copy.o date.o delete.o \
+LIBOBJ += attach.o auth.o btree.o btree_rb.o build.o copy.o date.o delete.o \
          expr.o func.o hash.o insert.o encode.o \
          main.o opcodes.o os.o pager.o parse.o pragma.o printf.o random.o \
          select.o table.o tokenize.o trigger.o update.o util.o \
EOD
cp -p sqlite/src/shell.c sqlite/src/libshell.c
patch sqlite/src/libshell.c <<'EOD'
--- sqlite/src/libshell.c.orig  2007-01-10 19:13:01.000000000 +0100
+++ sqlite/src/libshell.c  2007-01-10 19:25:56.000000000 +0100
@@ -20,6 +20,10 @@
 #include "sqlite.h"
 #include <ctype.h>
 
+#ifdef _WIN32
+# include <windows.h>
+#endif
+
 #if !defined(_WIN32) && !defined(WIN32) && !defined(__MACOS__)
 # include <signal.h>
 # include <pwd.h>
@@ -1205,7 +1209,7 @@
   strcpy(continuePrompt,"   ...> ");
 }
 
-int main(int argc, char **argv){
+int sqlite_main(int argc, char **argv){
   char *zErrMsg = 0;
   struct callback_data data;
   const char *zInitFile = 0;
@@ -1246,6 +1250,17 @@
   if( i<argc ){
     data.zDbFilename = argv[i++];
   }else{
+#ifdef _WIN32
+    static OPENFILENAME ofn;
+    static char zDbFn[1024];
+    ofn.lStructSize = sizeof(ofn);
+    ofn.lpstrFile = (LPTSTR) zDbFn;
+    ofn.nMaxFile = sizeof(zDbFn);
+    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
+    if( GetOpenFileName(&ofn) ){
+      data.zDbFilename = zDbFn;
+    } else
+#endif
     data.zDbFilename = ":memory:";
   }
   if( i<argc ){
EOD
rm -f sqlite/src/minshell.c
touch sqlite/src/minshell.c
patch sqlite/src/minshell.c <<'EOD'
--- sqlite/src/minshell.c.orig  2007-01-10 18:46:47.000000000 +0100
+++ sqlite/src/minshell.c  2007-01-10 18:46:47.000000000 +0100
@@ -0,0 +1,20 @@
+/*
+** 2001 September 15
+**
+** The author disclaims copyright to this source code.  In place of
+** a legal notice, here is a blessing:
+**
+**    May you do good and not evil.
+**    May you find forgiveness for yourself and forgive others.
+**    May you share freely, never taking more than you give.
+**
+*************************************************************************
+** This file contains code to implement the "sqlite" command line
+** utility for accessing SQLite databases.
+*/
+
+int sqlite_main(int argc, char **argv);
+
+int main(int argc, char **argv){
+  return sqlite_main(argc, argv);
+}
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

# use open file dialog when no database name given
# need to link with -lcomdlg32 when enabled
true || patch sqlite3/src/shell.c <<'EOD'
--- sqlite3/src/shell.c.orig        2006-06-06 14:32:21.000000000 +0200
+++ sqlite3/src/shell.c     2006-07-23 11:04:50.000000000 +0200
@@ -21,6 +21,10 @@
 #include "sqlite3.h"
 #include <ctype.h>
 
+#if defined(_WIN32) && defined(DRIVER_VER_INFO)
+# include <windows.h>
+#endif
+
 #if !defined(_WIN32) && !defined(WIN32) && !defined(__MACOS__)
 # include <signal.h>
 # include <pwd.h>
@@ -1676,6 +1676,17 @@
   if( i<argc ){
     data.zDbFilename = argv[i++];
   }else{
+#if defined(_WIN32) && defined(DRIVER_VER_INFO)
+    static OPENFILENAME ofn;
+    static char zDbFn[1024];
+    ofn.lStructSize = sizeof(ofn);
+    ofn.lpstrFile = (LPTSTR) zDbFn;
+    ofn.nMaxFile = sizeof(zDbFn);
+    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
+    if( GetOpenFileName(&ofn) ){
+      data.zDbFilename = zDbFn;
+    } else
+#endif
 #ifndef SQLITE_OMIT_MEMORYDB
     data.zDbFilename = ":memory:";
 #else
EOD

# same but new module libshell.c
cp -p sqlite3/src/shell.c sqlite3/src/libshell.c
patch sqlite3/src/libshell.c <<'EOD'
--- sqlite3/src/libshell.c.orig  2007-01-08 23:40:05.000000000 +0100
+++ sqlite3/src/libshell.c  2007-01-10 18:35:43.000000000 +0100
@@ -21,6 +21,10 @@
 #include "sqlite3.h"
 #include <ctype.h>
 
+#ifdef _WIN32
+# include <windows.h>
+#endif
+
 #if !defined(_WIN32) && !defined(WIN32) && !defined(__MACOS__) && !defined(__OS2__)
 # include <signal.h>
 # include <pwd.h>
@@ -1774,7 +1778,7 @@
   strcpy(continuePrompt,"   ...> ");
 }
 
-int main(int argc, char **argv){
+int sqlite3_main(int argc, char **argv){
   char *zErrMsg = 0;
   struct callback_data data;
   const char *zInitFile = 0;
@@ -1816,6 +1820,17 @@
   if( i<argc ){
     data.zDbFilename = argv[i++];
   }else{
+#ifdef _WIN32
+    static OPENFILENAME ofn;
+    static char zDbFn[1024];
+    ofn.lStructSize = sizeof(ofn);
+    ofn.lpstrFile = (LPTSTR) zDbFn;
+    ofn.nMaxFile = sizeof(zDbFn);
+    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
+    if( GetOpenFileName(&ofn) ){
+      data.zDbFilename = zDbFn;
+    } else
+#endif
 #ifndef SQLITE_OMIT_MEMORYDB
     data.zDbFilename = ":memory:";
 #else
EOD
rm -f sqlite3/src/minshell.c
touch sqlite3/src/minshell.c
patch sqlite3/src/minshell.c <<'EOD'
--- sqlite3/src/minshell.c.orig  2007-01-10 18:46:47.000000000 +0100
+++ sqlite3/src/minshell.c  2007-01-10 18:46:47.000000000 +0100
@@ -0,0 +1,20 @@
+/*
+** 2001 September 15
+**
+** The author disclaims copyright to this source code.  In place of
+** a legal notice, here is a blessing:
+**
+**    May you do good and not evil.
+**    May you find forgiveness for yourself and forgive others.
+**    May you share freely, never taking more than you give.
+**
+*************************************************************************
+** This file contains code to implement the "sqlite" command line
+** utility for accessing SQLite databases.
+*/
+
+int sqlite3_main(int argc, char **argv);
+
+int main(int argc, char **argv){
+  return sqlite3_main(argc, argv);
+}
EOD

# patch: parse foreign key constraints on virtual tables
patch -d sqlite3 -p1 <<'EOD'
diff -ur sqlite3.orig/src/build.c sqlite3/src/build.c
--- sqlite3.orig/src/build.c	2007-01-09 14:53:04.000000000 +0100
+++ sqlite3/src/build.c	2007-01-30 08:14:41.000000000 +0100
@@ -2063,7 +2063,7 @@
   char *z;
 
   assert( pTo!=0 );
-  if( p==0 || pParse->nErr || IN_DECLARE_VTAB ) goto fk_end;
+  if( p==0 || pParse->nErr ) goto fk_end;
   if( pFromCol==0 ){
     int iCol = p->nCol-1;
     if( iCol<0 ) goto fk_end;
diff -ur sqlite3.orig/src/pragma.c sqlite3/src/pragma.c
--- sqlite3.orig/src/pragma.c	2007-01-27 03:24:56.000000000 +0100
+++ sqlite3/src/pragma.c	2007-01-30 09:19:30.000000000 +0100
@@ -589,6 +589,9 @@
     pTab = sqlite3FindTable(db, zRight, zDb);
     if( pTab ){
       v = sqlite3GetVdbe(pParse);
+#ifndef SQLITE_OMIT_VIRTUAL_TABLE
+      if( pTab->isVirtual ) sqlite3ViewGetColumnNames(pParse, pTab);
+#endif
       pFK = pTab->pFKey;
       if( pFK ){
         int i = 0; 
diff -ur sqlite3.orig/src/vtab.c sqlite3/src/vtab.c
--- sqlite3.orig/src/vtab.c	2007-01-09 15:01:14.000000000 +0100
+++ sqlite3/src/vtab.c	2007-01-30 08:23:22.000000000 +0100
@@ -436,6 +436,9 @@
   int rc = SQLITE_OK;
   Table *pTab = db->pVTab;
   char *zErr = 0;
+#ifndef SQLITE_OMIT_FOREIGN_KEYS
+  FKey *pFKey;
+#endif
 
   if( !pTab ){
     sqlite3Error(db, SQLITE_MISUSE, 0);
@@ -464,6 +467,15 @@
   }
   sParse.declareVtab = 0;
 
+#ifndef SQLITE_OMIT_FOREIGN_KEYS
+  assert( pTab->pFKey==0 );
+  pTab->pFKey = sParse.pNewTable->pFKey;
+  sParse.pNewTable->pFKey = 0;
+  for(pFKey=pTab->pFKey; pFKey; pFKey=pFKey->pNextFrom){
+    pFKey->pFrom=pTab;
+  }
+#endif
+
   sqlite3_finalize((sqlite3_stmt*)sParse.pVdbe);
   sqlite3DeleteTable(0, sParse.pNewTable);
   sParse.pNewTable = 0;
EOD

echo "===================="
echo "Preparing TinyCC ..."
echo "===================="
test -r tcc-${TCCVER}.tar.gz || \
    wget -c http://fabrice.bellard.free.fr/tcc/tcc-${TCCVER}.tar.gz
test -r tcc-${TCCVER}.tar.gz || exit 1

rm -rf tcc tcc-${TCCVER}
tar xzf tcc-${TCCVER}.tar.gz
ln -sf tcc-${TCCVER} tcc
patch -d tcc -p1 < tcc-${TCCVER}.patch

echo "========================"
echo "Cleanup before build ..."
echo "========================"
make -f Makefile.mingw-cross clean
make -C sqlite -f ../mf-sqlite.mingw-cross clean
make -C sqlite3 -f ../mf-sqlite3.mingw-cross clean
make -C sqlite3 -f ../mf-sqlite3fts.mingw-cross clean

echo "============================="
echo "Building SQLite 2 ... ISO8859"
echo "============================="
make -C sqlite -f ../mf-sqlite.mingw-cross all

echo "====================="
echo "Building SQLite 3 ..."
echo "====================="
make -C sqlite3 -f ../mf-sqlite3.mingw-cross all

echo "==================="
echo "Building TinyCC ..."
echo "==================="
( cd tcc ; sh mingw-cross-build.sh )
# copy SQLite headers into TCC install include directory
cp -p sqlite/sqlite.h TCC/include
cp -p sqlite3/sqlite3.h sqlite3/src/sqlite3ext.h TCC/include
# copy LGPL to TCC install doc directory
cp -p tcc-${TCCVER}/COPYING TCC/doc

echo "==============================="
echo "Building ODBC drivers and utils"
echo "==============================="
make -f Makefile.mingw-cross

echo "=========================="
echo "Building SQLite 2 ... UTF8"
echo "=========================="
make -C sqlite -f ../mf-sqlite.mingw-cross clean
make -C sqlite -f ../mf-sqlite.mingw-cross ENCODING=UTF8 all

echo "========================="
echo "Building drivers ... UTF8"
echo "========================="
make -f Makefile.mingw-cross sqliteodbcu.dll sqliteu.exe

echo "==================================="
echo "Building SQLite3 FTS extensions ..."
echo "==================================="
make -C sqlite3 -f ../mf-sqlite3fts.mingw-cross all
mv sqlite3/sqlite3_mod_*.dll .

echo "============================"
echo "Building DLL import defs ..."
echo "============================"
# requires wine: create .def files with tiny_impdef.exe
# for all .dll files which provide SQLite
wine TCC/tiny_impdef.exe -p sqliteodbc.dll > TCC/lib/sqlite.def
wine TCC/tiny_impdef.exe -p sqliteodbcu.dll > TCC/lib/sqliteu.def
wine TCC/tiny_impdef.exe -p sqlite3odbc.dll > TCC/lib/sqlite3.def

echo "======================="
echo "Cleanup after build ..."
echo "======================="
make -C sqlite -f ../mf-sqlite.mingw-cross clean
rm -f sqlite/sqlite.exe
make -C sqlite3 -f ../mf-sqlite3.mingw-cross clean
rm -f sqlite/sqlite3.exe
make -C sqlite3 -f ../mf-sqlite3fts.mingw-cross clean

echo "==========================="
echo "Creating NSIS installer ..."
echo "==========================="
cp -p README readme.txt
unix2dos < license.terms > license.txt
unix2dos -k TCC/doc/COPYING
unix2dos -k TCC/doc/readme.txt
makensis sqliteodbc.nsi
