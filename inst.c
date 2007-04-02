/**
 * @file inst.c
 * SQLite ODBC Driver installer/uninstaller for WIN32
 *
 * $Id: inst.c,v 1.9 2007/03/22 13:05:36 chw Exp chw $
 *
 * Copyright (c) 2001-2006 Christian Werner <chw@ch-werner.de>
 *
 * See the file "license.terms" for information on usage
 * and redistribution of this file and for a
 * DISCLAIMER OF ALL WARRANTIES.
 */

#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <odbcinst.h>
#include <winver.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static char *DriverName[3] = {
    "SQLite ODBC Driver",
    "SQLite ODBC (UTF-8) Driver",
    "SQLite3 ODBC Driver"
};
static char *DSName[3] = {
    "SQLite Datasource",
    "SQLite UTF-8 Datasource",
    "SQLite3 Datasource"
};
static char *DriverDLL[3] = {
    "sqliteodbc.dll",
    "sqliteodbcu.dll",
    "sqlite3odbc.dll"
};

static int quiet = 0;

/**
 * Handler for ODBC installation error messages.
 * @param name name of API function for which to show error messages
 */

static BOOL
ProcessErrorMessages(char *name)
{
    WORD err = 1;
    DWORD code;
    char errmsg[301];
    WORD errlen, errmax = sizeof (errmsg) - 1;
    int rc;
    BOOL ret = FALSE;

    do {
	errmsg[0] = '\0';
	rc = SQLInstallerError(err, &code, errmsg, errmax, &errlen);
	if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
	    MessageBox(NULL, errmsg, name,
		       MB_ICONSTOP|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND);
	    ret = TRUE;
	}
	err++;
    } while (rc != SQL_NO_DATA);
    return ret;
}

/**
 * Copy or delete SQLite3 module DLLs
 * @param dllname file name of driver DLL
 * @param path install directory for modules
 * @param del flag, when true, delete DLLs in install directory
 */

static BOOL
CopyOrDelModules(char *dllname, char *path, BOOL del)
{
    char firstpat[MAX_PATH];
    WIN32_FIND_DATA fdata;
    HANDLE h;
    DWORD err;

    if (strncmp(dllname, "sqlite3", 7)) {
	return TRUE;
    }
    firstpat[0] = '\0';
    if (del) {
	strcpy(firstpat, path);
	strcat(firstpat, "\\");
    }
    strcat(firstpat, "sqlite3_mod*.dll");
    h = FindFirstFile(firstpat, &fdata);
    if (h == INVALID_HANDLE_VALUE) {
	return TRUE;
    }
    do {
	if (del) {
	    DeleteFile(fdata.cFileName);
	} else {
	    char buf[1024];

	    sprintf(buf, "%s\\%s", path, fdata.cFileName);
	    if (!CopyFile(fdata.cFileName, buf, 0)) {
		sprintf(buf, "Copy %s to %s failed", fdata.cFileName, path);
		MessageBox(NULL, buf, "CopyFile",
			   MB_ICONSTOP|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND); 
		FindClose(h);
		return FALSE;
	    }
	}
    } while (FindNextFile(h, &fdata));
    err = GetLastError();
    FindClose(h);
    return err == ERROR_NO_MORE_FILES;
}

/**
 * Driver installer/uninstaller.
 * @param remove true for uninstall
 * @param drivername print name of driver
 * @param dllname file name of driver DLL
 * @param dsname name for data source
 */

static BOOL
InUn(int remove, char *drivername, char *dllname, char *dsname)
{
    char path[301], driver[300], attr[300], inst[400];
    WORD pathmax = sizeof (path) - 1, pathlen;
    DWORD usecnt, mincnt;

    if (SQLInstallDriverManager(path, pathmax, &pathlen)) {
	char *p;

	sprintf(driver, "%s;Driver=%s;Setup=%s;",
		drivername, dllname, dllname);
	p = driver;
	while (*p) {
	    if (*p == ';') {
		*p = '\0';
	    }
	    ++p;
	}
	usecnt = 0;
	SQLInstallDriverEx(driver, NULL, path, pathmax, &pathlen,
			   ODBC_INSTALL_INQUIRY, &usecnt);
	sprintf(driver, "%s;Driver=%s\\%s;Setup=%s\\%s;",
		drivername, path, dllname, path, dllname);
	p = driver;
	while (*p) {
	    if (*p == ';') {
		*p = '\0';
	    }
	    ++p;
	}
	sprintf(inst, "%s\\%s", path, dllname);
	if (!remove && usecnt > 0) {
	    /* first install try: copy over driver dll, keeping DSNs */
	    if (GetFileAttributes(dllname) != 0xFFFFFFFF &&
		CopyFile(dllname, inst, 0) &&
		CopyOrDelModules(dllname, path, 0)) {
		return TRUE;
	    }
	}
	mincnt = remove ? 1 : 0;
	while (usecnt != mincnt) {
	    if (!SQLRemoveDriver(driver, TRUE, &usecnt)) {
		break;
	    }
	}
	if (remove) {
	    if (!SQLRemoveDriver(driver, TRUE, &usecnt)) {
		ProcessErrorMessages("SQLRemoveDriver");
		return FALSE;
	    }
	    if (!usecnt) {
		char buf[512];

		DeleteFile(inst);
		CopyOrDelModules(dllname, path, 1);
		if (!quiet) {
		    sprintf(buf, "%s uninstalled.", drivername);
		    MessageBox(NULL, buf, "Info",
			       MB_ICONINFORMATION|MB_OK|MB_TASKMODAL|
			       MB_SETFOREGROUND);
		}
	    }
	    sprintf(attr, "DSN=%s;Database=sqlite.db;", dsname);
	    p = attr;
	    while (*p) {
		if (*p == ';') {
		    *p = '\0';
		}
		++p;
	    }
	    SQLConfigDataSource(NULL, ODBC_REMOVE_SYS_DSN, drivername, attr);
	    return TRUE;
	}
	if (GetFileAttributes(dllname) == 0xFFFFFFFF) {
	    return FALSE;
	}
	if (!CopyFile(dllname, inst, 0)) {
	    char buf[512];

	    sprintf(buf, "Copy %s to %s failed", dllname, inst);
	    MessageBox(NULL, buf, "CopyFile",
		       MB_ICONSTOP|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND); 
	    return FALSE;
	}
	if (!CopyOrDelModules(dllname, path, 0)) {
	    return FALSE;
	}
	if (!SQLInstallDriverEx(driver, path, path, pathmax, &pathlen,
				ODBC_INSTALL_COMPLETE, &usecnt)) {
	    ProcessErrorMessages("SQLInstallDriverEx");
	    return FALSE;
	}
	sprintf(attr, "DSN=%s;Database=sqlite.db;", dsname);
	p = attr;
	while (*p) {
	    if (*p == ';') {
		*p = '\0';
	    }
	    ++p;
	}
	SQLConfigDataSource(NULL, ODBC_REMOVE_SYS_DSN, drivername, attr);
	if (!SQLConfigDataSource(NULL, ODBC_ADD_SYS_DSN, drivername, attr)) {
	    ProcessErrorMessages("SQLConfigDataSource");
	    return FALSE;
	}
    } else {
	ProcessErrorMessages("SQLInstallDriverManager");
	return FALSE;
    }
    return TRUE;
}

/**
 * Main function of installer/uninstaller.
 * This is the Win32 GUI main entry point.
 * It (un)registers the ODBC driver(s) and deletes or
 * copies the driver DLL(s) to the system folder.
 */

int APIENTRY
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	LPSTR lpszCmdLine, int nCmdShow)
{
    char path[300], *p;
    int i, remove;
    BOOL ret[3];

    GetModuleFileName(NULL, path, sizeof (path));
    p = path;
    while (*p) {
	*p = tolower(*p);
	++p;
    }
    p = strrchr(path, '\\');
    if (p == NULL) {
	p = path;
    } else {
	*p = '\0';
	++p;
	SetCurrentDirectory(path);
    }
    remove = strstr(p, "uninst") != NULL;
    quiet = strstr(p, "instq") != NULL;
    for (i = 0; i < 3; i++) {
	ret[i] = InUn(remove, DriverName[i], DriverDLL[i], DSName[i]);
    }
    if (!remove && (ret[0] || ret[1] || ret[2])) {
	if (!quiet) {
	    MessageBox(NULL, "SQLite ODBC Driver(s) installed.", "Info",
		       MB_ICONINFORMATION|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND);
	}
    }
    exit(0);
}

