/*
 * SQLite ODBC Driver installer/uninstaller
 *
 * $Id: inst.c,v 1.1 2001/10/02 15:43:41 chw Exp chw $
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

int APIENTRY
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	LPSTR lpszCmdLine, int nCmdShow)
{
    char path[301], driver[300], attr[300], inst[400], *p;
    WORD pathmax = sizeof (path) - 1, pathlen;
    DWORD usecnt, remove;

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

