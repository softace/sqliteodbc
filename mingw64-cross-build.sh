#!/bin/sh
#
# Build script for cross compiling and packaging SQLite 3
# ODBC drivers and tools for Win32 using MinGW and NSIS.
# Tested on Fedora Core 3/5/8, Debian Etch, RHEL 5.
#
# Cross toolchain and NSIS for Linux/i386 can be fetched from
#  http://www.ch-werner.de/xtools/crossmingw64-0.1-1.i386.rpm
#  http://www.ch-werner.de/xtools/nsis-2.11-1.i386.rpm

set -e

VER3=3.6.14

if test -n "$SQLITE_DLLS" ; then
    export ADD_CFLAGS="-DWITHOUT_SHELL=1 -DWITH_SQLITE_DLLS=1"
    ADD_NSIS="-DWITH_SQLITE_DLLS"
fi

if test -n "$WITH_SOURCES" ; then
    ADD_NSIS="$ADD_NSIS -DWITH_SOURCES"
fi

echo "====================="
echo "Preparing sqlite3 ..."
echo "====================="
test -r sqlite-${VER3}.tar.gz || \
    wget -c http://www.sqlite.org/sqlite-${VER3}.tar.gz
test -r sqlite-${VER3}.tar.gz || exit 1

rm -f sqlite3
tar xzf sqlite-${VER3}.tar.gz
ln -sf sqlite-${VER3} sqlite3

patch sqlite3/main.mk <<'EOD'
--- sqlite3/main.mk.orig        2007-03-31 14:32:21.000000000 +0200
+++ sqlite3/main.mk     2007-04-02 11:04:50.000000000 +0200
@@ -67,7 +67,7 @@

 # All of the source code files.
 #
-SRC = \
+SRC += \
   $(TOP)/src/alter.c \
   $(TOP)/src/analyze.c \
   $(TOP)/src/attach.c \
EOD

# use open file dialog when no database name given
# need to link with -lcomdlg32 when enabled
true || patch sqlite3/src/shell.c <<'EOD'
--- sqlite3/src/shell.c.orig        2006-06-06 14:32:21.000000000 +0200
+++ sqlite3/src/shell.c     2006-07-23 11:04:50.000000000 +0200
@@ -21,6 +21,10 @@
 #include <ctype.h>
 #include <stdarg.h>
 
+#if defined(_WIN32) && defined(DRIVER_VER_INFO)
+# include <windows.h>
+#endif
+
 #if !defined(_WIN32) && !defined(WIN32) && !defined(__OS2__)
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
# SQLite 3.5.1 Win32 mutex fix
test "$VER3" != "3.5.1" || patch sqlite3/src/mutex_w32.c <<'EOD'
--- sqlite3/src/mutex_w32.c.orig	2007-08-30 14:10:30
+++ sqlite3/src/mutex_w32.c	2007-09-04 22:31:3
@@ -141,6 +141,12 @@
   p->nRef++;
 }
 int sqlite3_mutex_try(sqlite3_mutex *p){
+  /* The TryEnterCriticalSection() interface is not available on all
+  ** windows systems.  Since sqlite3_mutex_try() is only used as an
+  ** optimization, we can skip it on windows. */
+  return SQLITE_BUSY;
+
+#if 0  /* Not Available */
   int rc;
   assert( p );
   assert( p->id==SQLITE_MUTEX_RECURSIVE || sqlite3_mutex_notheld(p) );
@@ -152,6 +158,7 @@
     rc = SQLITE_BUSY;
   }
   return rc;
+#endif
 }
 
 /*

EOD

# same but new module libshell.c
cp -p sqlite3/src/shell.c sqlite3/src/libshell.c
patch sqlite3/src/libshell.c <<'EOD'
--- sqlite3/src/libshell.c.orig  2007-01-08 23:40:05.000000000 +0100
+++ sqlite3/src/libshell.c  2007-01-10 18:35:43.000000000 +0100
@@ -21,6 +21,10 @@
 #include <ctype.h>
 #include <stdarg.h>

+#ifdef _WIN32
+# include <windows.h>
+#endif
+
 #if !defined(_WIN32) && !defined(WIN32) && !defined(__OS2__)
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
+#if defined(_WIN32) && !defined(__TINYC__)
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

# amalgamation: add libshell.c 
test "$VER3" != "3.5.6" && test -r sqlite3/tool/mksqlite3c.tcl && patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/tool/mksqlite3c.tcl	2007-04-02 14:20:10.000000000 +0200
+++ sqlite3/tool/mksqlite3c.tcl	2007-04-03 09:42:03.000000000 +0200
@@ -194,6 +194,7 @@
    where.c
 
    parse.c
+   libshell.c

    tokenize.c
    complete.c
EOD
test "$VER3" = "3.5.6" && test -r sqlite3/tool/mksqlite3c.tcl && patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/tool/mksqlite3c.tcl	2007-04-02 14:20:10.000000000 +0200
+++ sqlite3/tool/mksqlite3c.tcl	2007-04-03 09:42:03.000000000 +0200
@@ -200,6 +200,7 @@
 
    main.c

+   libshell.c
    fts3.c
    fts3_hash.c
    fts3_porter.c
EOD

# patch: parse foreign key constraints on virtual tables
patch -d sqlite3 -p1 <<'EOD'
diff -u sqlite3.orig/src/build.c sqlite3/src/build.c
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
diff -u sqlite3.orig/src/pragma.c sqlite3/src/pragma.c
--- sqlite3.orig/src/pragma.c	2007-01-27 03:24:56.000000000 +0100
+++ sqlite3/src/pragma.c	2007-01-30 09:19:30.000000000 +0100
@@ -589,6 +589,9 @@
     pTab = sqlite3FindTable(db, zRight, zDb);
     if( pTab ){
       v = sqlite3GetVdbe(pParse);
+#ifndef SQLITE_OMIT_VIRTUAL_TABLE
+      if( pTab->pVtab ) sqlite3ViewGetColumnNames(pParse, pTab);
+#endif
       pFK = pTab->pFKey;
       if( pFK ){
         int i = 0; 
diff -u sqlite3.orig/src/vtab.c sqlite3/src/vtab.c
--- sqlite3.orig/src/vtab.c	2007-01-09 15:01:14.000000000 +0100
+++ sqlite3/src/vtab.c	2007-01-30 08:23:22.000000000 +0100
@@ -540,6 +540,9 @@
   int rc = SQLITE_OK;
   Table *pTab = db->pVTab;
   char *zErr = 0;
+#ifndef SQLITE_OMIT_FOREIGN_KEYS
+  FKey *pFKey;
+#endif
 
   sqlite3_mutex_enter(db->mutex);
   pTab = db->pVTab;
@@ -568,6 +571,15 @@
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
   if( sParse.pVdbe ){
     sqlite3VdbeFinalize(sParse.pVdbe);
   }
EOD

# patch: re-enable NO_TCL in tclsqlite.c (3.3.15)
patch -d sqlite3 -p1 <<'EOD'
diff -u sqlite3.orig/src/tclsqlite.c sqlite3/src/tclsqlite.c
--- sqlite3.orig/src/tclsqlite.c	2007-04-06 17:02:14.000000000 +0200
+++ sqlite3/src/tclsqlite.c	2007-04-10 07:47:49.000000000 +0200
@@ -14,6 +14,7 @@
 **
 ** $Id: mingw64-cross-build.sh,v 1.5 2009/05/11 06:28:00 chw Exp chw $
 */
+#ifndef NO_TCL     /* Omit this whole file if TCL is unavailable */
 #include "tcl.h"
 
 /*
@@ -2264,3 +2265,5 @@
   return 0;
 }
 #endif /* TCLSH */
+
+#endif /* !defined(NO_TCL) */
EOD

# patch: Win32 locking and pager unlock, for SQLite3 < 3.4.0
true || patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/src/os_win.c	2007-04-11 19:52:04.000000000 +0200
+++ sqlite3/src/os_win.c	2007-05-08 06:57:06.000000000 +0200
@@ -1237,8 +1237,8 @@
   ** the PENDING_LOCK byte is temporary.
   */
   newLocktype = pFile->locktype;
-  if( pFile->locktype==NO_LOCK
-   || (locktype==EXCLUSIVE_LOCK && pFile->locktype==RESERVED_LOCK)
+  if( locktype==SHARED_LOCK
+   || (locktype==EXCLUSIVE_LOCK && pFile->locktype<PENDING_LOCK)
   ){
     int cnt = 3;
     while( cnt-->0 && (res = LockFile(pFile->h, PENDING_BYTE, 0, 1, 0))==0 ){
@@ -1289,6 +1289,18 @@
       newLocktype = EXCLUSIVE_LOCK;
     }else{
       OSTRACE2("error-code = %d\n", GetLastError());
+      if( !getReadLock(pFile) ){
+        /* This should never happen.  We should always be able to
+        ** reacquire the read lock */
+        OSTRACE1("could not re-get a SHARED lock.\n");
+	if( newLocktype==PENDING_LOCK || pFile->locktype==PENDING_LOCK ){
+          UnlockFile(pFile->h, PENDING_BYTE, 0, 1, 0);
+        }
+        if( pFile->locktype==RESERVED_LOCK ){
+          UnlockFile(pFile->h, RESERVED_BYTE, 0, 1, 0);
+        }
+        newLocktype = NO_LOCK;
+      }
     }
   }
 
@@ -1362,6 +1374,7 @@
       /* This should never happen.  We should always be able to
       ** reacquire the read lock */
       rc = SQLITE_IOERR_UNLOCK;
+      locktype = NO_LOCK;
     }
   }
   if( type>=RESERVED_LOCK ){
EOD

# patch: Win32 locking and pager unlock, for SQLite3 >= 3.5.4 && <= 3.6.10
true || patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/src/os_win.c       2007-12-13 22:38:58.000000000 +0100
+++ sqlite3/src/os_win.c    2008-01-18 10:01:48.000000000 +0100
@@ -855,8 +855,8 @@
   ** the PENDING_LOCK byte is temporary.
   */
   newLocktype = pFile->locktype;
-  if( pFile->locktype==NO_LOCK
-   || (locktype==EXCLUSIVE_LOCK && pFile->locktype==RESERVED_LOCK)
+  if( locktype==SHARED_LOCK
+   || (locktype==EXCLUSIVE_LOCK && pFile->locktype<PENDING_LOCK)
   ){
     int cnt = 3;
     while( cnt-->0 && (res = LockFile(pFile->h, PENDING_BYTE, 0, 1, 0))==0 ){
@@ -907,7 +907,18 @@
       newLocktype = EXCLUSIVE_LOCK;
     }else{
       OSTRACE2("error-code = %d\n", GetLastError());
-      getReadLock(pFile);
+      if( !getReadLock(pFile) ){
+        /* This should never happen.  We should always be able to
+        ** reacquire the read lock */
+        OSTRACE1("could not re-get a SHARED lock.\n");
+        if( newLocktype==PENDING_LOCK || pFile->locktype==PENDING_LOCK ){
+          UnlockFile(pFile->h, PENDING_BYTE, 0, 1, 0);
+        }
+        if( pFile->locktype==RESERVED_LOCK ){
+          UnlockFile(pFile->h, RESERVED_BYTE, 0, 1, 0);
+        }
+        newLocktype = NO_LOCK;
+      }
     }
   }
 
@@ -982,6 +993,7 @@
       /* This should never happen.  We should always be able to
       ** reacquire the read lock */
       rc = SQLITE_IOERR_UNLOCK;
+      locktype = NO_LOCK;
     }
   }
   if( type>=RESERVED_LOCK ){
EOD

# patch: Win32 locking and pager unlock, for SQLite3 >= 3.6.11
patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/src/os_win.c    2009-02-15 14:07:09.000000000 +0100
+++ sqlite3/src/os_win.c    2009-02-20 16:39:48.000000000 +0100
@@ -922,7 +922,7 @@
   newLocktype = pFile->locktype;
   if(   (pFile->locktype==NO_LOCK)
      || (   (locktype==EXCLUSIVE_LOCK)
-         && (pFile->locktype==RESERVED_LOCK))
+         && (pFile->locktype<RESERVED_LOCK))
   ){
     int cnt = 3;
     while( cnt-->0 && (res = LockFile(pFile->h, PENDING_BYTE, 0, 1, 0))==0 ){
@@ -981,7 +981,18 @@
     }else{
       error = GetLastError();
       OSTRACE2("error-code = %d\n", error);
-      getReadLock(pFile);
+      if( !getReadLock(pFile) ){
+        /* This should never happen.  We should always be able to
+        ** reacquire the read lock */
+        OSTRACE1("could not re-get a SHARED lock.\n");
+        if( newLocktype==PENDING_LOCK || pFile->locktype==PENDING_LOCK ){
+          UnlockFile(pFile->h, PENDING_BYTE, 0, 1, 0);
+        }
+        if( pFile->locktype==RESERVED_LOCK ){
+          UnlockFile(pFile->h, RESERVED_BYTE, 0, 1, 0);
+        }
+        newLocktype = NO_LOCK;
+      }
     }
   }
 
@@ -1057,6 +1068,7 @@
       /* This should never happen.  We should always be able to
       ** reacquire the read lock */
       rc = SQLITE_IOERR_UNLOCK;
+      locktype = NO_LOCK;
     }
   }
   if( type>=RESERVED_LOCK ){
EOD

# patch: compile fix for FTS3 as extension module
patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/ext/fts3/fts3.c 2008-02-02 17:24:34.000000000 +0100
+++ sqlite3/ext/fts3/fts3.c      2008-03-16 11:29:02.000000000 +0100
@@ -274,10 +274,6 @@
 
 #if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)
 
-#if defined(SQLITE_ENABLE_FTS3) && !defined(SQLITE_CORE)
-# define SQLITE_CORE 1
-#endif
-
 #include <assert.h>
 #include <stdlib.h>
 #include <stdio.h>
@@ -6389,7 +6385,7 @@
   return rc;
 }
 
-#if !SQLITE_CORE
+#ifndef SQLITE_CORE
 int sqlite3_extension_init(
   sqlite3 *db, 
   char **pzErrMsg,
--- sqlite3.orig/ext/fts3/fts3_porter.c  2008-02-01 16:40:34.000000000 +0100
+++ sqlite3/ext/fts3/fts3_porter.c       2008-03-16 11:34:50.000000000 +0100
@@ -31,6 +31,11 @@
 #include <string.h>
 #include <ctype.h>
 
+#include "sqlite3ext.h"
+#ifndef SQLITE_CORE
+extern const sqlite3_api_routines *sqlite3_api;
+#endif
+
 #include "fts3_tokenizer.h"
 
 /*
--- sqlite3.orig/ext/fts3/fts3_tokenizer1.c      2007-11-23 18:31:18.000000000 +0100
+++ sqlite3/ext/fts3/fts3_tokenizer1.c   2008-03-16 11:35:37.000000000 +0100
@@ -31,6 +31,11 @@
 #include <string.h>
 #include <ctype.h>
 
+#include "sqlite3ext.h"
+#ifndef SQLITE_CORE
+extern const sqlite3_api_routines *sqlite3_api;
+#endif
+
 #include "fts3_tokenizer.h"
 
 typedef struct simple_tokenizer {
--- sqlite3.orig/ext/fts3/fts3_hash.c    2007-11-24 01:41:52.000000000 +0100
+++ sqlite3/ext/fts3/fts3_hash.c 2008-03-16 11:39:57.000000000 +0100
@@ -29,6 +29,11 @@
 #include <stdlib.h>
 #include <string.h>
 
+#include "sqlite3ext.h"
+#ifndef SQLITE_CORE
+extern const sqlite3_api_routines *sqlite3_api;
+#endif
+
 #include "sqlite3.h"
 #include "fts3_hash.h"
 
--- sqlite3.orig/ext/fts3/fts3_tokenizer.c	2008-06-24 03:29:58.000000000 +0200
+++ sqlite3/ext/fts3/fts3_tokenizer.c	2008-07-17 08:38:24.000000000 +0200
@@ -27,7 +27,7 @@
 
 #include "sqlite3ext.h"
 #ifndef SQLITE_CORE
-  SQLITE_EXTENSION_INIT1
+extern const sqlite3_api_routines *sqlite3_api;
 #endif
 
 #include "fts3_hash.h"
EOD

# patch: FTS3 again, for SQLite3 >= 3.6.8
test "$VER3" = "3.6.8" -o "$VER3" = "3.6.9" -o "$VER3" = "3.6.10" \
  -o "$VER3" = "3.6.11" -o "$VER3" = "3.6.12" -o "$VER3" = "3.6.13" \
  -o "$VER3" = "3.6.14" && \
  patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/ext/fts3/fts3_expr.c	2009-01-01 15:06:13.000000000 +0100
+++ sqlite3/ext/fts3/fts3_expr.c	2009-01-14 09:55:13.000000000 +0100
@@ -57,6 +57,12 @@
 #define SQLITE_FTS3_DEFAULT_NEAR_PARAM 10
 
 #include "fts3_expr.h"
+
+#include "sqlite3ext.h"
+#ifndef SQLITE_CORE
+extern const sqlite3_api_routines *sqlite3_api;
+#endif
+
 #include "sqlite3.h"
 #include <ctype.h>
 #include <string.h>
EOD
# patch: compile fix for rtree as extension module
patch -d sqlite3 -p1 <<'EOD'
--- sqlite3.orig/ext/rtree/rtree.c	2008-07-16 16:43:35.000000000 +0200
+++ sqlite3/ext/rtree/rtree.c	2008-07-17 08:59:53.000000000 +0200
@@ -2812,7 +2812,7 @@
   return rc;
 }
 
-#if !SQLITE_CORE
+#ifndef SQLITE_CORE
 int sqlite3_extension_init(
   sqlite3 *db,
   char **pzErrMsg,
EOD

echo "========================"
echo "Cleanup before build ..."
echo "========================"
make -f Makefile.mingw64-cross clean
make -C sqlite3 -f ../mf-sqlite3.mingw64-cross clean
make -C sqlite3 -f ../mf-sqlite3fts.mingw64-cross clean
make -C sqlite3 -f ../mf-sqlite3rtree.mingw64-cross clean

echo "====================="
echo "Building SQLite 3 ..."
echo "====================="
make -C sqlite3 -f ../mf-sqlite3.mingw64-cross all
test -r sqlite3/tool/mksqlite3c.tcl && \
  make -C sqlite3 -f ../mf-sqlite3.mingw64-cross sqlite3.c
if test -n "$SQLITE_DLLS" ; then
    make -C sqlite3 -f ../mf-sqlite3.mingw64-cross sqlite3.dll
fi

echo "==============================="
echo "Building ODBC drivers and utils"
echo "==============================="
make -f Makefile.mingw64-cross
make -f Makefile.mingw64-cross sqlite3odbcnw.dll

echo "==================================="
echo "Building SQLite3 FTS extensions ..."
echo "==================================="
make -C sqlite3 -f ../mf-sqlite3fts.mingw64-cross clean all
mv sqlite3/sqlite3_mod_fts*.dll .

echo "====================================="
echo "Building SQLite3 rtree extensions ..."
echo "====================================="
make -C sqlite3 -f ../mf-sqlite3rtree.mingw64-cross clean all
mv sqlite3/sqlite3_mod_rtree.dll .

echo "======================="
echo "Cleanup after build ..."
echo "======================="
make -C sqlite3 -f ../mf-sqlite3.mingw64-cross clean
make -C sqlite3 -f ../mf-sqlite3fts.mingw64-cross clean
make -C sqlite3 -f ../mf-sqlite3rtree.mingw64-cross clean

echo "==========================="
echo "Creating NSIS installer ..."
echo "==========================="
cp -p README readme.txt
unix2dos < license.terms > license.txt
makensis $ADD_NSIS sqliteodbc_w64.nsi

