#ifndef _SQLITEODBC_H
#define _SQLITEODBC_H

/*
 * SQLite ODBC Driver
 *
 * $Id: sqliteodbc.h,v 1.9 2002/05/25 09:04:20 chw Exp chw $
 */

#ifdef _WIN32
#include <windows.h>
#include <string.h>
#include <sql.h>
#include <sqlext.h>
#define ASYNC 1
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sql.h>
#include <sqlext.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#define ASYNC 1
#endif
#endif
#include <ctype.h>

#include "sqlite.h"
#ifdef HAVE_IODBC
#include <iodbcinst.h>
#endif
#if defined(HAVE_UNIXODBC) || defined(_WIN32)
#include <odbcinst.h>
#endif

#ifndef SQL_API
#define SQL_API
#endif

struct dbc;
struct stmt;

typedef struct {
    int magic;			/* magic cookie */
    struct dbc *dbcs;		/* pointer to first DBC */
} ENV;

typedef struct dbc {
    int magic;			/* magic cookie */
    ENV *env;			/* pointer to environment */
    struct dbc *next;		/* pointer to next DBC */
    sqlite *sqlite;		/* SQLITE database handle */
#ifdef ASYNC
    sqlite *sqlite2;		/* ditto for thread */
#endif
    char *dbname;		/* SQLITE database name */
    char *dsn;			/* ODBC data source name */
    int autocommit;		/* Auto commit state */
    int intrans;		/* True when transaction started */
    struct stmt *stmt;		/* STMT list of this DBC */
    char sqlstate[6];		/* SQL state for SQLError() */
    SQLCHAR logmsg[1024];	/* message for SQLError() */
#ifdef ASYNC
    int curtype;		/* default cursor type */
    int thread_enable;		/* true when threading enabled */
    struct stmt *async_stmt;	/* async executing STMT */
    int async_run;		/* true when thread running */
    int async_stop;		/* true for thread stop requested */
    int async_done;		/* true when thread done */
    char *async_errp;		/* thread's sqlite error message */
    char **async_rows;		/* thread's one row result */
    int async_ncols;		/* ditto */
    int async_rownum;		/* current row number */
#ifdef HAVE_PTHREAD
    int async_cont;		/* true when thread should continue */
    pthread_t thr;		/* thread identifier */
    pthread_mutex_t mut;	/* mutex for condition */
    pthread_cond_t cond;	/* condition */
#endif
#ifdef _WIN32
    HANDLE thr;			/* thread identifier */
    HANDLE ev_res;		/* signal set when result available */
    HANDLE ev_cont;		/* signal set when thread should continue */
#endif
#endif
} DBC;

typedef struct {
    char *db;			/* database name */
    char *table;		/* table name */
    char *column;		/* column name */
    int type;			/* data type of column */
    int size;			/* size of column */
    int index;			/* index of column in result */
    int nosign;			/* unsigned type */
    int scale;			/* scale of column */
    int prec;			/* precision of column */
    char *typename;		/* column type name or NULL */
} COL;

typedef struct {
    SQLSMALLINT type;		/* ODBC type */
    SQLINTEGER max;		/* max size of value buffer */
    SQLINTEGER *lenp;		/* value return, actual size of value buffer */
    SQLPOINTER valp;		/* value buffer */
    int index;			/* index of column in result */
    int offs;			/* byte offset for SQLGetData() */
} BINDCOL;

typedef struct {
    int type, stype;		/* ODBC and SQL types */
    int max, *lenp;		/* max size, actual size of parameter buffer */
    void *param;		/* parameter buffer */
} BINDPARM;

typedef struct stmt {
    struct stmt *next;		/* linkage for STMT list in DBC */
    HDBC dbc;			/* pointer to DBC */
    SQLCHAR cursorname[32];	/* cursor name */
    SQLCHAR *query;		/* current query, raw string */
    int isselect;		/* true if query is a SELECT statement */
    int ncols;			/* number of result columns */
    COL *cols;			/* result column array */
    COL *dyncols;		/* ditto, but malloc()ed */
    int dcols;			/* number of entries in dyncols */
    BINDCOL *bindcols;		/* array of bound columns */
    int nbindparms;		/* number bound parameters */
    BINDPARM *bindparms;	/* array of bound parameters */
    int nparams;		/* number of parameters in query */
    int nrows;			/* number of result rows */
    int rowp;			/* current result row */
    char **rows;		/* 2 dim array, result set */
    void (*rowfree)();		/* free function for rows */
    char sqlstate[6];		/* SQL state for SQLError() */
    SQLCHAR logmsg[1024];	/* message for SQLError() */ 
#ifdef ASYNC
    int curtype;		/* cursor type */
    int *async_run;		/* true when async STMT running */
    int async_enable;		/* true when SQL_ASYNC_ENABLE */
#endif
} STMT;

#endif
