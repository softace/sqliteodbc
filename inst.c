/**
 * @file inst.c
 * SQLite ODBC Driver installer/uninstaller for WIN32
 *
 * $Id: inst.c,v 1.2 2002/06/04 10:07:02 chw Exp chw $
 *
 * Copyright (c) 2001,2002 Christian Werner <chw@ch-werner.de>
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
#include <stdio.h>

static char *DriverName	="SQLite ODBC Driver";
static char *DSName = "SQLite Datasource";
static char *DriverDLL = "sqliteodbc.dll";

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
 * Main function of installer/uninstaller.
 * This is the Win32 GUI main entry point.
 * It (un)registers the ODBC driver and deletes or
 * copies the driver DLL to the system folder.
 */

int APIENTRY
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	LPSTR lpszCmdLine, int nCmdShow)
{
    char path[301], driver[300], attr[300], inst[400], *p;
    WORD pathmax = sizeof (path) - 1, pathlen;
    DWORD usecnt, remove, mincnt;

    GetModuleFileName(NULL, path, sizeof (path));
    p = path;
    while (*p) {
	*p = tolower(*p);
	++p;
    }
    remove = strstr(path, "uninst") != NULL;
    if (SQLInstallDriverManager(path, pathmax, &pathlen)) {
	char *p;

	sprintf(driver, "%s;Driver=%s;Setup=%s;",
		DriverName, DriverDLL, DriverDLL);
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
		DriverName, path, DriverDLL, path, DriverDLL);
	p = driver;
	while (*p) {
	    if (*p == ';') {
		*p = '\0';
		++p;
	    }
	    ++p;
	}
	sprintf(inst, "%s\\%s", path, DriverDLL);
	mincnt = remove ? 1 : 0;
	while (usecnt != mincnt) {
	    if (!SQLRemoveDriver(driver, TRUE, &usecnt)) {
		break;
	    }
	}
	if (remove) {
	    if (!SQLRemoveDriver(driver, TRUE, &usecnt)) {
		ProcessErrorMessages("SQLRemoveDriver");
		exit(1);
	    }
	    if (!usecnt) {
		DeleteFile(inst);
		MessageBox(NULL, "SQLite ODBC Driver uninstalled.", "Info",
			   MB_ICONINFORMATION|MB_OK|MB_TASKMODAL|
			   MB_SETFOREGROUND);
	    }
	    sprintf(attr, "DSN=%s;Database=sqlite.db;", DSName);
	    p = attr;
	    while (*p) {
		if (*p == ';') {
		    *p = '\0';
		    ++p;
		}
		++p;
	    }
	    SQLConfigDataSource(NULL, ODBC_REMOVE_SYS_DSN, DriverName, attr);
	    exit(0);
	}
	if (!CopyFile(DriverDLL, inst, 0)) {
	    char buf[512];

	    sprintf(buf, "Copy %s to %s failed", DriverDLL, inst);
	    MessageBox(NULL, buf, "CopyFile",
		       MB_ICONSTOP|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND); 
	    exit(1);
	}
	if (!SQLInstallDriverEx(driver, path, path, pathmax, &pathlen,
				ODBC_INSTALL_COMPLETE, &usecnt)) {
	    ProcessErrorMessages("SQLInstallDriverEx");
	    exit(1);
	}
	sprintf(attr, "DSN=%s;Database=sqlite.db;", DSName);
	p = attr;
	while (*p) {
	    if (*p == ';') {
		*p = '\0';
		++p;
	    }
	    ++p;
	}
	SQLConfigDataSource(NULL, ODBC_REMOVE_SYS_DSN, DriverName, attr);
	if (!SQLConfigDataSource(NULL, ODBC_ADD_SYS_DSN, DriverName, attr)) {
	    ProcessErrorMessages("SQLConfigDataSource");
	    exit(1);
	}
    } else {
	ProcessErrorMessages("SQLInstallDriverManager");
	exit(1);
    }
    MessageBox(NULL, "SQLite ODBC Driver installed.", "Info",
	       MB_ICONINFORMATION|MB_OK|MB_TASKMODAL|MB_SETFOREGROUND);
    exit(0);
}

