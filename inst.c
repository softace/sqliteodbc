/**
 * @file inst.c
 * SQLite ODBC Driver installer/uninstaller for WIN32
 *
 * $Id: inst.c,v 1.4 2004/06/23 08:53:40 chw Exp chw $
 *
 * Copyright (c) 2001-2003 Christian Werner <chw@ch-werner.de>
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

static char *DriverName[2] =
    { "SQLite ODBC Driver", "SQLite ODBC (UTF-8) Driver" };
static char *DSName[2] =
    { "SQLite Datasource", "SQLite UTF-8 Datasource" };
static char *DriverDLL[2] =
    { "sqliteodbc.dll", "sqliteodbcu.dll" };

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
 * Driver installer/uninstaller.
 * @param remove true for uninstall
 * @param drivername print name of driver
 * @param dllname file name of driver DLL
 * @param dsname name for data source
 */

static BOOL
InUn(int remove, char *drivername, char *dllname, char *dsname)
{
    char path[301], driver[300], attr[300], inst[400], *p;
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
		++p;
	    }
	    ++p;
	}
	SQLInstallDriverEx(driver, NULL, path, pathmax, &pathlen,
			   ODBC_INSTALL_INQUIRY, &usecnt);
	sprintf(driver, "%s;Driver=%s\\%s;Setup=%s\\%s;",
		drivername, path, dllname, path, dllname);
	p = driver;
	while (*p) {
	    if (*p == ';') {
		*p = '\0';
		++p;
	    }
	    ++p;
	}
	sprintf(inst, "%s\\%s", path, dllname);
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
		sprintf(buf, "%s uninstalled.", drivername);
		MessageBox(NULL, buf, "Info",
			   MB_ICONINFORMATION|MB_OK|MB_TASKMODAL|
			   MB_SETFOREGROUND);
	    }
	    sprintf(attr, "DSN=%s;Database=sqlite.db;", dsname);
	    p = attr;
	    while (*p) {
		if (*p == ';') {
		    *p = '\0';
		    ++p;
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
		++p;
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
    int i, remove, quiet;
    BOOL ret[2];

    GetModuleFileName(NULL, path, sizeof (path));
    p = path;
    while (*p) {
	*p = tolower(*p);
	++p;
    }
    p = strrchr(path, '\\');
    if (p == NULL) {
	p = path;
    }
    remove = strstr(p, "uninst") != NULL;
    quiet = strstr(p, "instq") != NULL;
    for (i = 0; i < 2; i++) {
	ret[i] = InUn(remove, DriverName[i], DriverDLL[i], DSName[i]);
    }
    if (!remove && (ret[0] || ret[1])) {
	if (!quiet) {
	    MessageBox(NULL, "SQLite ODBC Driver(s) installed.", "Info",
		       MB_ICONINFORMATION|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND);
	}
    }
    exit(0);
}

