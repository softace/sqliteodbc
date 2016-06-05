#!/bin/bash

MV=`grep "^VER_INFO\s*=" ../Makefile | tr -d ' \t'`
set -- $MV
eval "$1"
cat sqliteodbc.pkgproj.tmpl | sed "s/\${VERSION}/${VER_INFO}/g" >sqliteodbc.pkgproj
rm build/sqlite*.pkg
packagesbuild -v sqliteodbc.pkgproj
rm diskimage/sqlite*pkg
cp build/sqliteodbc-${VER_INFO}.pkg diskimage
rm sqliteodbc-${VER_INFO}.dmg
hdiutil create sqliteodbc-${VER_INFO}.dmg -volname sqliteodbc-${VER_INFO} -srcfolder diskimage

