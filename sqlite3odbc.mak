# VC++ 6 Makefile

CC=		cl
LN=		link
RC=		rc

!IF "$(DEBUG)" == "1"
LDEBUG=		/DEBUG
CDEBUG=		-Zi
!ELSE
LDEBUG=		/RELEASE
!ENDIF

CFLAGS=		-I. -Isqlite3 -Gs -GX -D_WIN32 -D_DLL -nologo $(CDEBUG) \
		-DHAVE_SQLITEATOF=1 
CFLAGSEXE=	-I. -Gs -GX -D_WIN32 -nologo $(CDEBUG)
DLLLFLAGS=	/NODEFAULTLIB $(LDEBUG) /NOLOGO /MACHINE:IX86 \
		/SUBSYSTEM:WINDOWS /DLL
DLLLIBS=	msvcrt.lib odbccp32.lib kernel32.lib \
		user32.lib comdlg32.lib sqlite3\libsqlite3.lib

DRVDLL=		sqlite3odbc.dll

OBJECTS=	sqlite3odbc.obj

.c.obj:
		$(CC) $(CFLAGS) /c $<

all:		$(DRVDLL) inst.exe uninst.exe adddsn.exe remdsn.exe \
		addsysdsn.exe remsysdsn.exe

clean:
		del *.obj
		del *.res
		del *.exp
		del *.ilk
		del *.pdb
		del *.res
		del resource.h
		del *.exe
		cd sqlite
		nmake -f ..\sqlite3.mak clean
		cd ..

uninst.exe:	inst.exe
		copy inst.exe uninst.exe

inst.exe:	inst.c
		$(CC) $(CFLAGSEXE) inst.c odbc32.lib odbccp32.lib \
		kernel32.lib user32.lib

remdsn.exe:	adddsn.exe
		copy adddsn.exe remdsn.exe

adddsn.exe:	adddsn.c
		$(CC) $(CFLAGSEXE) adddsn.c odbc32.lib odbccp32.lib \
		kernel32.lib user32.lib

remsysdsn.exe:	adddsn.exe
		copy adddsn.exe remsysdsn.exe

addsysdsn.exe:	adddsn.exe
		copy adddsn.exe addsysdsn.exe

fixup.exe:	fixup.c
		$(CC) $(CFLAGSEXE) fixup.c

mkopc.exe:	mkopc.c
		$(CC) $(CFLAGSEXE) mkopc.c

sqlite3odbc.c:	resource.h

sqlite3odbc.res:	sqlite3odbc.rc resource.h
		$(RC) -I. -Isqlite3 -fo sqlite3odbc.res -r sqlite3odbc.rc

sqlite3odbc.dll:	sqlite3\libsqlite3.lib $(OBJECTS) sqlite3odbc.res
		$(LN) $(DLLLFLAGS) $(OBJECTS) sqlite3odbc.res \
		-def:sqlite3odbc.def -out:$@ $(DLLLIBS)

VERSION_C:	VERSION
		.\fixup < VERSION > VERSION_C . ,

resource.h:	resource.h.in VERSION_C fixup.exe
		.\fixup < resource.h.in > resource.h \
		    --VERS-- @VERSION \
		    --VERS_C-- @VERSION_C

sqlite3\libsqlite3.lib:	fixup.exe mkopc.exe
		cd sqlite3
		nmake -f ..\sqlite3.mak
		cd ..
