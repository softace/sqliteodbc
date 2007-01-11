; NSIS Config (http://nsis.sf.net)
;
; Run it with
;
;    .../makensis [-DWITH_SOURCES] this-file.nsi
;
; to create the installer sqliteodbc.exe
;
; If -DWITH_SOURCES is specified, source code is included.

; -------------------------------
; Start

BrandingText " "
Name "SQLite ODBC Driver"

!define PROD_NAME  "SQLite ODBC Driver"
!define PROD_NAME0 "SQLite ODBC Driver"
CRCCheck On
!include "MUI.nsh"
!include "Sections.nsh"
 
;--------------------------------
; General
 
OutFile "sqliteodbc.exe"
 
;--------------------------------
; Folder selection page
 
InstallDir "$PROGRAMFILES\${PROD_NAME0}"
 
;--------------------------------
; Modern UI Configuration

!define MUI_ICON "sqliteodbc.ico"
!define MUI_UNICON "sqliteodbc.ico" 
!define MUI_WELCOMEPAGE_TITLE "SQLite ODBC Installation"
!define MUI_WELCOMEPAGE_TEXT "This program will guide you through the \
installation of SQLite ODBC Driver.\r\n\r\n$_CLICK"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "license.txt"
!insertmacro MUI_PAGE_DIRECTORY
!ifdef WITH_SOURCES
!insertmacro MUI_PAGE_COMPONENTS
!endif
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "SQLite ODBC Installation"  
!define MUI_FINISHPAGE_TEXT "The installation of SQLite ODBC Driver is complete.\
\r\n\r\n$_CLICK"

!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
 
;--------------------------------
; Language
 
!insertmacro MUI_LANGUAGE "English"
 
;--------------------------------
; Installer Sections

Section "-Main (required)" InstallationInfo
 
; Add files
 SetOutPath "$INSTDIR"
 File "sqliteodbc.dll"
 File "sqliteodbcu.dll"
 File "sqlite3odbc.dll"
 File "sqlite.exe"
 File "sqliteu.exe"
 File "sqlite3.exe"
 File "inst.exe"
 File "instq.exe"
 File "uninst.exe"
 File "uninstq.exe"
 File "adddsn.exe"
 File "remdsn.exe"
 File "addsysdsn.exe"
 File "remsysdsn.exe"
 File "SQLiteODBCInstaller.exe"
 File "license.terms"
 File "license.txt"
 File "README"
 File "readme.txt"

; Shortcuts
 SetOutPath "$SMPROGRAMS\${PROD_NAME0}"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\Re-install ODBC Drivers.lnk" \
   "$INSTDIR\inst.exe"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\Remove ODBC Drivers.lnk" \
   "$INSTDIR\uninst.exe"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\Uninstall.lnk" \
   "$INSTDIR\uninstall.exe"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\View README.lnk" \
   "$INSTDIR\readme.txt"
 SetOutPath "$SMPROGRAMS\${PROD_NAME0}\Shells"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\Shells\SQLite 3.lnk" \
   "$INSTDIR\sqlite3.exe"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\Shells\SQLite 2.lnk" \
   "$INSTDIR\sqlite.exe"
 CreateShortCut "$SMPROGRAMS\${PROD_NAME0}\Shells\SQLite 2 (UTF-8).lnk" \
   "$INSTDIR\sqliteu.exe"
 
; Write uninstall information to the registry
 WriteRegStr HKLM \
  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PROD_NAME0}" \
  "DisplayName" "${PROD_NAME} (remove only)"
 WriteRegStr HKLM \
  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PROD_NAME0}" \
  "UninstallString" "$INSTDIR\Uninstall.exe"

 SetOutPath "$INSTDIR"
 WriteUninstaller "$INSTDIR\Uninstall.exe"

 ExecWait '"$INSTDIR\instq.exe"'

SectionEnd

!ifdef WITH_SOURCES
Section /o "Source Code" SourceInstall
 SetOutPath "$INSTDIR\source"
 File "README"
 File "VERSION"
 File "ChangeLog"
 File "license.terms"
 File "header.html"
 File "footer.html"
 File "stylesheet.css"
 File "doxygen.conf"
 File "adddsn.c"
 File "inst.c"
 File "fixup.c"
 File "mkopc.c"
 File "mkopc3.c"
 File "sqliteodbc.c"
 File "sqliteodbc.h"
 File "sqliteodbc.rc"
 File "sqliteodbc.mak"
 File "sqliteodbc.def"
 File "sqliteodbcu.def"
 File "sqliteodbc.spec"
 File "sqliteodbc.spec.in"
 File "sqlite3odbc.c"
 File "sqlite3odbc.h"
 File "sqlite3odbc.rc"
 File "sqlite3odbc.mak"
 File "sqlite3odbc.def"
 File "sqlite.mak"
 File "sqlite3.mak"
 File "resource.h.in"
 File "install-sh"
 File "Makefile.in"
 File "configure.in"
 File "config.guess"
 File "config.sub"
 File "ltmain.sh"
 File "libtool"
 File "aclocal.m4"
 File "configure"
 File "sqliteodbcos2.rc"
 File "sqliteodbcos2.def"
 File "makefile.os2"
 File "resourceos2.h"
 File "README.OS2"
 File "drvdsninst.sh"
 File "drvdsnuninst.sh"
 File "Makefile.mingw-cross"
 File "mf-sqlite.mingw-cross"
 File "mf-sqlite3.mingw-cross"
 File "mingw-cross-build.sh"
 File "sqliteodbc.nsi"
 File "SQLiteODBCInstaller.c"
 File /r "missing"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
 !insertmacro MUI_DESCRIPTION_TEXT ${SourceInstall} \
   "Source code"
!insertmacro MUI_FUNCTION_DESCRIPTION_END
!endif

;--------------------------------
; Uninstaller Section

Section "Uninstall"

ExecWait '"$INSTDIR\uninstq.exe"'
   
; Delete Files 
RMDir /r "$INSTDIR\*" 
RMDir /r "$INSTDIR\*.*" 
 
; Remove the installation directory
RMDir /r "$INSTDIR"

; Remove start menu/program files subdirectory

RMDir /r "$SMPROGRAMS\${PROD_NAME0}"
  
; Delete Uninstaller And Unistall Registry Entries
DeleteRegKey HKEY_LOCAL_MACHINE "SOFTWARE\${PROD_NAME0}"
DeleteRegKey HKEY_LOCAL_MACHINE \
    "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\${PROD_NAME0}"
  
SectionEnd
 
;--------------------------------
; EOF
