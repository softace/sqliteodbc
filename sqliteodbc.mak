# VC++ 6 Makefile

CC=		cl
LN=		link
RC=		rc

CFLAGS=		-I. -Isqlite -Gs -GX -D_WIN32 -D_DLL -nologo -Zi
CFLAGSEXE=	-I. -Gs -GX -D_WIN32 -nologo
DLLLFLAGS=	/NODEFAULTLIB /RELEASE /NOLOGO /MACHINE:IX86 \
		/SUBSYSTEM:WINDOWS /DLL
DLLLIBS=	msvcrt.lib odbc32.lib odbccp32.lib kernel32.lib \
		user32.lib comdlg32.lib sqlite\libsqlite.lib

OBJECTS=	sqliteodbc.obj

.c.obj:
		$(CC) $(CFLAGS) /c $<

all:		sqliteodbc.dll inst.exe uninst.exe

uninst.exe:	inst.exe
		copy inst.exe uninst.exe

inst.exe:	inst.c
		$(CC) $(CFLAGSEXE) inst.c odbc32.lib odbccp32.lib \
		kernel32.lib user32.lib

fixup.exe:	fixup.c
		$(CC) $(CFLAGSEXE) fixup.c

mkopc.exe:	mkopc.c
		$(CC) $(CFLAGSEXE) mkopc.c

sqliteodbc.c:	resource.h

sqliteodbc.res:	sqliteodbc.rc resource.h
		$(RC) -I. -Isqlite -fo sqliteodbc.res -r sqliteodbc.rc

sqliteodbc.dll:	sqlite\libsqlite.lib $(OBJECTS) sqliteodbc.res
		$(LN) $(DLLLFLAGS) $(OBJECTS) sqliteodbc.res \
		-def:sqliteodbc.def -out:$@ $(DLLLIBS)

resource.h:	resource.h.in fixup.exe
		.\fixup < resource.h.in > resource.h \
		    --VERS-- @VERSION

sqlite\libsqlite.lib:	fixup.exe mkopc.exe
		cd sqlite
		nmake -f ..\sqlite.mak
		cd ..
