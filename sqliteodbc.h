#ifndef _SQLITEODBC_H
#define _SQLITEODBC_H

/**
 * @mainpage
 * @section readme README
 * @verbinclude README
 * @section changelog ChangeLog
 * @verbinclude ChangeLog
 * @section copying License Terms
 * @verbinclude license.terms
 */

/**
 * @file sqliteodbc.h
 * Header file for SQLite ODBC driver.
 *
 * $Id: sqliteodbc.h,v 1.22 2003/06/03 14:01:21 chw Exp chw $
 *
 * Copyright (c) 2001-2003 Christian Werner <chw@ch-werner.de>
 *
 * See the file "license.terms" for information on usage
 * and redistribution of this file and for a
 * DISCLAIMER OF ALL WARRANTIES.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#endif
#if defined(HAVE_LOCALECONV) || defined(_WIN32)
#include <locale.h>
#endif
#include <stdarg.h>
#include <string.h>
#include <sql.h>
#include <sqlext.h>
#include <ctype.h>

#ifdef _WIN32
#define ASYNC 1
#else
#ifdef HAVE_PTHREAD
#include <pthread.h>
#define ASYNC 1
#endif
#ifdef HAVE_PTH_UCTX
#ifdef HAVE_PTHREAD
#error "Conflict in configure"
#error "Please reconfigure using the switches"
#error "   --enable-threads     for pthreads"
#error "or"
#error "   --enable-pth         for GNU pth"
#endif
#include <pth.h>
#define ASYNC 1
#endif
#endif

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

/**
 * @typedef ENV
 * @struct ENV
 * Driver internal structure for environment (HENV).
 */

typedef struct {
    int magic;			/**< Magic cookie */
    int ov3;			/**< True for SQL_OV_ODBC3 */
    struct dbc *dbcs;		/**< Pointer to first DBC */
} ENV;

/**
 * @typedef DBC
 * @struct dbc
 * Driver internal structure for database connection (HDBC).
 */

typedef struct dbc {
    int magic;			/**< Magic cookie */
    ENV *env;			/**< Pointer to environment */
    struct dbc *next;		/**< Pointer to next DBC */
    sqlite *sqlite;		/**< SQLITE database handle */
#ifdef ASYNC
    sqlite *sqlite2;		/**< SQLITE handle for thread */
#endif
    int version;		/**< SQLITE version number */
    char *dbname;		/**< SQLITE database name */
    char *dsn;			/**< ODBC data source name */
    int timeout;		/**< Lock timeout value */
    long t0;			/**< Start time for SQLITE busy handler */
    int *ov3;			/**< True for SQL_OV_ODBC3 */
    int ov3val;			/**< True for SQL_OV_ODBC3 */
    int autocommit;		/**< Auto commit state */
    int intrans;		/**< True when transaction started */
    struct stmt *stmt;		/**< STMT list of this DBC */
    char sqlstate[6];		/**< SQL state for SQLError() */
    SQLCHAR logmsg[1024];	/**< Message for SQLError() */
    int nowchar;		/**< Don't try to use WCHAR */
#ifdef ASYNC
    int curtype;		/**< Default cursor type */
    int thread_enable;		/**< True when threading enabled */
    struct stmt *async_stmt;	/**< Async executing STMT */
    int async_run;		/**< True when thread running */
    int async_stop;		/**< True for thread stop requested */
    int async_done;		/**< True when thread done */
    char *async_errp;		/**< Thread's sqlite error message */
    char **async_rows;		/**< Thread's one row result */
    int async_ncols;		/**< Thread's one row result (# cols) */
    int async_rownum;		/**< Current row number */
#ifdef HAVE_PTHREAD
    int async_cont;		/**< True when thread should continue */
    pthread_t thr;		/**< Thread identifier */
    pthread_mutex_t mut;	/**< Mutex for condition */
    pthread_cond_t cond;	/**< Condition */
#endif
#ifdef HAVE_PTH_UCTX
    int async_cont;		/**< True when coroutine should continue */
    volatile pth_uctx_t uctx[2];	/**< GNU pth contexts */
#endif
#ifdef _WIN32
    HANDLE thr;			/**< Thread identifier */
    HANDLE ev_res;		/**< Signal set when result available */
    HANDLE ev_cont;		/**< Signal set when thread should continue */
#endif
#endif
} DBC;

/**
 * @typedef COL
 * @struct COL
 * Internal structure to describe a column in a result set.
 */

typedef struct {
    char *db;			/**< Database name */
    char *table;		/**< Table name */
    char *column;		/**< Column name */
    int type;			/**< Data type of column */
    int size;			/**< Size of column */
    int index;			/**< Index of column in result */
    int nosign;			/**< Unsigned type */
    int scale;			/**< Scale of column */
    int prec;			/**< Precision of column */
    char *typename;		/**< Column type name or NULL */
} COL;

/**
 * @typedef BINDCOL
 * @struct BINDCOL
 * Internal structure for bound column (SQLBindCol).
 */

typedef struct {
    SQLSMALLINT type;	/**< ODBC type */
    SQLINTEGER max;	/**< Max. size of value buffer */
    SQLINTEGER *lenp;	/**< Value return, actual size of value buffer */
    SQLPOINTER valp;	/**< Value buffer */
    int index;		/**< Index of column in result */
    int offs;		/**< Byte offset for SQLGetData() */
} BINDCOL;

/**
 * @typedef BINDPARM
 * @struct BINDPARM
 * Internal structure for bound parameter (SQLBindParam).
 */

typedef struct {
    int type, stype;	/**< ODBC and SQL types */
    int max, *lenp;	/**< Max. size, actual size of parameter buffer */
    void *param;	/**< Parameter buffer */
    void *ind;		/**< Indicator for SQL_LEN_DATA_AT_EXEC */
    int need;		/**< True when SQL_LEN_DATA_AT_EXEC */
    int offs, len;	/**< Offset/length for SQLParamData()/SQLPutData() */
} BINDPARM;

/**
 * @typedef STMT
 * @struct stmt
 * Driver internal structure representing SQL statement (HSTMT).
 */

typedef struct stmt {
    struct stmt *next;		/**< Linkage for STMT list in DBC */
    HDBC dbc;			/**< Pointer to DBC */
    SQLCHAR cursorname[32];	/**< Cursor name */
    SQLCHAR *query;		/**< Current query, raw string */
    int *ov3;			/**< True for SQL_OV_ODBC3 */
    int isselect;		/**< True if query is a SELECT statement */
    int ncols;			/**< Number of result columns */
    COL *cols;			/**< Result column array */
    COL *dyncols;		/**< Column array, but malloc()ed */
    int dcols;			/**< Number of entries in dyncols */
    BINDCOL *bindcols;		/**< Array of bound columns */
    int nbindcols;		/**< Number of entries in bindcols */
    int nbindparms;		/**< Number bound parameters */
    BINDPARM *bindparms;	/**< Array of bound parameters */
    int nparams;		/**< Number of parameters in query */
    int nrows;			/**< Number of result rows */
    int rowp;			/**< Current result row */
    char **rows;		/**< 2-dim array, result set */
    void (*rowfree)();		/**< Free function for rows */
    char sqlstate[6];		/**< SQL state for SQLError() */
    SQLCHAR logmsg[1024];	/**< Message for SQLError() */ 
    int nowchar;		/**< Don't try to use WCHAR */
    SQLUSMALLINT *row_status;	/**< Row status pointer */
    SQLUSMALLINT row_status0;	/**< Row status array */
    SQLUINTEGER *row_count;	/**< Row count pointer */
    SQLUINTEGER row_count0;	/**< Row count */
    /* Dummies to make ADO happy */
    SQLUINTEGER *bind_offs;	/**< SQL_ATTR_PARAM_BIND_OFFSET_PTR */
    SQLUSMALLINT *parm_oper;	/**< SQL_ATTR_PARAM_OPERATION_PTR */
    SQLUSMALLINT *parm_status;	/**< SQL_ATTR_PARAMS_STATUS_PTR */
    SQLUINTEGER *parm_proc;	/**< SQL_ATTR_PARAMS_PROCESSED_PTR */
#ifdef ASYNC
    int curtype;		/**< Cursor type */
    int *async_run;		/**< True when async STMT running */
    int async_enable;		/**< True when SQL_ASYNC_ENABLE */
#endif
} STMT;

#endif
