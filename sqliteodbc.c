/**
 * @file sqliteodbc.c
 * SQLite ODBC Driver main module.
 *
 * $Id: sqliteodbc.c,v 1.23 2002/06/09 08:36:23 chw Exp chw $
 *
 * Copyright (c) 2001,2002 Christian Werner <chw@ch-werner.de>
 *
 * See the file "license.terms" for information on usage
 * and redistribution of this file and for a
 * DISCLAIMER OF ALL WARRANTIES.
 */

#include "sqliteodbc.h"

#ifdef SQLITE_UTF8
#warning "SQLITE_UTF8 is currently unsupported."
#warning "The LIKE and GLOB operators may not work as expected."
#warning "The LENGTH() and SUBSTR() SQL functions may return wrong results."
#warning "Please consider to re-create the SQLite library with SQLITE_ISO8859."
#endif

#ifdef _WIN32
#define ODBC_INI "ODBC.INI"
#else
#define ODBC_INI ".odbc.ini"
#endif

#undef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#undef max
#define max(a, b) ((a) < (b) ? (b) : (a))

#define array_size(x) (sizeof (x) / sizeof (x[0]))

#define stringify1(s) #s
#define stringify(s) stringify1(s)

#define ENV_MAGIC  0x53544145
#define DBC_MAGIC  0x53544144
#define DEAD_MAGIC 0xdeadbeef

#ifdef MEMORY_DEBUG

static void *
xmalloc_(int n, char *file, int line)
{
    int nn = n + 4 * sizeof (long);
    long *p;

    p = malloc(nn);
    if (!p) {
#if (MEMORY_DEBUG > 1)
	fprintf(stderr, "malloc\t%d\tNULL\t%s:%d\n", n, file, line);
#endif
	return NULL;
    }
    p[0] = 0xdead1234;
    nn = nn / sizeof (long) - 1;
    p[1] = n;
    p[nn] = 0xdead5678;
#if (MEMORY_DEBUG > 1)
    fprintf(stderr, "malloc\t%d\t%p\t%s:%d\n", n, &p[2], file, line);
#endif
    return (void *) &p[2];
}

static void *
xrealloc_(void *old, int n, char *file, int line)
{
    int nn = n + 4 * sizeof (long), nnn;
    long *p, *pp;

    if (n == 0) {
	return xmalloc_(n, file, line);
    }
    p = &((long *) old)[-2];
    if (p[0] != 0xdead1234) {
	fprintf(stderr, "*** low end corruption @ %p\n", old);
	abort();
    }
    nnn = p[1] + 4 * sizeof (long);
    nnn = nnn / sizeof (long) - 1;
    if (p[nnn] != 0xdead5678) {
	fprintf(stderr, "*** high end corruption @ %p\n", old);
	abort();
    }
    pp = realloc(p, nn);
    if (!pp) {
#if (MEMORY_DEBUG > 1)
	fprintf(stderr, "realloc\t%p,%d\tNULL\t%s:%d\n", old, n, file, line);
#endif
	return NULL;
    }
#if (MEMORY_DEBUG > 1)
    fprintf(stderr, "realloc\t%p,%d\t%p\t%s:%d\n", old, n, &pp[2], file, line);
#endif
    p = pp;
    if (n > p[1]) {
	memset(p + p[1], 0, 3 * sizeof (long));
    }
    p[1] = n;
    nn = nn / sizeof (long) - 1;
    p[nn] = 0xdead5678;
    return (void *) &p[2];
}

static void
xfree_(void *x, char *file, int line)
{
    long *p;
    int n;

    p = &((long *) x)[-2];
    if (p[0] != 0xdead1234) {
	fprintf(stderr, "*** low end corruption @ %p\n", x);
	abort();
    }
    n = p[1] + 4 * sizeof (long);
    n = n / sizeof (long) - 1;
    if (p[n] != 0xdead5678) {
	fprintf(stderr, "*** high end corruption @ %p\n", x);
	abort();
    }
#if (MEMORY_DEBUG > 1)
    fprintf(stderr, "free\t%p\t\t%s:%d\n", x, file, line);
#endif
    free(p);
}

static char *
xstrdup_(char *str, char *file, int line)
{
    char *p;

    if (!str) {
#if (MEMORY_DEBUG > 1)
	fprintf(stderr, "strdup\tNULL\tNULL\t%s:%d\n", file, line);
#endif
	return NULL;
    }
    p = xmalloc_(strlen(str) + 1, file, line);
    if (p) {
	strcpy(p, str);
    }
#if (MEMORY_DEBUG > 1)
    fprintf(stderr, "strdup\t%p\t%p\t%s:%d\n", str, p, file, line);
#endif
    return p;
}

#define xmalloc(x)    xmalloc_(x, __FILE__, __LINE__)
#define xrealloc(x,y) xrealloc_(x, y, __FILE__, __LINE__)
#define xfree(x)      xfree_(x, __FILE__, __LINE__)
#define xstrdup(x)    xstrdup_(x, __FILE__, __LINE__)

#else

#define xmalloc(x)    malloc(x)
#define xrealloc(x,y) realloc(x, y)
#define xfree(x)      free(x)
#define xstrdup(x)    strdup_(x)

#endif

/**
 * Reset bound columns to unbound state.
 * @param s statement pointer
 */

static void unbindcols(STMT *s);

/**
 * Clear and re-allocate space for bound columns.
 * @param s statement pointer
 */

static void mkbindcols(STMT *s);

/**
 * Free statement's result.
 * @param s statement pointer
 * @param clrcols boolean flag
 *
 * The result rows are free'd using the rowfree function pointer.
 * If clrcols is true, then column bindings and dynamic column
 * descriptions are free'd, too.
 */

static void freeresult(STMT *s, int clrcols);

/**
 * Internal free function for HSTMT.
 * @param stmt statement handle
 * @result ODBC error code
 */

static SQLRETURN freestmt(HSTMT stmt);

/**
 * Substitute parameter for statement.
 * @param s statement pointer
 * @param pnum parameter number
 * @param out output buffer or NULL
 * @param size size indicator or NULL
 * @result ODBC error code
 *
 * If no output buffer is given, the function computes and
 * reports the space needed for the parameter. Otherwise
 * the parameter is converted to its string representation
 * in order to be presented to sqlite_exec_vprintf() et.al.
 */

static SQLRETURN substparam(STMT *s, int pnum, char **out, int *size);

/**
 * Free dynamically allocated column descriptions of STMT.
 * @param s statement pointer
 */

static void freedyncols(STMT *s);

/**
 * Duplicate string using xmalloc().
 * @param str string to be duplicated
 * @result pointer to new string or NULL
 */

static char *
strdup_(const char *str)
{
    char *p = NULL;

    if (str) {
	p = xmalloc(strlen(str) + 1);
	if (p) {
	    strcpy(p, str);
	}
    }
    return p;
}

/**
 * Report IM001 (not implemented) SQL error code for HDBC.
 * @param dbc database connection handle
 * @result ODBC error code
 */

static SQLRETURN
drvunimpldbc(HDBC dbc)
{
    DBC *d;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    strcpy(d->logmsg, "not supported");
    strcpy(d->sqlstate, "IM001");
    return SQL_ERROR;
}

/**
 * Report IM001 (not implemented) SQL error code for HSTMT.
 * @param stmt statement handle
 * @result ODBC error code
 */

static SQLRETURN
drvunimplstmt(HSTMT stmt)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    strcpy(s->logmsg, "not supported");
    strcpy(s->sqlstate, "IM001");
    return SQL_ERROR;
}

/**
 * Free memory given pointer to memory pointer.
 * @param x pointer to pointer to memory to be free'd
 */

static void
freep(void *x)
{
    if (x && ((char **) x)[0]) {
	xfree(((char **) x)[0]);
	((char **) x)[0] = NULL;
    }
}

/**
 * Report S1000 (out of memory) SQL error given STMT.
 * @param s statement pointer
 * @result ODBC error code
 */

static SQLRETURN
nomem(STMT *s)
{
    strcpy(s->logmsg, "out of memory");
    strcpy(s->sqlstate, "S1000");
    return SQL_ERROR;
}

/**
 * Report S1000 (not connected) SQL error given STMT.
 * @param s statement pointer
 * @result ODBC error code
 */

static SQLRETURN
noconn(STMT *s)
{
    strcpy(s->logmsg, "not connected");
    strcpy(s->sqlstate, "S1000");
    return SQL_ERROR;
}

/**
 * Set SQLite options (PRAGMAs) given SQLite handle.
 * @param x SQLite database handle
 *
 * "full_column_names" is always turned on to get the table
 * names in column labels. "count_changes" is always turned
 * on to get the number of affected rows for INSERTs et.al.
 * "empty_result_callbacks" is always turned on to get
 * column labels even when no rows are returned.
 */

static void
setsqliteopts(sqlite *x)
{
    sqlite_exec(x, "PRAGMA full_column_names = on;", NULL, NULL, NULL);
    sqlite_exec(x, "PRAGMA count_changes = on;", NULL, NULL, NULL);
    sqlite_exec(x, "PRAGMA empty_result_callbacks = on;", NULL, NULL, NULL);
}

/**
 * Free counted array of char pointers.
 * @param rowp pointer to char pointer array
 *
 * The -1-th element of the array holds the array size.
 * All non-NULL pointers of the array and then the array
 * itself are free'd.
 */

static void
freerows(char **rowp)
{
    int size, i;

    if (!rowp) {
	return;
    }
    --rowp;
    size = (int) rowp[0];
    for (i = 1; i <= size; i++) {
	freep(&rowp[i]);
    }
    freep(&rowp);
}

/**
 * Map SQL field type from string to ODBC integer type code.
 * @param typename field type string
 * @param nosign pointer to indicator for unsigned field or NULL
 * @result SQL data type
 */

static int
mapsqltype(char *typename, int *nosign)
{
    char *p, *q;
    int testsign = 0, result = SQL_VARCHAR;

    q = p = xmalloc(strlen(typename) + 1);
    if (!p) {
	return result;
    }
    strcpy(p, typename);
    while (*q) {
	*q = tolower((unsigned char) *q);
	++q;
    }
    if (strncmp(p, "inter", 5) == 0) {
    } else if (strncmp(p, "int", 3) == 0 ||
	strncmp(p, "mediumint", 9) == 0) {
	testsign = 1;
	result = SQL_INTEGER;
    } else if (strncmp(p, "tinyint", 7) == 0) {
	testsign = 1;
	result = SQL_TINYINT;
    } else if (strncmp(p, "smallint", 8) == 0) {
	testsign = 1;
	result = SQL_SMALLINT;
    } else if (strncmp(p, "float", 5) == 0) {
	result = SQL_FLOAT;
    } else if (strncmp(p, "double", 6) == 0 ||
	strncmp(p, "real", 4) == 0) {
	result = SQL_DOUBLE;
    } else if (strncmp(p, "timestamp", 9) == 0) {
	result = SQL_TIMESTAMP;
    } else if (strncmp(p, "datetime", 8) == 0) {
	result = SQL_TIMESTAMP;
    } else if (strncmp(p, "time", 4) == 0) {
	result = SQL_TIME;
    } else if (strncmp(p, "date", 4) == 0) {
	result = SQL_DATE;
#ifdef SQL_LONGVARCHAR
    } else if (strncmp(p, "text", 4) == 0) {
	result = SQL_LONGVARCHAR;
#endif
    }
    if (nosign) {
	if (testsign) {
	    *nosign = strstr(p, "unsigned") != NULL;
	} else {
	    *nosign = 1;
	}
    }
    xfree(p);
    return result;
}

/**
 * Get maximum display size and number of decimal points
 * from field type specification.
 * @param typename field type specification
 * @param sqltype target SQL data type
 * @param mp pointer to maximum display size or NULL
 * @param dp pointer to number of decimal points or NULL
 */

static void
getmd(char *typename, int sqltype, int *mp, int *dp)
{
    int m = 0, d = 0;

    switch (sqltype) {
    case SQL_INTEGER:     m = 10; d = 9; break;
    case SQL_TINYINT:     m = 4; d = 3; break;
    case SQL_SMALLINT:    m = 6; d = 5; break;
    case SQL_FLOAT:       m = 25; d = 24; break;
    case SQL_DOUBLE:      m = 54; d = 53; break;
    case SQL_VARCHAR:     m = 255; d = 0; break;
    case SQL_DATE:        m = 10; d = 0; break;
    case SQL_TIME:        m = 8; d = 0; break;
    case SQL_TIMESTAMP:   m = 32; d = 0; break;
#ifdef SQL_LONGVARCHAR
    case SQL_LONGVARCHAR: m = 65536; d = 0; break;
#endif
    }
    if (m) {
	int mm, dd;

	if (sscanf(typename, "%*[^(](%d)", &mm) == 1) {
	    m = d = mm;
	} else if (sscanf(typename, "%*[^(](%d,%d)", &mm, &dd) == 2) {
	    m = mm;
	    d = dd;
	}
    }
    if (mp) {
	*mp = m;
    }
    if (dp) {
	*dp = d;
    }
}

/**
 * Fixup query string with optional parameter markers.
 * @param sql original query string
 * @param sqlLen length of query string or SQL_NTS
 * @param nparam output number of parameters
 * @param isselect output indicator for SELECT statement
 * @param errmsg output error message
 * @result newly allocated string containing query string for SQLite or NULL
 */

static char *
fixupsql(char *sql, int sqlLen, int *nparam, int *isselect, char **errmsg)
{
    char *q = sql, *qz = NULL, *p;
    int inq = 0, np = 0;
    char *out;

    *errmsg = NULL;
    if (sqlLen != SQL_NTS) {
	qz = q = xmalloc(sqlLen + 1);
	if (!qz) {
	    return NULL;
	}
	memcpy(q, sql, sqlLen);
	q[sqlLen] = '\0';
	p = xmalloc(sqlLen * 4);
    } else {
	p = xmalloc(strlen(sql) * 4);
    }
    if (!p) {
errout:
	freep(&qz);
	return NULL;
    }
    out = p;
    while (*q) {
	switch (*q) {
	case '`':
	    if (!inq) {
		*p++ = '\'';
	    } else {
		*p++ = *q;
	    }
	    break;
	case '\'':
	    if (inq) {
		if (q[-1] != '\'') {
		    inq = 0;
		}
	    } else {
		inq = 1;
	    }
	    *p++ = *q;
	    break;
	case '?':
	    if (inq) {
		*p++ = *q;
	    } else {
		*p++ = '\'';
		*p++ = '%';
		*p++ = 'q';
		*p++ = '\'';
		np++;
	    }
	    break;
	case ';':
	    if (inq) {
		*p++ = *q;
	    } else {
		do {
		    ++q;
		} while (*q && isspace((unsigned char) *q));
		if (*q) {		
		    freep(&out);
		    *errmsg = "only one SQL statement allowed";
		    goto errout;
		}
		--q;
	    }
	    break;
	case '%':
	    *p++ = '%';
	    *p++ = '%';
	    break;
	case '{':
	    /* deal with {d 'YYYY-MM-DD'}, {t ...}, and {ts ...} */
	    if (!inq) {
		char *end = q + 1;

		while (*end && *end != '}') {
		    ++end;
		}
		if (*end == '}') {
		    char *start = q + 1;
		    char *end2 = end - 1;

		    while (start < end2 && *start != '\'') {
			++start;
		    }
		    while (end2 > start && *end2 != '\'') {
			--end2;
		    }
		    if (*start == '\'' && *end2 == '\'') {
			while (start <= end2) {
			    *p++ = *start;
			    ++start;
			}
			q = end;
			break;
		    }
		}
	    }
	    /* FALL THROUGH */
	default:
	    *p++ = *q;
	}
	++q;
    }
    freep(&qz);
    *p = '\0';
    if (nparam) {
	*nparam = np;
    }
    if (isselect) {
	p = out;
	while (*p && isspace((unsigned char) *p)) {
	    ++p;
	}
#ifdef _WIN32
	*isselect = _strnicmp(p, "select", 6) == 0;
#else
	*isselect = strncasecmp(p, "select", 6) == 0;
#endif
    }
    return out;
}

/**
 * Fixup column information for a running statement.
 * @param s statement to get fresh column information
 * @param sqlite SQLite database handle
 *
 * The "dyncols" field of STMT is filled with column
 * information obtained by SQLite "PRAGMA table_info"
 * for each column whose table name is known.
 */

static void
fixupdyncols(STMT *s, sqlite *sqlite)
{
    int i, k, r, nrows, ncols;
    char **rowp;

    if (!s->dyncols) {
	return;
    }
    for (i = 0; i < s->dcols; i++) {
	if (!s->dyncols[i].table[0]) {
	    continue;
	}
	if (sqlite_get_table_printf(sqlite,
				    "PRAGMA table_info('%q')", &rowp,
				    &nrows, &ncols, NULL,
				    s->dyncols[i].table) != SQLITE_OK) {
	    continue;
	}
	for (k = r = 0; k < ncols; k++) {
	    if (strcmp(rowp[k], "name") == 0) {
		for (r = 1; r <= nrows; r++) {
		    if (strcmp(s->dyncols[i].column, rowp[r * ncols + k])
			== 0) {
			goto found;
		    }
		}
		r = 0;
	    }
	}
found:
	if (r > 0) {
	    for (k = 0; k < ncols; k++) {
		if (strcmp(rowp[k], "type") == 0) {
		    char *typename = rowp[r * ncols + k];

		    s->dyncols[i].typename = xstrdup(typename);
		    s->dyncols[i].type =
			 mapsqltype(typename, &s->dyncols[i].nosign);
		    getmd(typename, s->dyncols[i].type,
			  &s->dyncols[i].size, NULL);
#ifdef SQL_LONGVARCHAR
		    if (s->dyncols[i].type == SQL_VARCHAR &&
			s->dyncols[i].size > 255) {
			s->dyncols[i].type = SQL_LONGVARCHAR;
		    }
#endif
		}
	    }
	}
	sqlite_free_table(rowp);
    }
}

/**
 * Convert string to ODBC DATE_STRUCT.
 * @param str string to be converted
 * @param ds output DATE_STRUCT
 * @result 0 on success, -1 on error
 *
 * Strings of the format 'YYYYMMDD' or 'YYYY-MM-DD' or
 * 'YYYY/MM/DD' are converted to a DATE_STRUCT.
 */

static int
str2date(char *str, DATE_STRUCT *ds)
{
    int i;
    char *p, *q;

    ds->year = ds->month = ds->day = 0;
    p = str;
    while (*p && !isdigit((unsigned char) *p)) {
	++p;
    }
    q = p;
    i = 0;
    while (*q && isdigit((unsigned char) *q)) {
	++i;
	++q;
    }
    if (i >= 8) {
	char buf[8];

	strncpy(buf, p + 0, 4); buf[4] = '\0';
	ds->year = strtol(buf, NULL, 10);
	strncpy(buf, p + 4, 2); buf[2] = '\0';
	ds->month = strtol(buf, NULL, 10);
	strncpy(buf, p + 6, 2); buf[2] = '\0';
	ds->day = strtol(buf, NULL, 10);
	return 0;
    }
    i = 0;
    while (i < 3) {
	int n;

	q = NULL; 
	n = strtol(p, &q, 10);
	if (!q || q == p) {
	    if (*q == '\0') {
		return i == 0 ? -1 : 0;
	    }
	}
	if (*q == '-' || *q == '/' || *q == '\0' || i == 2) {
	    switch (i) {
	    case 0: ds->year = n; break;
	    case 1: ds->month = n; break;
	    case 2: ds->day = n; break;
	    }
	    ++i;
	    if (*q) {
		++q;
	    }
	} else {
	    i = 0;
	    while (*q && !isdigit((unsigned char) *q)) {
		++q;
	    }
	}
	p = q;
    }
    return 0;
}

/**
 * Convert string to ODBC TIME_STRUCT.
 * @param str string to be converted
 * @param ts output TIME_STRUCT
 * @result 0 on success, -1 on error
 *
 * Strings of the format 'HHMMSS' or 'HH:MM:SS'
 * are converted to a TIME_STRUCT.
 */

static int
str2time(char *str, TIME_STRUCT *ts)
{
    int i;
    char *p, *q;

    ts->hour = ts->minute = ts->second = 0;
    p = str;
    while (*p && !isdigit((unsigned char) *p)) {
	++p;
    }
    q = p;
    i = 0;
    while (*q && isdigit((unsigned char) *q)) {
	++i;
	++q;
    }
    if (i >= 6) {
	char buf[4];

	strncpy(buf, p + 0, 2); buf[2] = '\0';
	ts->hour = strtol(buf, NULL, 10);
	strncpy(buf, p + 2, 2); buf[2] = '\0';
	ts->minute = strtol(buf, NULL, 10);
	strncpy(buf, p + 4, 2); buf[2] = '\0';
	ts->second = strtol(buf, NULL, 10);
	return 0;
    }
    i = 0;
    while (i < 3) {
	int n;

	q = NULL; 
	n = strtol(p, &q, 10);
	if (!q || q == p) {
	    if (*q == '\0') {
		return i == 0 ? -1 : 0;
	    }
	}
	if (*q == ':' || *q == '\0' || i == 2) {
	    switch (i) {
	    case 0: ts->hour = n; break;
	    case 1: ts->minute = n; break;
	    case 2: ts->second = n; break;
	    }
	    ++i;
	    if (*q) {
		++q;
	    }
	} else {
	    i = 0;
	    while (*q && !isdigit((unsigned char) *q)) {
		++q;
	    }
	}
	p = q;
    }
    return 0;
}

/**
 * Convert string to ODBC TIMESTAMP_STRUCT.
 * @param str string to be converted
 * @param tss output TIMESTAMP_STRUCT
 * @result 0 on success, -1 on error
 *
 * Strings of the format 'YYYYMMDDhhmmssff' or 'YYYY-MM-DD hh:mm:ss ff'
 * or 'YYYY/MM/DD hh:mm:ss ff' or 'hh:mm:ss ff YYYY-MM-DD' are
 * converted to a TIMESTAMP_STRUCT.
 */

static int
str2timestamp(char *str, TIMESTAMP_STRUCT *tss)
{
    int i, m, n;
    char *p, *q, in = '\0';

    tss->year = tss->month = tss->day = 0;
    tss->hour = tss->minute = tss->second = 0;
    tss->fraction = 0;
    p = str;
    while (*p && !isdigit((unsigned char) *p)) {
	++p;
    }
    q = p;
    i = 0;
    while (*q && isdigit((unsigned char) *q)) {
	++i;
	++q;
    }
    if (i >= 14) {
	char buf[16];

	strncpy(buf, p + 0, 4); buf[4] = '\0';
	tss->year = strtol(buf, NULL, 10);
	strncpy(buf, p + 4, 2); buf[2] = '\0';
	tss->month = strtol(buf, NULL, 10);
	strncpy(buf, p + 6, 2); buf[2] = '\0';
	tss->day = strtol(buf, NULL, 10);
	strncpy(buf, p + 8, 2); buf[2] = '\0';
	tss->hour = strtol(buf, NULL, 10);
	strncpy(buf, p + 10, 2); buf[2] = '\0';
	tss->minute = strtol(buf, NULL, 10);
	strncpy(buf, p + 12, 2); buf[2] = '\0';
	tss->second = strtol(buf, NULL, 10);
	if (i > 14) {
	    m = i - 14;
	    strncpy(buf, p + 14, m);
	    while (m < 9) {
		buf[m] = '0';
		++m;
	    }
	    buf[m] = '\0';
	    tss->fraction = strtol(buf, NULL, 0);
	}
	return 0;
    }
    m = i = 0;
    while (m != 7) {
	q = NULL; 
	n = strtol(p, &q, 10);
	if (!q || q == p) {
	    if (*q == '\0') {
		return m < 1 ? -1 : 0;
	    }
	}
	if (in == '\0') {
	    switch (*q) {
	    case '-':
	    case '/':
		if ((m & 1) == 0) {
		    in = *q;
		    i = 0;
		}
		break;
	    case ':':
		if ((m & 2) == 0) {
		    in = *q;
		    i = 0;
		}
		break;
	    case ' ':
	    case '.':
		break;
	    default:
		in = '\0';
		i = 0;
		break;
	    }
	}
	switch (in) {
	case '-':
	case '/':
	    switch (i) {
	    case 0: tss->year = n; break;
	    case 1: tss->month = n; break;
	    case 2: tss->day = n; break;
	    }
	    if (++i >= 3) {
		i = 0;
		m |= 1;
		goto skip;
	    } else {
		++q;
	    }
	    break;
	case ':':
	    switch (i) {
	    case 0: tss->hour = n; break;
	    case 1: tss->minute = n; break;
	    case 2: tss->second = n; break;
	    }
	    if (++i >= 3) {
		i = 0;
		m |= 2;
		if (*q == '.') {
		    in = '.';
		    goto skip2;
		}
		if (*q == ' ') {
		    if ((m & 1) == 0) {
			char *e = NULL;

			strtol(q + 1, &e, 10);
			if (e && *e == '-') {
			    goto skip;
			}
		    }
		    in = '.';
		    goto skip2;
		}
		goto skip;
	    } else {
		++q;
	    }
	    break;
	case '.':
	    if (++i >= 1) {
		int ndig = q - p;

		if (p[0] == '+' || p[0] == '-') {
		    ndig--;
		}
		while (ndig < 9) {
		    n = n * 10;
		    ++ndig;
		}
		tss->fraction = n;
		m |= 4;
		i = 0;
	    }
	default:
	skip:
	    in = '\0';
	skip2:
	    while (*q && !isdigit((unsigned char) *q)) {
		++q;
	    }
	}
	p = q;
    }
    return 0;
}

#ifdef ASYNC

/**
 * Get boolean flag from string.
 * @param string string to be inspected
 * @result true or false
 */

static int
getbool(char *string)
{
    if (string) {
	return strchr("Yy123456789Tt", string[0]) != NULL;
    }
    return 0;
}

/**
 * Wait for a new row of data from thread executing a SELECT statement.
 * @param s statement pointer
 * @result ODBC error code, error message in statement, if any
 */

static SQLRETURN
waitfordata(STMT *s)
{
    SQLRETURN ret = SQL_ERROR;
    DBC *d = (DBC *) s->dbc;

#ifdef HAVE_PTHREAD
    pthread_mutex_lock(&d->mut);
    while (!d->async_done && !d->async_ncols) {
	if (s->async_enable) {
	    pthread_mutex_unlock(&d->mut);
	    return SQL_STILL_EXECUTING;
	}
	pthread_cond_wait(&d->cond, &d->mut);
    }
#endif
#ifdef _WIN32
    if (!d->async_done) {
	if (s->async_enable) {
	    if (WaitForSingleObject(d->ev_res, 0) != WAIT_OBJECT_0) {
		return SQL_STILL_EXECUTING;
	    }
	} else {
	    WaitForSingleObject(d->ev_res, INFINITE);
	}
    }
#endif
    if (d->async_done) {
	ret = SQL_NO_DATA;
	if (d->async_errp) {
	    strncpy(s->logmsg, d->async_errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&d->async_errp);
	    ret = SQL_ERROR;
	}
    } else {
	freeresult(s, 0);
	s->rows = d->async_rows;
	s->ncols = d->async_ncols;
	d->async_ncols = 0;
	d->async_rows = NULL;
	if (s->rows) {
	    s->rowfree = freerows;
	    s->nrows = 1;
	    ret = SQL_SUCCESS;
	} else {
	    ret = nomem(s);
	}
    }
#ifdef HAVE_PTHREAD
    d->async_cont = 1;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mut);
#endif
#ifdef _WIN32
    SetEvent(d->ev_cont);
#endif
    return ret;
}

/**
 * @struct cbarg
 * Callback argument for SQLite callback function used in
 * thread for SELECT statements.
 */

struct cbarg {
    STMT *s;		/**< Executing statement. */
    int rowcount;	/**< Current # of row being processed. */
};

/**
 * SQLite callback function used by thread.
 * @param arg user data, actually a cbarg pointer
 * @param ncols number of columns
 * @param values value string array
 * @param cols column name string array
 * @result 1 to abort sqlite_exec(), 0 to continue
 */

static int
async_cb(void *arg, int ncols, char **values, char **cols)
{
    struct cbarg *ca = (struct cbarg *) arg;
    STMT *s = ca->s;
    DBC *d = (DBC *) s->dbc;
    char **rowd;
    int i;

    ++d->async_rownum;
    if (++ca->rowcount <= 1) {
	int size;
	char *p;
	COL *dyncols;

	if (ncols <= 0 || !cols) {
	    goto contsig;
	}
	for (i = size = 0; i < ncols; i++) {
	    size += 2 + 2 * strlen(cols[i]);
	}
	dyncols = xmalloc(ncols * sizeof (COL) + size);
	if (!dyncols) {
	    freedyncols(s);
	    s->ncols = 0;
	    return 1;
	}
	p = (char *) (dyncols + ncols);
	for (i = 0; i < ncols; i++) {
	    char *q;

	    dyncols[i].db = ((DBC *) (s->dbc))->dbname;
	    q = strchr(cols[i], '.');
	    if (q) {
		dyncols[i].table = p;
		strncpy(p, cols[i], q - cols[i]);
		p[q - cols[i]] = '\0';
		p += strlen(p) + 1;
		strcpy(p, q + 1);
		dyncols[i].column = p;
		p += strlen(p) + 1;
	    } else {
		dyncols[i].table = "";
		strcpy(p, cols[i]);
		dyncols[i].column = p;
		p += strlen(p) + 1;
	    }
#ifdef SQL_LONGVARCHAR
	    dyncols[i].type = SQL_LONGVARCHAR;
	    dyncols[i].size = 65536;
#else
	    dyncols[i].type = SQL_VARCHAR;
	    dyncols[i].size = 255;
#endif
	    dyncols[i].index = i;
	    dyncols[i].scale = 0;
	    dyncols[i].prec = 0;
	    dyncols[i].nosign = 1;
	    dyncols[i].typename = NULL;
	}
	freedyncols(s);
	s->ncols = s->dcols = ncols;
	s->dyncols = s->cols = dyncols;
	fixupdyncols(s, d->sqlite2);
	mkbindcols(s);
contsig:
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&d->mut);
	d->async_cont = 0;
	pthread_cond_signal(&d->cond);
	while (!d->async_run && !d->async_stop) {
	    pthread_cond_wait(&d->cond, &d->mut);
	}
	pthread_mutex_unlock(&d->mut);
#endif
#ifdef _WIN32
	SetEvent(d->ev_res);
	WaitForSingleObject(d->ev_cont, INFINITE);
#endif
	if (!cols) {
	    return 1;
	}
    }
    if (ncols <= 0) {
	return 1;
    }
    rowd = xmalloc((1 + 2 * ncols) * sizeof (char *));
    if (rowd) {
	rowd[0] = (char *) (ncols * 2);
	++rowd;
	for (i = 0; i < ncols; i++) {
	    rowd[i] = NULL;
	    rowd[i + ncols] = xstrdup(values ? values[i] : NULL);
	}
	for (i = 0; i < ncols; i++) {
	    if (values && values[i] && !rowd[i + ncols]) {
		freerows(rowd);
		rowd = NULL;
		break;
	    }
	}
    }
#ifdef HAVE_PTHREAD
    pthread_mutex_lock(&d->mut);
    d->async_rows = rowd;
    d->async_ncols = ncols;
    pthread_cond_signal(&d->cond);
    while (!d->async_cont && !d->async_stop) {
	pthread_cond_wait(&d->cond, &d->mut);
    }
    d->async_cont = 0;
    pthread_mutex_unlock(&d->mut);
#endif
#ifdef _WIN32
    d->async_rows = rowd;
    d->async_ncols = ncols;
    SetEvent(d->ev_res);
    WaitForSingleObject(d->ev_cont, INFINITE);
#endif
    return d->async_stop;
}

/**
 * @struct thrarg
 * Thread startup arguments for thread executing SQLite SELECT statement.
 */

struct thrarg {
    DBC *d;		/**< Connection handle. */
    STMT *s;		/**< Statement handle. */
    char **params;	/**< Parameters for SELECT statement. */
};

/**
 * Thread function to execute SELECT in parallel.
 * @param arg thread start argument, actually a thrarg pointer
 */

#ifdef HAVE_PTHREAD
static void *
async_exec(void *arg)
#endif
#ifdef _WIN32
static __stdcall
async_exec(void *arg)
#endif
{
    char *errp = NULL;
    int ret;
    struct thrarg ta = *((struct thrarg *) arg);
    DBC *d = ta.d;
    struct cbarg ca;

    ca.s = ta.s;
    ca.rowcount = 0;
    ret = sqlite_exec_vprintf(d->sqlite2, ta.s->query,
			      async_cb, &ca, &errp, (char *) ta.params);
    freep(&ta.params);
#ifdef HAVE_PTHREAD
    pthread_mutex_lock(&d->mut);
    d->async_errp = errp;
    d->async_done = 1;
    d->async_cont = 0;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mut);
#endif
#ifdef _WIN32
    d->async_errp = errp;
    d->async_done = 1;
    SetEvent(d->ev_res);
#endif
    return 0;
}

/**
 * Stop thread executing SELECT.
 * @param s statement pointer
 */

static void
async_end(STMT *s)
{
#ifdef HAVE_PTHREAD
    void *dummy;
#endif
    DBC *d;

    if (!s || !*s->async_run) {
	return;
    }
    d = (DBC *) s->dbc;
    sqlite_interrupt(d->sqlite2);
#ifdef HAVE_PTHREAD
    pthread_mutex_lock(&d->mut);
    d->async_cont = 1;
    d->async_stop = 1;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mut);
    pthread_join(d->thr, &dummy);
#endif
#ifdef _WIN32
    d->async_stop = 1;
    SetEvent(d->ev_cont);
    if (!d->async_done) {
	Sleep(50);
	SetEvent(d->ev_cont);
    }
    WaitForSingleObject(d->thr, INFINITE);
#endif
    d->async_run = 0;
    d->async_stmt = NULL;
    d->async_rownum = -1;
    if (d->async_rows) {
	freerows(d->async_rows);
	d->async_rows = NULL;
    }
    freep(&d->async_errp);
    d->async_ncols = 0;
}

/**
 * Conditionally stop executing thread.
 * @param s statement pointer
 */

static void
async_end_if(STMT *s)
{
    DBC *d = (DBC *) s->dbc;

    if (d && d->async_stmt == s) {
	async_end(s);
    }
}

/**
 * Start thread for execution of SELECT statement.
 * @param s statement pointer
 * @param params string array of statement parameters
 * @result ODBC error code
 */

static SQLRETURN
async_start(STMT *s, char **params)
{
    struct thrarg ta;
    DBC *d = (DBC *) s->dbc;
    SQLRETURN ret = SQL_SUCCESS;
#ifdef _WIN32
    DWORD dummy;
#endif

    ta.d = d;
    ta.s = s;
    ta.params = params;
    d->async_done = 0;
    d->async_run = 0;
    d->async_stmt = s;
    d->async_rownum = -1;
    d->async_ncols = 0;
#ifdef HAVE_PTHREAD
    d->async_stop = 0;
    d->async_cont = 1;
    pthread_mutex_lock(&d->mut);
    if (pthread_create(&d->thr, NULL, async_exec, &ta)) {
	ret = nomem(s);
    }
#endif
#ifdef _WIN32
    d->async_stop = 0;
    ResetEvent(d->ev_res);
    ResetEvent(d->ev_cont);
    d->thr = (HANDLE) _beginthreadex(NULL, 32768, async_exec, &ta, 0, &dummy);
    if (!d->thr) {
	ret = nomem(s);
    }
#endif
    if (ret == SQL_SUCCESS) {
#ifdef HAVE_PTHREAD
	while (d->async_cont && !d->async_done) {
	    pthread_cond_wait(&d->cond, &d->mut);
	}
	d->async_run = 1;
	pthread_cond_signal(&d->cond);
#endif
#ifdef _WIN32
	d->async_run = 1;
	WaitForSingleObject(d->ev_res, INFINITE);
	SetEvent(d->ev_cont);
#endif
    }
#ifdef HAVE_PTHREAD
    pthread_mutex_unlock(&d->mut);
#endif
    return ret;
}

#endif

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLBulkOperations(SQLHSTMT stmt, SQLSMALLINT oper)
{
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLDataSources(SQLHENV env, SQLUSMALLINT dir, SQLCHAR *srvname,
	       SQLSMALLINT buflen1, SQLSMALLINT *lenp1,
	       SQLCHAR *desc, SQLSMALLINT buflen2, SQLSMALLINT *lenp2)
{
    if (env == SQL_NULL_HENV) {
	return SQL_INVALID_HANDLE;
    }
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLDrivers(SQLHENV env, SQLUSMALLINT dir, SQLCHAR *drvdesc,
	   SQLSMALLINT descmax, SQLSMALLINT *desclenp,
	   SQLCHAR *drvattr, SQLSMALLINT attrmax, SQLSMALLINT *attrlenp)
{
    if (env == SQL_NULL_HENV) {
	return SQL_INVALID_HANDLE;
    }
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLBrowseConnect(SQLHDBC dbc, SQLCHAR *connin, SQLSMALLINT conninLen,
		 SQLCHAR *connout, SQLSMALLINT connoutMax,
		 SQLSMALLINT *connoutLen)
{
    return drvunimpldbc(dbc);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLPutData(SQLHSTMT stmt, SQLPOINTER data, SQLINTEGER len)
{
    return drvunimplstmt(stmt);
}

/**
 * Clear out parameter bindings, if any.
 * @param s statement pointer
 */

static SQLRETURN
freeparams(STMT *s)
{
    if (s->bindparms) {
	int n;

	for (n = 0; n < s->nbindparms; n++) {
	    memset(&s->bindparms[n], 0, sizeof (BINDPARM));
	}
    }
    return SQL_SUCCESS;
}

/* see doc on top */

static SQLRETURN
substparam(STMT *s, int pnum, char **out, int *size)
{
    char buf[256];
    int type;

    if (!s->bindparms || pnum < 0 || pnum >= s->nbindparms) {
	goto error;
    }
    if (!s->bindparms[pnum].param) {
	strcpy(buf, "NULL");
	goto bind;
    }
    type = s->bindparms[pnum].type;
    if (type == SQL_C_DEFAULT) {
	switch (s->bindparms[pnum].stype) {
	case SQL_INTEGER:
	    type = SQL_C_LONG;
	    break;
	case SQL_TINYINT:
	    type = SQL_C_TINYINT;
	    break;
	case SQL_SMALLINT:
	    type = SQL_C_SHORT;
	    break;
	case SQL_FLOAT:
	    type = SQL_C_FLOAT;
	    break;
	case SQL_DOUBLE:
	    type = SQL_C_DOUBLE;
	    break;
	case SQL_TIMESTAMP:
	    type = SQL_C_TIMESTAMP;
	    break;
	case SQL_TIME:
	    type = SQL_C_TIME;
	    break;
	case SQL_DATE:
	    type = SQL_C_DATE;
	    break;
	default:
	    type = SQL_C_CHAR;
	    break;
	}
    }
    switch (type) {
    case SQL_C_CHAR:
	break;
    case SQL_C_UTINYINT:
	sprintf(buf, "%d", *(unsigned char *) s->bindparms[pnum].param);
	break;
    case SQL_C_TINYINT:
    case SQL_C_STINYINT:
	sprintf(buf, "%d", *(char *) s->bindparms[pnum].param);
	break;
    case SQL_C_USHORT:
	sprintf(buf, "%d", *(unsigned short *) s->bindparms[pnum].param);
	break;
    case SQL_C_SHORT:
    case SQL_C_SSHORT:
	sprintf(buf, "%d", *(short *) s->bindparms[pnum].param);
	break;
    case SQL_C_ULONG:
    case SQL_C_LONG:
    case SQL_C_SLONG:
	sprintf(buf, "%ld", *(long *) s->bindparms[pnum].param);
	goto bind;
    case SQL_C_FLOAT:
	sprintf(buf, "%g", *(float *) s->bindparms[pnum].param);
	goto bind;
    case SQL_C_DOUBLE:
	sprintf(buf, "%g", *(double *) s->bindparms[pnum].param);
	goto bind;
    case SQL_C_DATE:
	sprintf(buf, "%04d-%02d-%02d",
		((DATE_STRUCT *) s->bindparms[pnum].param)->year,
		((DATE_STRUCT *) s->bindparms[pnum].param)->month,
		((DATE_STRUCT *) s->bindparms[pnum].param)->day);
	goto bind;
    case SQL_C_TIME:
	sprintf(buf, "%02d:%02d:%02d",
		((TIME_STRUCT *) s->bindparms[pnum].param)->hour,
		((TIME_STRUCT *) s->bindparms[pnum].param)->minute,
		((TIME_STRUCT *) s->bindparms[pnum].param)->second);
	goto bind;
    case SQL_C_TIMESTAMP:
	sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d.%ld",
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->year,
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->month,
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->day,
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->hour,
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->minute,
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->second,
		((TIMESTAMP_STRUCT *) s->bindparms[pnum].param)->fraction);
    bind:
	if (out) {
	    strcpy(*out, buf);
	    *out += strlen(buf) + 1;
	}
	if (size) {
	    *size += strlen(buf) + 1;
	}
	return SQL_SUCCESS;
    default:
	goto error;
    }
    if (out) {
	if (s->bindparms[pnum].max == SQL_NTS ||
	    s->bindparms[pnum].max == SQL_SETPARAM_VALUE_MAX) {
	    strcpy(*out, s->bindparms[pnum].param);
	    *out += strlen(s->bindparms[pnum].param) + 1;
	} else {
	    int len = *s->bindparms[pnum].lenp;

	    memcpy(*out, s->bindparms[pnum].param, len);
	    *out += len;
	    **out = '\0';
	    *out += 1;
	}
    }
    if (size) {
	if (s->bindparms[pnum].max == SQL_NTS ||
	    s->bindparms[pnum].max == SQL_SETPARAM_VALUE_MAX) {
	    *size += strlen(s->bindparms[pnum].param) + 1;
	} else {
	    *size += *s->bindparms[pnum].lenp + 1;
	}
    }
    return SQL_SUCCESS;
error:
    strcpy(s->logmsg, "invalid parameter");
    strcpy(s->sqlstate, "S1093");
    return SQL_ERROR;
}

/**
 * Internal bind parameter on HSTMT.
 * @param stmt statement handle
 * @param pnum parameter number, starting at 1
 * @param iotype input/output type of parameter
 * @param buftype type of host variable
 * @param ptype
 * @param coldef
 * @param scale
 * @param data pointer to host variable
 * @param buflen length of host variable
 * @param len output length pointer
 * @result ODBC error code
 */

static SQLRETURN
drvbindparam(SQLHSTMT stmt, SQLUSMALLINT pnum, SQLSMALLINT iotype,
	     SQLSMALLINT buftype, SQLSMALLINT ptype, SQLUINTEGER coldef,
	     SQLSMALLINT scale,
	     SQLPOINTER data, SQLINTEGER buflen, SQLINTEGER *len)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (pnum == 0) {
	strcpy(s->logmsg, "invalid parameter");
	strcpy(s->sqlstate, "S1093");
	return SQL_ERROR;
    }
#if 0
    if (iotype != SQL_PARAM_INPUT)
	return SQL_ERROR;
#endif
    --pnum;
    if (s->bindparms) {
	if (pnum >= s->nbindparms) {
	    BINDPARM *newparms;
	    
	    newparms = xrealloc(s->bindparms,
				(pnum + 1) * sizeof (BINDPARM));
	    if (!newparms) {
outofmem:
		return nomem(s);
	    }
	    s->bindparms = newparms;
	    memset(&s->bindparms[s->nbindparms], 0,
		   (pnum - s->nbindparms) * sizeof (BINDPARM));
	    s->nbindparms = pnum + 1;
	}
    } else {
	int npar = max(10, pnum + 1);

	s->bindparms = xmalloc(npar * sizeof (BINDPARM));
	if (!s->bindparms) {
	    goto outofmem;
	}
	memset(s->bindparms, 0, npar * sizeof (BINDPARM));
	s->nbindparms = npar;
    }
    s->bindparms[pnum].type = buftype; 
    s->bindparms[pnum].stype = ptype; 
    s->bindparms[pnum].max = buflen; 
    s->bindparms[pnum].lenp = (int *) len;
    s->bindparms[pnum].param = data;
    return SQL_SUCCESS;
}

/**
 * Bind parameter on HSTMT.
 * @param stmt statement handle
 * @param pnum parameter number, starting at 1
 * @param iotype input/output type of parameter
 * @param buftype type of host variable
 * @param ptype
 * @param coldef
 * @param scale
 * @param data pointer to host variable
 * @param buflen length of host variable
 * @param len output length pointer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLBindParameter(SQLHSTMT stmt, SQLUSMALLINT pnum, SQLSMALLINT iotype,
		 SQLSMALLINT buftype, SQLSMALLINT ptype, SQLUINTEGER coldef,
		 SQLSMALLINT scale,
		 SQLPOINTER data, SQLINTEGER buflen, SQLINTEGER *len)
{
    return drvbindparam(stmt, pnum, iotype, buftype, ptype, coldef,
			scale, data, buflen, len);
}

/**
 * Bind parameter on HSTMT.
 * @param stmt statement handle
 * @param pnum parameter number, starting at 1
 * @param vtype input/output type of parameter
 * @param ptype
 * @param lenprec
 * @param scale
 * @param val pointer to host variable
 * @param lenp output length pointer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLBindParam(SQLHSTMT stmt, SQLUSMALLINT pnum, SQLSMALLINT vtype,
	     SQLSMALLINT ptype, SQLUINTEGER lenprec,
	     SQLSMALLINT scale, SQLPOINTER val,
	     SQLINTEGER *lenp)
{
    return drvbindparam(stmt, pnum, SQL_PARAM_INPUT, vtype, ptype,
			lenprec, scale, val, 0, lenp);
}

/**
 * Return number of parameters.
 * @param stmt statement handle
 * @param nparam output parameter count
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLNumParams(SQLHSTMT stmt, SQLSMALLINT *nparam)
{
    STMT *s;
    SQLSMALLINT dummy;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!nparam) {
	nparam = &dummy;
    }
    *nparam = s->nparams;
    return SQL_SUCCESS;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLParamData(SQLHSTMT stmt, SQLPOINTER *p)
{
    return drvunimplstmt(stmt);
}

/**
 * Return information about parameter.
 * @param stmt statement handle
 * @param pnum parameter number, starting at 1
 * @param dtype output type indicator
 * @param size output size indicator
 * @param decdigits output number of digits
 * @param nullable output NULL allowed indicator
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLDescribeParam(SQLHSTMT stmt, UWORD pnum, SWORD *dtype, UDWORD *size,
		 SWORD *decdigits, SWORD *nullable)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    --pnum;
    if (pnum >= s->nparams) {
	strcpy(s->logmsg, "invalid parameter index");
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    if (dtype) {
#ifdef SQL_LONGVARCHAR
	*dtype = SQL_LONGVARCHAR;
#else
	*dtype = SQL_VARCHAR;
#endif
    }
    if (size) {
#ifdef SQL_LONGVARCHAR
	*size = 65536;
#else
	*size = 255;
#endif
    }
    if (decdigits) {
	*decdigits = 0;
    }
    if (nullable) {
	*nullable = SQL_NULLABLE;
    }
    return SQL_SUCCESS;
}

/**
 * Set information on parameter.
 * @param stmt statement handle
 * @param par parameter number, starting at 1
 * @param type type of host variable
 * @param sqltype
 * @param coldef
 * @param scale
 * @param val pointer to host variable
 * @param len output length pointer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSetParam(SQLHSTMT stmt, SQLUSMALLINT par, SQLSMALLINT type,
	    SQLSMALLINT sqltype, SQLUINTEGER coldef,
	    SQLSMALLINT scale, SQLPOINTER val, SQLINTEGER *nval)
{
    return drvbindparam(stmt, par, SQL_PARAM_INPUT,
			type, sqltype, coldef, scale, val,
			SQL_SETPARAM_VALUE_MAX, nval);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLParamOptions(SQLHSTMT stmt, UDWORD rows, UDWORD *rowp)
{
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLGetDescField(SQLHDESC handle, SQLSMALLINT recno,
		SQLSMALLINT fieldid, SQLPOINTER value,
		SQLINTEGER buflen, SQLINTEGER *strlen)
{
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLSetDescField(SQLHDESC handle, SQLSMALLINT recno,
		SQLSMALLINT fieldid, SQLPOINTER value,
		SQLINTEGER buflen)
{
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLGetDescRec(SQLHDESC handle, SQLSMALLINT recno,
	      SQLCHAR *name, SQLSMALLINT buflen,
	      SQLSMALLINT *strlen, SQLSMALLINT *type,
	      SQLSMALLINT *subtype, SQLINTEGER *len,
	      SQLSMALLINT *prec, SQLSMALLINT *scale,
	      SQLSMALLINT *nullable)
{
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLSetDescRec(SQLHDESC handle, SQLSMALLINT recno,
	      SQLSMALLINT type, SQLSMALLINT subtype,
	      SQLINTEGER len, SQLSMALLINT prec,
	      SQLSMALLINT scale, SQLPOINTER data,
	      SQLINTEGER *strlen, SQLINTEGER *indicator)
{
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLTablePrivileges(SQLHSTMT stmt,
		   SQLCHAR *catalog, SQLSMALLINT catalogLen,
		   SQLCHAR *schema, SQLSMALLINT schemaLen,
		   SQLCHAR *table, SQLSMALLINT tableLen)
{
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLColumnPrivileges(SQLHSTMT stmt,
		    SQLCHAR *catalog, SQLSMALLINT catalogLen,
		    SQLCHAR *schema, SQLSMALLINT schemaLen,
		    SQLCHAR *table, SQLSMALLINT tableLen,
		    SQLCHAR *column, SQLSMALLINT columnLen)
{
    return drvunimplstmt(stmt);
}

/**
 * Columns for result set of SQLPrimaryKeys().
 */

static COL pkeySpec[] = {
    { "SYSTEM", "PRIMARYKEY", "TABLE_QUALIFIER", SQL_VARCHAR, 50 },
    { "SYSTEM", "PRIMARYKEY", "TABLE_OWNER", SQL_VARCHAR, 50 },
    { "SYSTEM", "PRIMARYKEY", "TABLE_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "PRIMARYKEY", "COLUMN_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "PRIMARYKEY", "KEY_SEQ", SQL_SMALLINT, 50 },
    { "SYSTEM", "PRIMARYKEY", "PK_NAME", SQL_VARCHAR, 50 }
};

/**
 * Retrieve information about indexed columns.
 * @param stmt statement handle
 * @param cat catalog name/pattern or NULL
 * @param catLen length of catalog name/pattern or SQL_NTS
 * @param schema schema name/pattern or NULL
 * @param schemaLen length of schema name/pattern or SQL_NTS
 * @param table table name/pattern or NULL
 * @param tableLen length of table name/pattern or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLPrimaryKeys(SQLHSTMT stmt,
	   SQLCHAR *cat, SQLSMALLINT catLen,
	   SQLCHAR *schema, SQLSMALLINT schemaLen,
	   SQLCHAR *table, SQLSMALLINT tableLen)
{
    STMT *s;
    DBC *d;
    int i, size, ret, nrows, ncols, offs;
    int namec = -1, uniquec = -1;
    char **rowp, *errp, tname[512];

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
#ifdef ASYNC
    async_end_if(s);
#endif
    if (!table || table[0] == '\0' || table[0] == '%') {
	strcpy(s->logmsg, "need table name");
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    if (tableLen == SQL_NTS) {
	size = sizeof (tname) - 1;
    } else {
	size = min(sizeof (tname) - 1, tableLen);
    }
    strncpy(tname, table, size);
    tname[size] = '\0';
    freeresult(s, 1);
    s->ncols = array_size(pkeySpec);
    s->cols = pkeySpec;
    s->nrows = 0;
    mkbindcols(s);
    s->rowp = -1;
    ret = sqlite_get_table_printf(d->sqlite,
				  "PRAGMA index_list('%q')", &rowp,
				  &nrows, &ncols, &errp, tname);
    if (ret != SQLITE_OK) {
	if (errp) {
	    strncpy(s->logmsg, errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    sprintf(s->logmsg, "unknown error %d", ret);
	}
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    freep(&errp);
    if (ncols * nrows <= 0) {
nodata:
	sqlite_free_table(rowp);
	return SQL_SUCCESS;
    }
    size = 0;
    for (i = 0; i < ncols; i++) {
	if (strcmp(rowp[i], "name") == 0) {
	    namec = i;
	} else if (strcmp(rowp[i], "unique") == 0) {
	    uniquec = i;
	}
    }
    if (namec < 0 || uniquec < 0) {
	goto nodata;
    }
    for (i = 1; i <= nrows; i++) {
	int nnrows, nncols;
	char **rowpp;

	if (*rowp[i * ncols + uniquec] != '0' &&
	    sqlite_get_table_printf(d->sqlite,
				    "PRAGMA index_info('%q')", &rowpp,
				    &nnrows, &nncols, NULL,
				    rowp[i * ncols + namec]) == SQLITE_OK) {
	    size += nnrows;
	    sqlite_free_table(rowpp);
	}
    }
    if (size == 0) {
	goto nodata;
    }
    s->nrows = size;
    size = (size + 1) * array_size(pkeySpec);
    s->rows = xmalloc((size + 1) * sizeof (char *));
    if (!s->rows) {
	s->nrows = 0;
	return nomem(s);
    }
    s->rows[0] = (char *) size;
    s->rows += 1;
    memset(s->rows, 0, sizeof (char *) * size);
    s->rowfree = freerows;
    offs = 0;
    for (i = 1; i <= nrows; i++) {
	int nnrows, nncols;
	char **rowpp;

	if (*rowp[i * ncols + uniquec] != '0' &&
	    sqlite_get_table_printf(d->sqlite,
				    "PRAGMA index_info('%q')", &rowpp,
				    &nnrows, &nncols, NULL,
				    rowp[i * ncols + namec]) == SQLITE_OK) {
	    int k;

	    for (k = 0; nnrows && k < nncols; k++) {
		if (strcmp(rowpp[k], "name") == 0) {
		    int m;

		    for (m = 1; m <= nnrows; m++) {
			int roffs = (offs + m) * s->ncols;

			s->rows[roffs + 0] = xstrdup("");
			s->rows[roffs + 1] = xstrdup("");
			s->rows[roffs + 2] = xstrdup(tname);
			s->rows[roffs + 3] = xstrdup(rowpp[m * nncols + k]);
			s->rows[roffs + 5] = xstrdup(rowp[i * ncols + namec]);
		    }
		} else if (strcmp(rowpp[k], "seqno") == 0) {
		    int m;

		    for (m = 1; m <= nnrows; m++) {
			int roffs = (offs + m) * s->ncols;
			int pos = m - 1;
			char buf[32];

			sscanf(rowpp[m * nncols + k], "%d", &pos);
			sprintf(buf, "%d", pos + 1);
			s->rows[roffs + 4] = xstrdup(buf);
		    }
		}
	    }
	    offs += nnrows;
	    sqlite_free_table(rowpp);
	}
    }
    sqlite_free_table(rowp);
    return SQL_SUCCESS;
}

/**
 * Columns for result set of SQLSpecialColumns().
 */

static COL scolSpec[] = {
    { "SYSTEM", "COLUMN", "SCOPE", SQL_SMALLINT, 1 },
    { "SYSTEM", "COLUMN", "COLUMN_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "COLUMN", "DATA_TYPE", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "TYPE_NAME", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "PRECISION", SQL_INTEGER, 50 },
    { "SYSTEM", "COLUMN", "LENGTH", SQL_INTEGER, 50 },
    { "SYSTEM", "COLUMN", "DECIMAL_DIGITS", SQL_INTEGER, 50 },
    { "SYSTEM", "COLUMN", "PSEUDO_COLUMN", SQL_SMALLINT, 1 },
    { "SYSTEM", "COLUMN", "NULLABLE", SQL_SMALLINT, 1 }
};

/**
 * Retrieve information about indexed columns.
 * @param stmt statement handle
 * @param id type of information, e.g. best row id
 * @param cat catalog name/pattern or NULL
 * @param catLen length of catalog name/pattern or SQL_NTS
 * @param schema schema name/pattern or NULL
 * @param schemaLen length of schema name/pattern or SQL_NTS
 * @param table table name/pattern or NULL
 * @param tableLen length of table name/pattern or SQL_NTS
 * @param scope
 * @param nullable
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSpecialColumns(SQLHSTMT stmt, SQLUSMALLINT id,
		  SQLCHAR *cat, SQLSMALLINT catLen,
		  SQLCHAR *schema, SQLSMALLINT schemaLen,
		  SQLCHAR *table, SQLSMALLINT tableLen,
		  SQLUSMALLINT scope, SQLUSMALLINT nullable)
{
    STMT *s;
    DBC *d;
    int i, size, ret, nrows, ncols, nnnrows, nnncols, offs;
    int namec = -1, uniquec = -1;
    int namecc = -1, typecc = -1, notnullcc = -1;
    char *errp, tname[512];
    char **rowp = NULL, **rowppp = NULL;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
#ifdef ASYNC
    async_end_if(s);
#endif
    if (!table || table[0] == '\0' || table[0] == '%') {
	strcpy(s->logmsg, "need table name");
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    if (tableLen == SQL_NTS) {
	size = sizeof (tname) - 1;
    } else {
	size = min(sizeof (tname) - 1, tableLen);
    }
    strncpy(tname, table, size);
    tname[size] = '\0';
    freeresult(s, 1);
    s->ncols = array_size(scolSpec);
    s->cols = scolSpec;
    mkbindcols(s);
    s->nrows = 0;
    s->rowp = -1;
    if (id != SQL_BEST_ROWID) {
	goto nodata;
    }
    ret = sqlite_get_table_printf(d->sqlite, "PRAGMA index_list('%q')",
				  &rowp, &nrows, &ncols, &errp, tname);
    if (ret != SQLITE_OK) {
	if (errp) {
	    strncpy(s->logmsg, errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    sprintf(s->logmsg, "unknown error %d", ret);
	}
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;	
    }
    freep(&errp);
    if (ncols * nrows <= 0) {
nodata:
	sqlite_free_table(rowp);
	sqlite_free_table(rowppp);
	return SQL_SUCCESS;
    }
    ret = sqlite_get_table_printf(d->sqlite, "PRAGMA table_info('%q')",
				  &rowppp, &nnnrows, &nnncols, &errp, tname);
    if (ret != SQLITE_OK) {
	sqlite_free_table(rowp);
	if (errp) {
	    strncpy(s->logmsg, errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    sprintf(s->logmsg, "unknown error %d", ret);
	}
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;	
    }
    freep(&errp);
    size = 0;
    for (i = 0; i < ncols; i++) {
	if (strcmp(rowp[i], "name") == 0) {
	    namec = i;
	} else if (strcmp(rowp[i], "unique") == 0) {
	    uniquec = i;
	}
    }
    if (namec < 0 || uniquec < 0) {
	goto nodata;
    }
    for (i = 0; i < nnncols; i++) {
	if (strcmp(rowppp[i], "name") == 0) {
	    namecc = i;
	} else if (strcmp(rowppp[i], "type") == 0) {
	    typecc = i;
	} else if (strcmp(rowppp[i], "notnull") == 0) {
	    notnullcc = i;
	}
    }
    for (i = 1; i <= nrows; i++) {
	int nnrows, nncols;
	char **rowpp;

	if (*rowp[i * ncols + uniquec] != '0' &&
	    sqlite_get_table_printf(d->sqlite,
				    "PRAGMA index_info('%q')", &rowpp,
				    &nnrows, &nncols, NULL,
				    rowp[i * ncols + namec]) == SQLITE_OK) {
	    size += nnrows;
	    sqlite_free_table(rowpp);
	}
    }
    if (size == 0) {
	goto nodata;
    }
    s->nrows = size;
    size = (size + 1) * array_size(scolSpec);
    s->rows = xmalloc((size + 1) * sizeof (char *));
    if (!s->rows) {
	s->nrows = 0;
	return nomem(s);
    }
    s->rows[0] = (char *) size;
    s->rows += 1;
    memset(s->rows, 0, sizeof (char *) * size);
    s->rowfree = freerows;
    offs = 0;
    for (i = 1; i <= nrows; i++) {
	int nnrows, nncols;
	char **rowpp;

	if (*rowp[i * ncols + uniquec] != '0' &&
	    sqlite_get_table_printf(d->sqlite,
				    "PRAGMA index_info('%q')", &rowpp,
				    &nnrows, &nncols, NULL,
				    rowp[i * ncols + namec]) == SQLITE_OK) {
	    int k;

	    for (k = 0; nnrows && k < nncols; k++) {
		if (strcmp(rowpp[k], "name") == 0) {
		    int m;

		    for (m = 1; m <= nnrows; m++) {
			int roffs = (offs + m) * s->ncols;

			s->rows[roffs + 0] =
			    xstrdup(stringify(SQL_SCOPE_CURROW));
			s->rows[roffs + 1] = xstrdup(rowpp[m * nncols + k]);
			s->rows[roffs + 4] = xstrdup("0");
			s->rows[roffs + 7] = xstrdup(stringify(SQL_PC_UNKNOWN));
			if (namecc >= 0 && typecc >= 0) {
			    int ii;

			    for (ii = 1; ii <= nnnrows; ii++) {
				if (strcmp(rowppp[ii * nnncols + namecc],
					   rowpp[m * nncols + k]) == 0) {
				    char *typen = rowppp[ii * nnncols + typecc];
				    int sqltype, mm, dd, isnullable = 0;
				    char buf[32];
					
				    s->rows[roffs + 3] = xstrdup(typen);
				    sqltype = mapsqltype(typen, NULL);
				    getmd(typen, sqltype, &mm, &dd);
#ifdef SQL_LONGVARCHAR
				    if (sqltype == SQL_VARCHAR && mm > 255) {
					sqltype = SQL_LONGVARCHAR;
				    }
#endif
				    sprintf(buf, "%d", sqltype);
				    s->rows[roffs + 2] = xstrdup(buf);
				    sprintf(buf, "%d", mm);
				    s->rows[roffs + 5] = xstrdup(buf);
				    sprintf(buf, "%d", dd);
				    s->rows[roffs + 6] = xstrdup(buf);
				    if (notnullcc >= 0) {
					char *inp =
					   rowppp[ii * nnncols + notnullcc];

					isnullable = inp[0] != '0';
				    }
				    sprintf(buf, "%d", isnullable);
				    s->rows[roffs + 8] = xstrdup(buf);
				}
			    }
			}
		    }
		}
	    }
	    offs += nnrows;
	    sqlite_free_table(rowpp);
	}
    }
    sqlite_free_table(rowp);
    sqlite_free_table(rowppp);
    if (nullable == SQL_NO_NULLS) {
	for (i = 1; i < s->nrows; i++) {
	    if (s->rows[i * s->ncols + 8][0] == '0') {
		int m, i1 = i + 1;

		for (m = 0; m < s->ncols; m++) {
		    freep(&s->rows[i * s->ncols + m]);
		}
		size = s->ncols * sizeof (char *) * (s->nrows - i1);
		if (size > 0) {
		    memmove(s->rows + i * s->ncols,
			    s->rows + i1 * s->ncols,
			    size);
		    memset(s->rows + s->nrows * s->ncols, 0,
			   s->ncols * sizeof (char *));
		}
		s->nrows--;
		--i;
	    }
	}
    }
    return SQL_SUCCESS;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLForeignKeys(SQLHSTMT stmt,
	       SQLCHAR *PKcatalog, SQLSMALLINT PKcatalogLen,
	       SQLCHAR *PKschema, SQLSMALLINT PKschemaLen,
	       SQLCHAR *PKtable, SQLSMALLINT PKtableLen,
	       SQLCHAR *FKcatalog, SQLSMALLINT FKcatalogLen,
	       SQLCHAR *FKschema, SQLSMALLINT FKschemaLen,
	       SQLCHAR *FKtable, SQLSMALLINT FKtableLen)
{
    return drvunimplstmt(stmt);
}

/**
 * Internal commit or rollback transaction.
 * @param d database connection pointer
 * @param comptype type of transaction's end, SQL_COMMIT or SQL_ROLLBACK
 * @result ODBC error code
 */

static SQLRETURN
endtran(DBC *d, SQLSMALLINT comptype)
{
    int fail = 0;
    char *sql, *errp;

    if (!d->sqlite) {
	strcpy(d->logmsg, "not connected");
	strcpy(d->sqlstate, "S1000");
	return SQL_ERROR;
    }
    if (d->autocommit || !d->intrans) {
	return SQL_SUCCESS;
    }
    switch (comptype) {
    case SQL_COMMIT:
	sql = "COMMIT TRANSACTION";
	goto doit;
    case SQL_ROLLBACK:
    rollback:
	sql = "ROLLBACK TRANSACTION";
    doit:
	d->intrans = 0;
	if (sqlite_exec(d->sqlite, sql, NULL, NULL, &errp) != SQLITE_OK) {
	    if (!fail) {
		if (errp) {
		    strncpy(d->logmsg, errp, sizeof (d->logmsg));
		    d->logmsg[sizeof (d->logmsg) - 1] = '\0';
		    freep(&errp);
		} else {
		    strcpy(d->logmsg, "transaction failed");
		}
		strcpy(d->sqlstate, "S1000");
		fail = 1;
		goto rollback;
	    }
	    freep(&errp);
	    return SQL_ERROR;
	}
	freep(&errp);
	return SQL_SUCCESS;
    }
    strcpy(d->logmsg, "invalid completion type");
    strcpy(d->sqlstate, "S1000");
    return SQL_ERROR;
}

/**
 * Internal commit or rollback transaction.
 * @param type type of handle
 * @param handle HDBC, HENV, or HSTMT handle
 * @param comptype SQL_COMMIT or SQL_ROLLBACK
 * @result ODBC error code
 */

static SQLRETURN
drvendtran(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT comptype)
{
    DBC *d;
    int fail = 0;

    switch (type) {
    case SQL_HANDLE_DBC:
	if (handle == SQL_NULL_HDBC) {
	    return SQL_INVALID_HANDLE;
	}
	d = (DBC *) handle;
	return endtran(d, comptype);
    case SQL_HANDLE_ENV:
	if (handle == SQL_NULL_HENV) {
	    return SQL_INVALID_HANDLE;
	}
	d = ((ENV *) handle)->dbcs;
	while (d) {
	    SQLRETURN ret;

	    ret = endtran(d, comptype);
	    if (ret != SQL_SUCCESS) {
		fail++;
		comptype = SQL_ROLLBACK;
	    }
	    d = d->next;
	}
	return fail ? SQL_ERROR : SQL_SUCCESS;
    }
    return SQL_INVALID_HANDLE;
}

/**
 * Commit or rollback transaction.
 * @param type type of handle
 * @param handle HDBC, HENV, or HSTMT handle
 * @param comptype SQL_COMMIT or SQL_ROLLBACK
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLEndTran(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT comptype)
{
    return drvendtran(type, handle, comptype);
}

/**
 * Commit or rollback transaction.
 * @param env environment handle or NULL
 * @param dbc database connection handle or NULL
 * @param type SQL_COMMIT or SQL_ROLLBACK
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLTransact(SQLHENV env, SQLHDBC dbc, UWORD type)
{
    if (env != SQL_NULL_HENV) {
	return drvendtran(SQL_HANDLE_ENV, (SQLHANDLE) env, type);
    }
    return drvendtran(SQL_HANDLE_DBC, (SQLHANDLE) dbc, type);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLCopyDesc(SQLHDESC source, SQLHDESC target)
{
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLNativeSql(SQLHSTMT stmt, SQLCHAR *sqlin, SQLINTEGER sqlinLen,
	     SQLCHAR *sql, SQLINTEGER sqlMax, SQLINTEGER *sqlLen)
{
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLProcedures(SQLHSTMT stmt,
	      SQLCHAR *catalog, SQLSMALLINT catalogLen,
	      SQLCHAR *schema, SQLSMALLINT schemaLen,
	      SQLCHAR *proc, SQLSMALLINT procLen)
{
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLProcedureColumns(SQLHSTMT stmt,
		    SQLCHAR *catalog, SQLSMALLINT catalogLen,
		    SQLCHAR *schema, SQLSMALLINT schemaLen,
		    SQLCHAR *proc, SQLSMALLINT procLen,
		    SQLCHAR *column, SQLSMALLINT columnLen)
{
    return drvunimplstmt(stmt);
}

/**
 * Get information of HENV.
 * @param env environment handle
 * @param attr attribute to be retrieved
 * @param val output buffer
 * @param len length of output buffer
 * @param lenp output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetEnvAttr(SQLHENV env, SQLINTEGER attr, SQLPOINTER val,
	      SQLINTEGER len, SQLINTEGER *lenp)
{
    if (env == SQL_NULL_HENV) {
	return SQL_INVALID_HANDLE;
    }
    switch (attr) {
    case SQL_ATTR_CONNECTION_POOLING:
	return SQL_ERROR;
    case SQL_ATTR_CP_MATCH:
    case SQL_ATTR_OUTPUT_NTS:
	return SQL_NO_DATA;
    case SQL_ATTR_ODBC_VERSION:
	if (val) {
	    *(SQLINTEGER *) val = SQL_OV_ODBC2;
	}
	if (lenp) {
	    *lenp = sizeof (SQLINTEGER);
	}
	return SQL_SUCCESS;
    }
    return SQL_ERROR;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLSetEnvAttr(SQLHENV env, SQLINTEGER attr, SQLPOINTER val, SQLINTEGER len)
{
    return SQL_ERROR;
}

/**
 * Get error message given handle (HENV, HDBC, or HSTMT).
 * @param htype handle type
 * @param handle HENV, HDBC, or HSTMT
 * @param recno
 * @param sqlstate output buffer for SQL state
 * @param nativeerr output buffer of native error code
 * @param msg output buffer for error message
 * @param buflen length of output buffer
 * @param msglen output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetDiagRec(SQLSMALLINT htype, SQLHANDLE handle, SQLSMALLINT recno,
	      SQLCHAR *sqlstate, SQLINTEGER *nativeerr, SQLCHAR *msg,
	      SQLSMALLINT buflen, SQLSMALLINT *msglen)
{
    DBC *d = NULL;
    STMT *s = NULL;
    int len;
    char *logmsg, *sqlst;

    if (handle == SQL_NULL_HANDLE) {
	return SQL_INVALID_HANDLE;
    }
    switch (htype) {
    case SQL_HANDLE_ENV:
    case SQL_HANDLE_DESC:
	return SQL_NO_DATA;
    case SQL_HANDLE_DBC:
	d = (DBC *) handle;
	logmsg = d->logmsg;
	sqlst = d->sqlstate;
	break;
    case SQL_HANDLE_STMT:
	s = (STMT *) handle;
	logmsg = s->logmsg;
	sqlst = s->sqlstate;
	break;
    default:
	return SQL_INVALID_HANDLE;
    }
    if (msg == NULL || buflen <= 0) {
	return SQL_ERROR;
    }
    if (recno > 1) {
	logmsg[0] = '\0';
	return SQL_NO_DATA;
    }
    len = strlen(logmsg);
    if (len == 0) {
	return SQL_NO_DATA;
    }
    if (nativeerr) {
	*nativeerr = 0;
    }
    if (sqlstate) {
	strcpy(sqlstate, sqlst);
    }
    if (msglen) {
	*msglen = len;
    }
    if (len > buflen) {
	strncpy(msg, logmsg, buflen);
	msg[buflen - 1] = '\0';
	return SQL_SUCCESS_WITH_INFO;
    }
    strcpy(msg, logmsg);
    return SQL_SUCCESS;
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLGetDiagField(SQLSMALLINT htype, SQLHANDLE handle, SQLSMALLINT recno,
		SQLSMALLINT id, SQLPOINTER info, 
		SQLSMALLINT buflen, SQLSMALLINT *strlen)
{
    return SQL_ERROR;
}

/**
 * Get option of HSTMT.
 * @param stmt statement handle
 * @param attr attribute to be retrieved
 * @param val output buffer
 * @param bufmax length of output buffer
 * @param buflen output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetStmtAttr(SQLHSTMT stmt, SQLINTEGER attr, SQLPOINTER val,
	       SQLINTEGER bufmax, SQLINTEGER *buflen)
{
    STMT *s = (STMT *) stmt;
    SQLUINTEGER *uval = (SQLUINTEGER *) val;

    switch (attr) {
    case SQL_ATTR_CURSOR_TYPE:
#ifdef ASYNC
	*uval = s->curtype;
#else
	*uval = SQL_CURSOR_STATIC;
#endif
	return SQL_SUCCESS;
    case SQL_ATTR_CURSOR_SCROLLABLE:
#ifdef ASYNC
	*uval = s->curtype != SQL_CURSOR_FORWARD_ONLY ?
	    SQL_SCROLLABLE : SQL_NONSCROLLABLE;
#else
	*uval = SQL_SCROLLABLE;
#endif
	return SQL_SUCCESS;
    case SQL_ATTR_ROW_NUMBER:
#ifdef ASYNC
	{
	    STMT *s = (STMT *) stmt;
	    DBC *d = (DBC *) s->dbc;

	    if (s == d->async_stmt) {
		*uval = d->async_rownum < 0 ?
		    SQL_ROW_NUMBER_UNKNOWN : d->async_rownum;
	    }
	}
#endif
	*uval = s->rowp < 0 ? SQL_ROW_NUMBER_UNKNOWN : s->rowp;
	return SQL_SUCCESS;
#ifdef ASYNC
    case SQL_ATTR_ASYNC_ENABLE: {
	STMT *s = (STMT *) stmt;
	*uval = s->async_enable ? SQL_TRUE : SQL_FALSE;
	return SQL_SUCCESS;
    }
#endif
    case SQL_CONCURRENCY:
	*uval = SQL_CONCUR_ROWVER;
	return SQL_SUCCESS;
    case SQL_ATTR_RETRIEVE_DATA:
	*uval = SQL_RD_ON;
	return SQL_SUCCESS;
    case SQL_ROWSET_SIZE:
    case SQL_ATTR_ROW_ARRAY_SIZE:
	*uval = 1;
	return SQL_SUCCESS;
    }
    return drvunimplstmt(stmt);
}

/**
 * Set option on HSTMT.
 * @param stmt statement handle
 * @param attr attribute to be set
 * @param val input buffer (attribute value)
 * @param buflen length of input buffer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSetStmtAttr(SQLHSTMT stmt, SQLINTEGER attr, SQLPOINTER val,
	       SQLINTEGER buflen)
{
    STMT *s = (STMT *) stmt;

    switch (attr) {
    case SQL_ATTR_CURSOR_TYPE:
#ifdef ASYNC
	if ((SQLUINTEGER) val == SQL_CURSOR_FORWARD_ONLY) {
	    s->curtype = SQL_CURSOR_FORWARD_ONLY;
	} else {
	    s->curtype = SQL_CURSOR_STATIC;
	}
#endif
	return SQL_SUCCESS;
    case SQL_ATTR_CURSOR_SCROLLABLE:
#ifdef ASYNC
	if ((SQLUINTEGER) val == SQL_NONSCROLLABLE) {
	    s->curtype = SQL_CURSOR_FORWARD_ONLY;
	} else {
	    s->curtype = SQL_CURSOR_STATIC;
	}
#endif
	return SQL_SUCCESS;
#ifdef ASYNC
    case SQL_ATTR_ASYNC_ENABLE:
	s->async_enable = (SQLUINTEGER) val;
	return SQL_SUCCESS;
#endif
    case SQL_CONCURRENCY:
    case SQL_ATTR_QUERY_TIMEOUT:
	return SQL_SUCCESS;
    case SQL_ATTR_RETRIEVE_DATA:
	if ((SQLUINTEGER) val != SQL_RD_ON) {
	    goto e01s02;
	}
	return SQL_SUCCESS;
    case SQL_ROWSET_SIZE:
    case SQL_ATTR_ROW_ARRAY_SIZE:
	if ((SQLUINTEGER) val != 1) {
    e01s02:
	    strcpy(s->logmsg, "option value changed");
	    strcpy(s->sqlstate, "01S02");
	    return SQL_SUCCESS_WITH_INFO;
	}
	return SQL_SUCCESS;
    }
    return drvunimplstmt(stmt);
}

/**
 * Get option of HSTMT.
 * @param stmt statement handle
 * @param opt option to be retrieved
 * @param param output buffer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetStmtOption(SQLHSTMT stmt, SQLUSMALLINT opt, SQLPOINTER param)
{
    STMT *s = (STMT *) stmt;
    SQLUINTEGER *ret = (SQLUINTEGER *) param;

    switch (opt) {
    case SQL_CURSOR_TYPE:
#ifdef ASYNC
	*ret = s->curtype;
#else
	*ret = SQL_CURSOR_STATIC;
#endif
	return SQL_SUCCESS;
    case SQL_ROW_NUMBER:
#ifdef ASYNC
	{
	    DBC *d = (DBC *) s->dbc;

	    if (s == d->async_stmt) {
		*ret = d->async_rownum < 0 ?
		    SQL_ROW_NUMBER_UNKNOWN : d->async_rownum;
	    }
	}
#endif
	*ret = s->rowp < 0 ? SQL_ROW_NUMBER_UNKNOWN : s->rowp;
	return SQL_SUCCESS;
#ifdef ASYNC
    case SQL_ASYNC_ENABLE:
	*ret = s->async_enable ? SQL_TRUE : SQL_FALSE;
	return SQL_SUCCESS;
#endif
    case SQL_CONCURRENCY:
	*ret = SQL_CONCUR_ROWVER;
	return SQL_SUCCESS;
    case SQL_ATTR_RETRIEVE_DATA:
	*ret = SQL_RD_ON;
	return SQL_SUCCESS;
    case SQL_ROWSET_SIZE:
    case SQL_ATTR_ROW_ARRAY_SIZE:
	*ret = 1;
	return SQL_SUCCESS;
    }
    return drvunimplstmt(stmt);
}

/**
 * Set option on HSTMT.
 * @param stmt statement handle
 * @param opt option to be set
 * @param param input buffer (option value)
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSetStmtOption(SQLHSTMT stmt, SQLUSMALLINT opt, SQLUINTEGER param)
{
    STMT *s = (STMT *) stmt;

    switch (opt) {
    case SQL_CURSOR_TYPE:
#ifdef ASYNC
	if (param == SQL_CURSOR_FORWARD_ONLY) {
	    s->curtype = param;
	} else {
	    s->curtype = SQL_CURSOR_STATIC;
	}
#endif
	return SQL_SUCCESS;
#ifdef ASYNC
    case SQL_ASYNC_ENABLE:
	s->async_enable = param;
	return SQL_SUCCESS;
#endif
    case SQL_CONCURRENCY:
    case SQL_QUERY_TIMEOUT:
	return SQL_SUCCESS;
    case SQL_RETRIEVE_DATA:
	if (param != SQL_RD_ON) {
	    goto e01s02;
	}
	return SQL_SUCCESS;
    case SQL_ROWSET_SIZE:
    case SQL_ATTR_ROW_ARRAY_SIZE:
	if (param != 1) {
    e01s02:
	    strcpy(s->logmsg, "option value changed");
	    strcpy(s->sqlstate, "01S02");
	    return SQL_SUCCESS_WITH_INFO;
	}
	return SQL_SUCCESS;
    }
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLSetPos(SQLHSTMT stmt, SQLUSMALLINT row, SQLUSMALLINT op, SQLUSMALLINT lock)
{
    return drvunimplstmt(stmt);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLSetScrollOptions(SQLHSTMT stmt, SQLUSMALLINT concur, SQLINTEGER rowkeyset,
		    SQLUSMALLINT rowset)
{
    return drvunimplstmt(stmt);
}

#define strmak(dst, src, max, lenp) { \
    int len = strlen(src); \
    int cnt = min(len + 1, max); \
    strncpy(dst, src, cnt); \
    *lenp = (cnt > len) ? len : cnt; \
}

/**
 * Return information about what this ODBC driver supports.
 * @param dbc database connection handle
 * @param type type of information to be retrieved
 * @param val output buffer
 * @param valMax length of output buffer
 * @param valLen output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetInfo(SQLHDBC dbc, SQLUSMALLINT type, SQLPOINTER val, SQLSMALLINT valMax,
	   SQLSMALLINT *valLen)
{
    DBC *d;
    char dummyc[16];
    SQLSMALLINT dummy;
    static char drvname[] =
#ifdef _WIN32
	"sqliteodbc.dll";
#else
	"sqliteodbc.so";
#endif

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (valMax) {
	valMax--;
    }
    if (!valLen) {
	valLen = &dummy;
    }
    if (!val) {
	val = dummyc;
	valMax = sizeof (dummyc) - 1;
    }
    switch (type) {
    case SQL_MAX_USER_NAME_LEN:
	*(SQLSMALLINT *) val = 16;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_DRIVER_ODBC_VER:
	strmak(val, "02.50", valMax, valLen);
	break;
    case SQL_ACTIVE_CONNECTIONS:
    case SQL_ACTIVE_STATEMENTS:
	*(SQLSMALLINT *) val = 0;
	*valLen = sizeof (SQLSMALLINT);
	break;
#ifdef SQL_ASYNC_MODE
    case SQL_ASYNC_MODE:
#ifdef ASYNC
	*(SQLUINTEGER *) val = SQL_AM_STATEMENT;
#else
	*(SQLUINTEGER *) val = SQL_AM_NONE;
#endif
	*valLen = sizeof (SQLUINTEGER);
	break;
#endif
#ifdef SQL_CREATE_TABLE
    case SQL_CREATE_TABLE:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
#endif
    case SQL_DATA_SOURCE_NAME:
	strmak(val, (d->dsn ? d->dsn : ""), valMax, valLen);
	break;
    case SQL_DRIVER_NAME:
	strmak(val, drvname, valMax, valLen);
	break;
    case SQL_DRIVER_VER:
	strmak(val, "02.50", valMax, valLen);
	break;
    case SQL_FETCH_DIRECTION:
	*(SQLUINTEGER *) val = SQL_FD_FETCH_NEXT | SQL_FD_FETCH_FIRST |
	    SQL_FD_FETCH_LAST | SQL_FD_FETCH_PRIOR | SQL_FD_FETCH_ABSOLUTE;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_ODBC_VER:
	strmak(val, "02.50", valMax, valLen);
	break;
    case SQL_ODBC_SAG_CLI_CONFORMANCE:
	*(SQLSMALLINT *) val = SQL_OSCC_NOT_COMPLIANT;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_SERVER_NAME:
    case SQL_DATABASE_NAME:
	strmak(val, (d->dbname ? d->dbname : ""), valMax, valLen);
	break;
    case SQL_SEARCH_PATTERN_ESCAPE:
	strmak(val, "", valMax, valLen);
	break;
    case SQL_ODBC_SQL_CONFORMANCE:
	*(SQLSMALLINT *) val = SQL_OSC_MINIMUM;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_ODBC_API_CONFORMANCE:
	*(SQLSMALLINT *) val = SQL_OAC_LEVEL1;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_DBMS_NAME:
	strmak(val, "SQLite", valMax, valLen);
	break;
    case SQL_DBMS_VER:
	strmak(val, SQLITE_VERSION, valMax, valLen);
	break;
    case SQL_ROW_UPDATES:
    case SQL_ACCESSIBLE_PROCEDURES:
    case SQL_PROCEDURES:
    case SQL_EXPRESSIONS_IN_ORDERBY:
    case SQL_NEED_LONG_DATA_LEN:
    case SQL_ODBC_SQL_OPT_IEF:
    case SQL_LIKE_ESCAPE_CLAUSE:
    case SQL_ORDER_BY_COLUMNS_IN_SELECT:
    case SQL_OUTER_JOINS:
    case SQL_COLUMN_ALIAS:
    case SQL_ACCESSIBLE_TABLES:
    case SQL_MULT_RESULT_SETS:
    case SQL_MULTIPLE_ACTIVE_TXN:
    case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
	strmak(val, "N", valMax, valLen);
	break;
    case SQL_DATA_SOURCE_READ_ONLY:
	strmak(val, "N", valMax, valLen);
	break;
#ifdef SQL_OJ_CAPABILITIES
    case SQL_OJ_CAPABILITIES:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
#endif
#ifdef SQL_MAX_IDENTIFIER_LEN
    case SQL_MAX_IDENTIFIER_LEN:
	*(SQLUSMALLINT *) val = 255;
	*valLen = sizeof (SQLUSMALLINT);
	break;
#endif
    case SQL_CONCAT_NULL_BEHAVIOR:
	*(SQLSMALLINT *) val = SQL_CB_NULL;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_CURSOR_COMMIT_BEHAVIOR:
    case SQL_CURSOR_ROLLBACK_BEHAVIOR:
	*(SQLSMALLINT *) val = SQL_CB_PRESERVE;
	*valLen = sizeof (SQLSMALLINT);
	break;
#ifdef SQL_CURSOR_SENSITIVITY
    case SQL_CURSOR_SENSITIVITY:
	*(SQLUINTEGER *) val = SQL_UNSPECIFIED;
	*valLen = sizeof (SQLUINTEGER);
	break;
#endif
    case SQL_DEFAULT_TXN_ISOLATION:
	*(SQLUINTEGER *) val = SQL_TXN_READ_UNCOMMITTED;
	*valLen = sizeof (SQLUINTEGER);
	break;
#ifdef SQL_DESCRIBE_PARAMETER
    case SQL_DESCRIBE_PARAMETER:
	strmak(val, "Y", valMax, valLen);
	break;
#endif
    case SQL_TXN_ISOLATION_OPTION:
	*(SQLUINTEGER *) val = SQL_TXN_READ_UNCOMMITTED;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_IDENTIFIER_CASE:
	*(SQLSMALLINT *) val = SQL_IC_SENSITIVE;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_IDENTIFIER_QUOTE_CHAR:
	strmak(val, " ", valMax, valLen);
	break;
    case SQL_MAX_TABLE_NAME_LEN:
    case SQL_MAX_COLUMN_NAME_LEN:
	*(SQLSMALLINT *) val = 255;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_MAX_CURSOR_NAME_LEN:
	*(SWORD *) val = 255;
	*valLen = sizeof (SWORD);
	break;
    case SQL_MAX_PROCEDURE_NAME_LEN:
	*(SQLSMALLINT *) val = 0;
	break;
    case SQL_MAX_QUALIFIER_NAME_LEN:
    case SQL_MAX_OWNER_NAME_LEN:
	*(SQLSMALLINT *) val = 255;
	break;
    case SQL_OWNER_TERM:
	strmak(val, "owner", valMax, valLen);
	break;
    case SQL_PROCEDURE_TERM:
	strmak(val, "procedure", valMax, valLen);
	break;
    case SQL_QUALIFIER_NAME_SEPARATOR:
	strmak(val, ".", valMax, valLen);
	break;
    case SQL_QUALIFIER_TERM:
	strmak(val, "database", valMax, valLen);
	break;
    case SQL_QUALIFIER_USAGE:
	*(SQLUINTEGER *) val = SQL_QU_DML_STATEMENTS | SQL_QU_TABLE_DEFINITION;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_SCROLL_CONCURRENCY:
	*(SQLUINTEGER *) val = SQL_SCCO_READ_ONLY;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_SCROLL_OPTIONS:
	*(SQLUINTEGER *) val = SQL_SO_STATIC | SQL_SO_FORWARD_ONLY;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_TABLE_TERM:
	strmak(val, "table", valMax, valLen);
	break;
    case SQL_TXN_CAPABLE:
	*(SQLSMALLINT *) val = SQL_TC_ALL;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_CONVERT_FUNCTIONS:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
       break;
    case SQL_SYSTEM_FUNCTIONS:
    case SQL_NUMERIC_FUNCTIONS:
    case SQL_STRING_FUNCTIONS:
    case SQL_TIMEDATE_FUNCTIONS:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_CONVERT_BIGINT:
    case SQL_CONVERT_BIT:
    case SQL_CONVERT_CHAR:
    case SQL_CONVERT_DATE:
    case SQL_CONVERT_DECIMAL:
    case SQL_CONVERT_DOUBLE:
    case SQL_CONVERT_FLOAT:
    case SQL_CONVERT_INTEGER:
    case SQL_CONVERT_LONGVARCHAR:
    case SQL_CONVERT_NUMERIC:
    case SQL_CONVERT_REAL:
    case SQL_CONVERT_SMALLINT:
    case SQL_CONVERT_TIME:
    case SQL_CONVERT_TIMESTAMP:
    case SQL_CONVERT_TINYINT:
    case SQL_CONVERT_VARCHAR:
	*(SQLUINTEGER *) val = 
	    SQL_CVT_CHAR | SQL_CVT_NUMERIC | SQL_CVT_DECIMAL |
	    SQL_CVT_INTEGER | SQL_CVT_SMALLINT | SQL_CVT_FLOAT | SQL_CVT_REAL |
	    SQL_CVT_DOUBLE | SQL_CVT_VARCHAR | SQL_CVT_LONGVARCHAR |
	    SQL_CVT_BIT | SQL_CVT_TINYINT | SQL_CVT_BIGINT |
	    SQL_CVT_DATE | SQL_CVT_TIME | SQL_CVT_TIMESTAMP;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_CONVERT_BINARY:
    case SQL_CONVERT_VARBINARY:
    case SQL_CONVERT_LONGVARBINARY:
    case SQL_POSITIONED_STATEMENTS:
    case SQL_LOCK_TYPES:
    case SQL_BOOKMARK_PERSISTENCE:
    case SQL_OWNER_USAGE:
    case SQL_SUBQUERIES:
    case SQL_UNION:
    case SQL_TIMEDATE_ADD_INTERVALS:
    case SQL_TIMEDATE_DIFF_INTERVALS:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_QUOTED_IDENTIFIER_CASE:
	*(SQLUSMALLINT *) val = SQL_IC_SENSITIVE;
	*valLen = sizeof (SQLUSMALLINT);
	break;
    case SQL_POS_OPERATIONS:
	*(SQLUINTEGER *) val = SQL_POS_POSITION;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_ALTER_TABLE:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_CORRELATION_NAME:
	*(SQLSMALLINT *) val = SQL_CN_DIFFERENT;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_NON_NULLABLE_COLUMNS:
	*(SQLSMALLINT *) val = SQL_NNC_NON_NULL;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_NULL_COLLATION:
	*(SQLSMALLINT *) val = SQL_NC_START;
	*valLen = sizeof(SQLSMALLINT);
	break;
    case SQL_MAX_COLUMNS_IN_GROUP_BY:
    case SQL_MAX_COLUMNS_IN_ORDER_BY:
    case SQL_MAX_COLUMNS_IN_SELECT:
    case SQL_MAX_COLUMNS_IN_TABLE:
    case SQL_MAX_ROW_SIZE:
	*(SQLSMALLINT *) val = 0;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_MAX_TABLES_IN_SELECT:
	*(SQLSMALLINT *) val = 1;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_MAX_BINARY_LITERAL_LEN:
    case SQL_MAX_CHAR_LITERAL_LEN:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_MAX_COLUMNS_IN_INDEX:
	*(SQLSMALLINT *) val = 0;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_MAX_INDEX_SIZE:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof(SQLUINTEGER);
	break;
#ifdef SQL_MAX_IDENTIFIER_LENGTH
    case SQL_MAX_IDENTIFIER_LENGTH:
	*(SQLUINTEGER *) val = 255;
	*valLen = sizeof (SQLUINTEGER);
	break;
#endif
    case SQL_MAX_STATEMENT_LEN:
	*(SQLUINTEGER *) val = 16384;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_QUALIFIER_LOCATION:
	*(SQLSMALLINT *) val = SQL_QL_START;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_GETDATA_EXTENSIONS:
    case SQL_STATIC_SENSITIVITY:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_FILE_USAGE:
	*(SQLSMALLINT *) val = SQL_FILE_NOT_SUPPORTED;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_GROUP_BY:
	*(SQLSMALLINT *) val = 0;
	*valLen = sizeof (SQLSMALLINT);
	break;
    case SQL_KEYWORDS:
	strmak(val, "SELECT,DROP,DELETE,UPDATE,INSERT,"
	       "INTO,VALUES,TABLE,FROM,SET,WHERE,AND,CURRENT,OF",
	       valMax, valLen);
	break;
    case SQL_SPECIAL_CHARACTERS:
	strmak(val, "", valMax, valLen);
	break;
    case SQL_BATCH_SUPPORT:
    case SQL_BATCH_ROW_COUNT:
    case SQL_PARAM_ARRAY_ROW_COUNTS:
	*(SQLUINTEGER *) val = 0;
	*valLen = sizeof (SQLUINTEGER);
	break;
    case SQL_STATIC_CURSOR_ATTRIBUTES1:
	*(SQLUINTEGER *) val =
	    SQL_CA1_NEXT | SQL_CA1_ABSOLUTE | SQL_CA1_RELATIVE;
	*valLen = sizeof (SQLUINTEGER);
	break;	
    case SQL_STATIC_CURSOR_ATTRIBUTES2:
	*(SQLUINTEGER *) val = SQL_CA2_READ_ONLY_CONCURRENCY;
	*valLen = sizeof (SQLUINTEGER);
	break;	
    default:
	sprintf(d->logmsg, "unsupported info option %d", type);
	strcpy(d->sqlstate, "S1C00");
	return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

/**
 * Return information about supported ODBC API functions.
 * @param dbc database connection handle
 * @param func function code to be retrieved
 * @param flags output indicator
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetFunctions(SQLHDBC dbc, SQLUSMALLINT func,
		SQLUSMALLINT *flags)
{
    DBC *d;
    static int initialized = 0;
    static SQLUSMALLINT exists[100];

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;

    if (!initialized) {
	memset(exists, 0, sizeof (exists));
	exists[SQL_API_SQLALLOCCONNECT] = SQL_TRUE;
	exists[SQL_API_SQLFETCH] = SQL_TRUE;
	exists[SQL_API_SQLALLOCENV] = SQL_TRUE;
	exists[SQL_API_SQLFREECONNECT] = SQL_TRUE;
	exists[SQL_API_SQLALLOCSTMT] = SQL_TRUE;
	exists[SQL_API_SQLFREEENV] = SQL_TRUE;
	exists[SQL_API_SQLBINDCOL] = SQL_TRUE;
	exists[SQL_API_SQLFREESTMT] = SQL_TRUE;
	exists[SQL_API_SQLCANCEL] = SQL_TRUE;
	exists[SQL_API_SQLGETCURSORNAME] = SQL_TRUE;
	exists[SQL_API_SQLCOLATTRIBUTES] = SQL_TRUE;
	exists[SQL_API_SQLNUMRESULTCOLS] = SQL_TRUE;
	exists[SQL_API_SQLCONNECT] = SQL_TRUE;
	exists[SQL_API_SQLPREPARE] = SQL_TRUE;
	exists[SQL_API_SQLDESCRIBECOL] = SQL_TRUE;
	exists[SQL_API_SQLROWCOUNT] = SQL_TRUE;
	exists[SQL_API_SQLDISCONNECT] = SQL_TRUE;
	exists[SQL_API_SQLSETCURSORNAME] = SQL_TRUE;
	exists[SQL_API_SQLERROR] = SQL_TRUE;
	exists[SQL_API_SQLSETPARAM] = SQL_TRUE;
	exists[SQL_API_SQLEXECDIRECT] = SQL_TRUE;
	exists[SQL_API_SQLTRANSACT] = SQL_TRUE;
	exists[SQL_API_SQLEXECUTE] = SQL_TRUE;
	exists[SQL_API_SQLBINDPARAMETER] = SQL_TRUE;
	exists[SQL_API_SQLGETTYPEINFO] = SQL_TRUE;
	exists[SQL_API_SQLCOLUMNS] = SQL_TRUE;
	exists[SQL_API_SQLPARAMDATA] = SQL_TRUE;
	exists[SQL_API_SQLDRIVERCONNECT] = SQL_TRUE;
	exists[SQL_API_SQLPUTDATA] = SQL_TRUE;
	exists[SQL_API_SQLGETCONNECTOPTION] = SQL_TRUE;
	exists[SQL_API_SQLSETCONNECTOPTION] = SQL_TRUE;
	exists[SQL_API_SQLGETDATA] = SQL_TRUE;
	exists[SQL_API_SQLSETSTMTOPTION] = SQL_TRUE;
	exists[SQL_API_SQLGETFUNCTIONS] = SQL_TRUE;
	exists[SQL_API_SQLSPECIALCOLUMNS] = SQL_TRUE;
	exists[SQL_API_SQLGETINFO] = SQL_TRUE;
	exists[SQL_API_SQLSTATISTICS] = SQL_TRUE;
	exists[SQL_API_SQLGETSTMTOPTION] = SQL_TRUE;
	exists[SQL_API_SQLTABLES] = SQL_TRUE;
	exists[SQL_API_SQLBROWSECONNECT] = SQL_FALSE;
	exists[SQL_API_SQLNUMPARAMS] = SQL_TRUE;
	exists[SQL_API_SQLCOLUMNPRIVILEGES] = SQL_FALSE;
	exists[SQL_API_SQLPARAMOPTIONS] = SQL_FALSE;
	exists[SQL_API_SQLDATASOURCES] = SQL_TRUE;
	exists[SQL_API_SQLPRIMARYKEYS] = SQL_TRUE;
	exists[SQL_API_SQLDESCRIBEPARAM] = SQL_TRUE;
	exists[SQL_API_SQLPROCEDURECOLUMNS] = SQL_FALSE;
	exists[SQL_API_SQLDRIVERS] = SQL_FALSE;
	exists[SQL_API_SQLPROCEDURES] = SQL_FALSE;
	exists[SQL_API_SQLEXTENDEDFETCH] = SQL_TRUE;
	exists[SQL_API_SQLSETPOS] = SQL_TRUE;
	exists[SQL_API_SQLFOREIGNKEYS] = SQL_TRUE;
	exists[SQL_API_SQLSETSCROLLOPTIONS] = SQL_TRUE;
	exists[SQL_API_SQLMORERESULTS] = SQL_TRUE;
	exists[SQL_API_SQLTABLEPRIVILEGES] = SQL_FALSE;
	exists[SQL_API_SQLNATIVESQL] = SQL_FALSE;
	initialized = 1;
    }
    if (func == SQL_API_ALL_FUNCTIONS) {
	memcpy(flags, exists, sizeof (exists));
    } else if (func == SQL_API_ODBC3_ALL_FUNCTIONS) {
	int i;

	for (i = 0; i < SQL_API_ODBC3_ALL_FUNCTIONS_SIZE; i++)
	    if (i < array_size(exists) && exists[i])
		flags[i >> 4] |= (1 << (i & 0xF));
	    else
		flags[i >> 4] &= ~(1 << (i & 0xF));
    } else {
	if (func < array_size(exists)) {
	    *flags = exists[func];
	} else {
	    *flags = 0;
	}
    }
    return SQL_SUCCESS;
}

/**
 * Internal allocate HENV.
 * @param env pointer to environment handle
 * @result ODBC error code
 */

static SQLRETURN
drvallocenv(SQLHENV *env)
{
    ENV *e;

    if (env == NULL) {
	return SQL_INVALID_HANDLE;
    }
    e = (ENV *) xmalloc(sizeof (ENV));
    if (e == NULL) {
	*env = SQL_NULL_HENV;
	return SQL_ERROR;
    }
    e->magic = ENV_MAGIC;
    e->dbcs = NULL;
    *env = (SQLHENV) e;
    return SQL_SUCCESS;
}

/**
 * Allocate HENV.
 * @param env pointer to environment handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLAllocEnv(SQLHENV *env)
{
    return drvallocenv(env);
}

/**
 * Internal free HENV.
 * @param env environment handle
 * @result ODBC error code
 */

static SQLRETURN
drvfreeenv(SQLHENV env)
{
    ENV *e;

    if (env == SQL_NULL_HENV) {
	return SQL_INVALID_HANDLE;
    }
    e = (ENV *) env;
    if (e->magic != ENV_MAGIC) {
	return SQL_SUCCESS;
    }
    if (e->dbcs) {
	return SQL_ERROR;
    }
    e->magic = DEAD_MAGIC;
    xfree(e);
    return SQL_SUCCESS;
}

/**
 * Free HENV.
 * @param env environment handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLFreeEnv(SQLHENV env)
{
    return drvfreeenv(env);
}

/**
 * Internal allocate HDBC.
 * @param env environment handle
 * @param dbc pointer to database connection handle
 * @result ODBC error code
 */

static SQLRETURN
drvallocconnect(SQLHENV env, SQLHDBC *dbc)
{
    DBC *d;
    ENV *e;

    if (dbc == NULL) {
	return SQL_ERROR;
    }
    d = (DBC *) xmalloc(sizeof (DBC));
    if (d == NULL) {
	*dbc = SQL_NULL_HDBC;
	return SQL_ERROR;
    }
    memset(d, 0, sizeof (DBC));
#ifdef ASYNC
    d->curtype = SQL_CURSOR_STATIC;
#ifdef HAVE_PTHREAD
    if (pthread_mutex_init(&d->mut, NULL)) {
	goto error;
    }
    if (pthread_cond_init(&d->cond, NULL)) {
	pthread_mutex_destroy(&d->mut);
error:
	xfree(d);
	return SQL_ERROR;
    }
#endif
#ifdef _WIN32
    d->ev_res = CreateEvent(NULL, 0, 0, NULL);
    if (d->ev_res == INVALID_HANDLE_VALUE) {
	goto error;
    }
    d->ev_cont = CreateEvent(NULL, 0, 0, NULL);
    if (d->ev_cont == INVALID_HANDLE_VALUE) {
	CloseHandle(d->ev_res);
error:
	xfree(d);
	return SQL_ERROR;
    }
#endif
#endif
    e = (ENV *) env;
    if (e->magic == ENV_MAGIC) {
	DBC *n, *p;

	d->env = e;
	p = NULL;
	n = e->dbcs;
	while (n) {
	    p = n;
	    n = n->next;
	}
	if (p) {
	    p->next = d;
	} else {
	    e->dbcs = d;
	}
    }
    d->autocommit = 1;
    d->magic = DBC_MAGIC;
    *dbc = (SQLHDBC) d;
    return SQL_SUCCESS;
}

/**
 * Allocate HDBC.
 * @param env environment handle
 * @param dbc pointer to database connection handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLAllocConnect(SQLHENV env, SQLHDBC *dbc)
{
    return drvallocconnect(env, dbc);
}

/**
 * Internal free connection (HDBC).
 * @param dbc database connection handle
 * @result ODBC error code
 */

static SQLRETURN
drvfreeconnect(SQLHDBC dbc)
{
    DBC *d;
    ENV *e;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (d->magic != DBC_MAGIC) {
	return SQL_INVALID_HANDLE;
    }
    if (d->sqlite) {
	strcpy(d->logmsg, "not disconnected");
	strcpy(d->sqlstate, "S1000");
	return SQL_ERROR;
    }
    while (d->stmt) {
	freestmt((HSTMT) d->stmt);
    }
    e = d->env;
    if (e && e->magic == ENV_MAGIC) {
	DBC *n, *p;

	p = NULL;
	n = e->dbcs;
	while (n) {
	    if (n == d) {
		break;
	    }
	    p = n;
	    n = n->next;
	}
	if (n) {
	    if (p) {
		p->next = d->next;
	    } else {
		e->dbcs = d->next;
	    }
	}
    }
#ifdef ASYNC
#ifdef HAVE_PTHREAD
    pthread_cond_destroy(&d->cond);
    pthread_mutex_destroy(&d->mut);
#endif
#ifdef _WIN32
    CloseHandle(d->ev_res);
    CloseHandle(d->ev_cont);
#endif
#endif
    d->magic = DEAD_MAGIC;
    xfree(d);
    return SQL_SUCCESS;
}

/**
 * Free connection (HDBC).
 * @param dbc database connection handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLFreeConnect(SQLHDBC dbc)
{
    return drvfreeconnect(dbc);
}

/**
 * Internal get connect attribute of HDBC.
 * @param dbc database connection handle
 * @param attr option to be retrieved
 * @param val output buffer
 * @param bufmax size of output buffer
 * @param buflen output length
 * @result ODBC error code
 */

static SQLRETURN
drvgetconnectattr(SQLHDBC dbc, SQLINTEGER attr, SQLPOINTER val,
		  SQLINTEGER bufmax, SQLINTEGER *buflen)
{
    DBC *d;
    SQLINTEGER dummy;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (!val) {
	val = (SQLPOINTER) &dummy;
    }
    if (!buflen) {
	buflen = &dummy;
    }
    switch (attr) {
    case SQL_ATTR_ACCESS_MODE:
	*(SQLINTEGER *) val = SQL_MODE_READ_WRITE;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_AUTOCOMMIT:
	*(SQLINTEGER *) val =
	    d->autocommit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_LOGIN_TIMEOUT:
	*(SQLINTEGER *) val = 100;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_ODBC_CURSORS:
	*(SQLINTEGER *) val = SQL_CUR_USE_DRIVER;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_PACKET_SIZE:
	*(SQLINTEGER *) val = 16384;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_TXN_ISOLATION:
	*(SQLINTEGER *) val = SQL_TXN_READ_UNCOMMITTED;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_TRACE:
    case SQL_ATTR_TRACEFILE:
    case SQL_ATTR_QUIET_MODE:
    case SQL_ATTR_TRANSLATE_OPTION:
    case SQL_ATTR_KEYSET_SIZE:
    case SQL_ATTR_QUERY_TIMEOUT:
    case SQL_ATTR_PARAM_BIND_TYPE:
    case SQL_ATTR_ROW_BIND_TYPE:
    case SQL_ATTR_CURRENT_CATALOG:
	*(SQLINTEGER *) val = 0;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_USE_BOOKMARKS:
	*(SQLINTEGER *) val = SQL_UB_OFF;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_ASYNC_ENABLE:
	*(SQLINTEGER *) val = SQL_ASYNC_ENABLE_OFF;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_NOSCAN:
	*(SQLINTEGER *) val = SQL_NOSCAN_ON;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_CONCURRENCY:
	*(SQLINTEGER *) val = SQL_CONCUR_ROWVER;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_SIMULATE_CURSOR:
	*(SQLINTEGER *) val = SQL_SC_NON_UNIQUE;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_MAX_ROWS:
    case SQL_ATTR_MAX_LENGTH:
	*(SQLINTEGER *) val = 1000000000;
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_CURSOR_TYPE:
#ifdef ASYNC
	*(SQLINTEGER *) val = d->curtype;
#else
	*(SQLINTEGER *) val = SQL_CURSOR_STATIC;
#endif
	*buflen = sizeof (SQLINTEGER);
	break;
    case SQL_ATTR_RETRIEVE_DATA:
	*(SQLINTEGER *) val = SQL_RD_ON;
	*buflen = sizeof (SQLINTEGER);
	break;
    default:
	*(SQLINTEGER *) val = 0;
	*buflen = sizeof (SQLINTEGER);
	sprintf(d->logmsg, "unsupported connect attribute %d", attr);
	strcpy(d->sqlstate, "S1C00");
	return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

/**
 * Get connect attribute of HDBC.
 * @param dbc database connection handle
 * @param attr option to be retrieved
 * @param val output buffer
 * @param bufmax size of output buffer
 * @param buflen output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetConnectAttr(SQLHDBC dbc, SQLINTEGER attr, SQLPOINTER val,
		  SQLINTEGER bufmax, SQLINTEGER *buflen)
{
    return drvgetconnectattr(dbc, attr, val, bufmax, buflen);
}

/**
 * Internal set connect attribute of HDBC.
 * @param dbc database connection handle
 * @param attr option to be set
 * @param val option value
 * @param len size of option
 * @result ODBC error code
 */

SQLRETURN SQL_API
drvsetconnectattr(SQLHDBC dbc, SQLINTEGER attr, SQLPOINTER val,
		  SQLINTEGER len)
{
    DBC *d;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    switch (attr) {
    case SQL_AUTOCOMMIT:
	if (val && len >= sizeof (SQLINTEGER)) {
	    d->autocommit = *((SQLINTEGER *) val) == SQL_AUTOCOMMIT_ON;
	    if (d->autocommit && d->intrans) {
		return endtran(d, SQL_COMMIT);
#ifdef ASYNC
	    } else if (!d->autocommit) {
		async_end(d->async_stmt);
#endif
	    }
	}
	break;
    }
    return SQL_SUCCESS;
}

/**
 * Set connect attribute of HDBC.
 * @param dbc database connection handle
 * @param attr option to be set
 * @param val option value
 * @param len size of option
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSetConnectAttr(SQLHDBC dbc, SQLINTEGER attr, SQLPOINTER val,
		  SQLINTEGER len)
{
    return drvsetconnectattr(dbc, attr, val, len);
}

/**
 * Internal get connect option of HDBC.
 * @param dbc database connection handle
 * @param opt option to be retrieved
 * @param param output buffer
 * @result ODBC error code
 */

static SQLRETURN
drvgetconnectoption(SQLHDBC dbc, SQLUSMALLINT opt, SQLPOINTER param)
{
    DBC *d;
    SQLINTEGER dummy;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (!param) {
	param = (SQLPOINTER) &dummy;
    }
    switch (opt) {
    case SQL_ACCESS_MODE:
	*(SQLINTEGER *) param = SQL_MODE_READ_WRITE;
	break;
    case SQL_AUTOCOMMIT:
	*(SQLINTEGER *) param =
	    d->autocommit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
	break;
    case SQL_LOGIN_TIMEOUT:
	*(SQLINTEGER *) param = 100;
	break;
    case SQL_ODBC_CURSORS:
	*(SQLINTEGER *) param = SQL_CUR_USE_DRIVER;
	break;
    case SQL_PACKET_SIZE:
	*(SQLINTEGER *) param = 16384;
	break;
    case SQL_TXN_ISOLATION:
	*(SQLINTEGER *) param = SQL_TXN_READ_UNCOMMITTED;
	break;
    case SQL_OPT_TRACE:
    case SQL_OPT_TRACEFILE:
    case SQL_QUIET_MODE:
    case SQL_TRANSLATE_DLL:
    case SQL_TRANSLATE_OPTION:
    case SQL_KEYSET_SIZE:
    case SQL_QUERY_TIMEOUT:
    case SQL_BIND_TYPE:
    case SQL_CURRENT_QUALIFIER:
	*(SQLINTEGER *) param = 0;
	break;
    case SQL_USE_BOOKMARKS:
	*(SQLINTEGER *) param = SQL_UB_OFF;
	break;
    case SQL_ASYNC_ENABLE:
	*(SQLINTEGER *) param = SQL_ASYNC_ENABLE_OFF;
	break;
    case SQL_NOSCAN:
	*(SQLINTEGER *) param = SQL_NOSCAN_ON;
	break;
    case SQL_CONCURRENCY:
	*(SQLINTEGER *) param = SQL_CONCUR_ROWVER;
	break;
    case SQL_SIMULATE_CURSOR:
	*(SQLINTEGER *) param = SQL_SC_NON_UNIQUE;
	break;
    case SQL_ROWSET_SIZE:
    case SQL_MAX_ROWS:
    case SQL_MAX_LENGTH:
	*(SQLINTEGER *) param = 1000000000;
	break;
    case SQL_CURSOR_TYPE:
#ifdef ASYNC
	*(SQLINTEGER *) param = d->curtype;
#else
	*(SQLINTEGER *) param = SQL_CURSOR_STATIC;
#endif
	break;
    case SQL_RETRIEVE_DATA:
	*(SQLINTEGER *) param = SQL_RD_ON;
	break;
    default:
	*(SQLINTEGER *) param = 0;
	sprintf(d->logmsg, "unsupported connect option %d", opt);
	strcpy(d->sqlstate, "S1C00");
	return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

/**
 * Get connect option of HDBC.
 * @param dbc database connection handle
 * @param opt option to be retrieved
 * @param param output buffer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetConnectOption(SQLHDBC dbc, SQLUSMALLINT opt, SQLPOINTER param)
{
    return drvgetconnectoption(dbc, opt, param);
}

/**
 * Internal set option on HDBC.
 * @param dbc database connection handle
 * @param opt option to be set
 * @param param option value
 * @result ODBC error code
 */

static SQLRETURN
drvsetconnectoption(SQLHDBC dbc, SQLUSMALLINT opt, SQLUINTEGER param)
{
    DBC *d;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    switch (opt) {
    case SQL_AUTOCOMMIT:
	d->autocommit = param == SQL_AUTOCOMMIT_ON;
	if (d->autocommit && d->intrans) {
	    return endtran(d, SQL_COMMIT);
#ifdef ASYNC
	} else if (!d->autocommit) {
	    async_end(d->async_stmt);
#endif
	}
	break;
    }
    return SQL_SUCCESS;
}

/**
 * Set option on HDBC.
 * @param dbc database connection handle
 * @param opt option to be set
 * @param param option value
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSetConnectOption(SQLHDBC dbc, SQLUSMALLINT opt, SQLUINTEGER param)
{
    return drvsetconnectoption(dbc, opt, param);
}

#if defined(WITHOUT_DRIVERMGR) || !defined(_WIN32)

/**
 * Handling of SQLConnect() connection attributes
 * for standalone operation without driver manager.
 * @param dsn DSN/driver connection string
 * @param attr attribute string to be retrieved
 * @param out output buffer
 * @param outLen length of output buffer
 * @result true or false
 */

static int
getdsnattr(char *dsn, char *attr, char *out, int outLen)
{
    char *str = dsn, *start;
    int len = strlen(attr);

    while (*str) {
	while (*str && *str == ';') {
	    ++str;
	}
	start = str;
	if ((str = strchr(str, '=')) == NULL) {
	    return 0;
	}
	if (str - start == len &&
#ifdef _WIN32
	    _strnicmp(start, attr, len) == 0
#else
	    strncasecmp(start, attr, len) == 0
#endif
	   ) {
	    start = ++str;
	    while (*str && *str != ';') {
		++str;
	    }
	    len = min(outLen - 1, str - start);
	    strncpy(out, start, len);
	    out[len] = '\0';
	    return 1;
	}
	while (*str && *str != ';') {
	    ++str;
	}
    }
    return 0;
}
#endif

/**
 * Connect to SQLite database.
 * @param dbc database connection handle
 * @param dsn DSN string
 * @param dsnLen length of DSN string or SQL_NTS
 * @param uid user id string or NULL
 * @param uidLen length of user id string or SQL_NTS
 * @param pass password string or NULL
 * @param passLen length of password string or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLConnect(SQLHDBC dbc, SQLCHAR *dsn, SQLSMALLINT dsnLen,
	   SQLCHAR *uid, SQLSMALLINT uidLen,
	   SQLCHAR *pass, SQLSMALLINT passLen)
{
    DBC *d;
    int len, tmp, busyto = 1000;
    char buf[SQL_MAX_MESSAGE_LENGTH], dbname[SQL_MAX_MESSAGE_LENGTH / 4];
    char busy[SQL_MAX_MESSAGE_LENGTH / 4];
#ifdef ASYNC
    char tflag[32];
#endif
    char *errp;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (d->magic != DBC_MAGIC) {
	return SQL_INVALID_HANDLE;
    }
    if (d->sqlite != NULL) {
	strcpy(d->logmsg, "connection already established");
	strcpy(d->sqlstate, "08002");
	return SQL_ERROR;
    }
    buf[0] = '\0';
    if (dsnLen == SQL_NTS) {
	len = sizeof (buf) - 1;
    } else {
	len = min(sizeof (buf) - 1, dsnLen);
    }
    if (dsn != NULL) {
	strncpy(buf, dsn, len);
    }
    buf[len] = '\0';
    if (buf[0] == '\0') {
	strcpy(d->logmsg, "invalid DSN");
	strcpy(d->sqlstate, "S1090");
	return SQL_ERROR;
    }
    busy[0] = '\0';
    dbname[0] = '\0';
#ifdef WITHOUT_DRIVERMGR
    getdsnattr(buf, "database", dbname, sizeof (dbname));
    if (dbname[0] == '\0') {
	strncpy(dbname, buf, sizeof (dbname));
	dbname[sizeof (dbname) - 1] = '\0';
    }
    getdsnattr(buf, "timeout", busy, sizeof (busy));
#ifdef ASYNC
    tflag[0] = '\0';
    getdsnattr(buf, "threaded", tflag, sizeof (tflag));
#endif
#else
    SQLGetPrivateProfileString(buf, "timeout", "1000",
			       busy, sizeof (busy), ODBC_INI);
    SQLGetPrivateProfileString(buf, "database", "",
			       dbname, sizeof (dbname), ODBC_INI);
#ifdef ASYNC
    SQLGetPrivateProfileString(buf, "threaded", "",
			       tflag, sizeof (tflag), ODBC_INI);
#endif
#endif
    tmp = strtoul(busy, &errp, 0);
    if (errp && *errp == '\0' && errp != busy) {
	busyto = tmp;
    }
    d->sqlite = sqlite_open(dbname, 0, &errp);
    if (d->sqlite == NULL) {
	if (errp) {
	    strncpy(d->logmsg, errp, sizeof (d->logmsg));
	    d->logmsg[sizeof (d->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    strcpy(d->logmsg, "connect failed");
	}
	strcpy(d->sqlstate, "S1000");
	return SQL_ERROR;
    }
    freep(&errp);
#ifdef ASYNC
    d->thread_enable = getbool(tflag);
    if (d->thread_enable) {
	d->sqlite2 = sqlite_open(dbname, 0, NULL);
	if (d->sqlite2 == NULL) {
	    d->thread_enable = 0;
	}
    }
    d->curtype = d->thread_enable ? SQL_CURSOR_FORWARD_ONLY : SQL_CURSOR_STATIC;
#endif
    sqlite_busy_timeout(d->sqlite, busyto);
    setsqliteopts(d->sqlite);
#ifdef ASYNC
    if (d->sqlite2) {
	sqlite_busy_timeout(d->sqlite2, busyto);
	setsqliteopts(d->sqlite2);
    }
#endif
    freep(&d->dbname);
    d->dbname = xstrdup(dbname);
    freep(&d->dsn);
    d->dsn = xstrdup(buf);
    return SQL_SUCCESS;
}

/**
 * Disconnect given HDBC.
 * @param dbc database connection handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLDisconnect(SQLHDBC dbc)
{
    DBC *d;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (d->magic != DBC_MAGIC) {
	return SQL_INVALID_HANDLE;
    }
    if (d->intrans) {
	strcpy(d->logmsg, "incomplete transaction");
	strcpy(d->sqlstate, "25000");
	return SQL_ERROR;
    }
#ifdef ASYNC
    async_end(d->async_stmt);
#endif
    if (d->sqlite) {
	sqlite_close(d->sqlite);
	d->sqlite = NULL;
    }
#ifdef ASYNC
    if (d->sqlite2) {
	sqlite_close(d->sqlite2);
	d->sqlite2 = NULL;
    }
#endif
    freep(&d->dbname);
    freep(&d->dsn);
    return SQL_SUCCESS;
}

#if defined(WITHOUT_DRIVERMGR) || !defined(_WIN32)

/**
 * Standalone (w/o driver manager) database connect.
 * @param dbc database connection handle
 * @param hwnd dummy window handle or NULL
 * @param connIn driver connect input string
 * @param connInLen length of driver connect input string or SQL_NTS
 * @param connOut driver connect output string
 * @param connOutMax length of driver connect output string
 * @param connOutLen output length of driver connect output string
 * @param drvcompl completion type
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLDriverConnect(SQLHDBC dbc, SQLHWND hwnd,
		 SQLCHAR *connIn, SQLSMALLINT connInLen,
		 SQLCHAR *connOut, SQLSMALLINT connOutMax,
		 SQLSMALLINT *connOutLen, SQLUSMALLINT drvcompl)
{
    DBC *d;
    int len, busyto = 1000, tmp;
    char buf[SQL_MAX_MESSAGE_LENGTH], dbname[SQL_MAX_MESSAGE_LENGTH / 4];
    char dsn[SQL_MAX_MESSAGE_LENGTH / 4], busy[SQL_MAX_MESSAGE_LENGTH / 4];
#ifdef ASYNC
    char tflag[32];
#endif
    char *errp;

    if (dbc == SQL_NULL_HDBC || hwnd != NULL) {
	return SQL_INVALID_HANDLE;
    }
    if (drvcompl != SQL_DRIVER_COMPLETE &&
	drvcompl != SQL_DRIVER_COMPLETE_REQUIRED &&
	drvcompl != SQL_DRIVER_PROMPT &&
	drvcompl != SQL_DRIVER_NOPROMPT) {
	return SQL_NO_DATA;
    }
    d = (DBC *) dbc;
    if (d->sqlite) {
	strcpy(d->logmsg, "connection already established");
	strcpy(d->sqlstate, "08002");
	return SQL_ERROR;
    }
    buf[0] = '\0';
    if (connInLen == SQL_NTS) {
	len = sizeof (buf) - 1;
    } else {
	len = min(connInLen, sizeof (buf) - 1);
    }
    if (connIn != NULL) {
	strncpy(buf, connIn, len);
    }
    buf[len] = '\0';
    if (!buf[0]) {
	strcpy(d->logmsg, "invalid connect attributes");
	strcpy(d->sqlstate, "S1090");
	return SQL_ERROR;
    }
    dsn[0] = '\0';
    getdsnattr(buf, "DSN", dsn, sizeof (dsn));

    busy[0] = '\0';
    getdsnattr(buf, "timeout", busy, sizeof (busy));
#ifndef WITHOUT_DRIVERMGR
    if (dsn[0] && !busy[0]) {
	SQLGetPrivateProfileString(dsn, "timeout", "1000",
				   busy, sizeof (busy), ODBC_INI);
    }
#endif
    tmp = strtoul(busy, &errp, 0);
    if (errp && *errp == '\0' && errp != busy) {
	busyto = tmp;
    }
    dbname[0] = '\0';
    getdsnattr(buf, "database", dbname, sizeof (dbname));
#ifndef WITHOUT_DRIVERMGR
    if (dsn[0] && !dbname[0]) {
	SQLGetPrivateProfileString(dsn, "database", "",
				   dbname, sizeof (dbname), ODBC_INI);
    }
#endif
#ifdef ASYNC
    tflag[0] = '\0';
    getdsnattr(buf, "threaded", tflag, sizeof (tflag));
#ifndef WITHOUT_DRIVERMGR
    if (dsn[0] && !tflag[0]) {
	SQLGetPrivateProfileString(dsn, "threaded", "",
				   tflag, sizeof (tflag), ODBC_INI);
    }
#endif
#endif
    if (!dbname[0] && !dsn[0]) {
	strcpy(dsn, "SQLite");
	strncpy(dbname, buf, sizeof (dbname));
	dbname[sizeof (dbname) - 1] = '\0';
    }
    if (connOut || connOutLen) {
	char buf2[64];

	strcpy(buf, "DSN=");
	strcat(buf, dsn);
	strcat(buf, ";database=");
	strcat(buf, dbname);
	strcat(buf, ";timeout=");
	sprintf(buf2, "%d", busyto);
	strcat(buf, buf2);
#ifdef ASYNC
	strcat(buf, ";threaded=");
	strcat(buf, tflag);
#endif
	len = min(connOutMax - 1, strlen(buf));
	if (connOut) {
	    strncpy(connOut, buf, len);
	    connOut[connOutMax - 1] = '\0';
	}
	if (connOutLen) {
	    *connOutLen = len;
	}
    }
    d->sqlite = sqlite_open(dbname, 0, &errp);
    if (d->sqlite == NULL) {
	if (errp) {
	    strncpy(d->logmsg, errp, sizeof (d->logmsg));
	    d->logmsg[sizeof (d->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    strcpy(d->logmsg, "connect failed");
	}
	strcpy(d->sqlstate, "S1000");
	return SQL_ERROR;
    }
    freep(&errp);
#ifdef ASYNC
    d->thread_enable = getbool(tflag);
    if (d->thread_enable) {
	d->sqlite2 = sqlite_open(dbname, 0, NULL);
	if (d->sqlite2 == NULL) {
	    d->thread_enable = 0;
	}
    }
    d->curtype = d->thread_enable ? SQL_CURSOR_FORWARD_ONLY : SQL_CURSOR_STATIC;
#endif
    sqlite_busy_timeout(d->sqlite, busyto);
    setsqliteopts(d->sqlite);
#ifdef ASYNC
    if (d->sqlite2) {
	sqlite_busy_timeout(d->sqlite2, busyto);
	setsqliteopts(d->sqlite2);
    }
#endif
    freep(&d->dbname);
    d->dbname = xstrdup(dbname);
    freep(&d->dsn);
    d->dsn = xstrdup(dsn);
    return SQL_SUCCESS;
}
#endif

/* see doc on top */

static SQLRETURN
freestmt(SQLHSTMT stmt)
{
    STMT *s;
    DBC *d;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    freeresult(s, 1);
    freep(&s->query);
    d = (DBC *) s->dbc;
    if (d && d->magic == DBC_MAGIC) {
	STMT *p, *n;

	p = NULL;
	n = d->stmt;
	while (n) {
	    if (n == s) {
		break;
	    }
	    p = n;
	    n = n->next;
	}
	if (n) {
	    if (p) {
		p->next = s->next;
	    } else {
		d->stmt = s->next;
	    }
	}
    }
    freep(&s->bindparms);
    xfree(s);
    return SQL_SUCCESS;
}

/**
 * Allocate HSTMT given HDBC (driver internal version).
 * @param dbc database connection handle
 * @param stmt pointer to statement handle
 * @result ODBC error code
 */

static SQLRETURN
drvallocstmt(SQLHDBC dbc, SQLHSTMT *stmt)
{
    DBC *d;
    STMT *s, *sl, *pl;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (d->magic != DBC_MAGIC || stmt == NULL) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) xmalloc(sizeof (STMT));
    if (s == NULL) {
	*stmt = SQL_NULL_HSTMT;
	return SQL_ERROR;
    }
    *stmt = (SQLHSTMT) s;
    memset(s, 0, sizeof (STMT));
    s->dbc = dbc;
#ifdef ASYNC
    s->curtype = d->curtype;
    s->async_run = &d->async_run;
#endif
    sprintf(s->cursorname, "CUR_%08lX", (long) *stmt);
    sl = d->stmt;
    pl = NULL;
    while (sl) {
	pl = sl;
	sl = sl->next;
    }
    if (pl) {
	pl->next = s;
    } else {
	d->stmt = s;
    }
    return SQL_SUCCESS;
}

/**
 * Allocate HSTMT given HDBC.
 * @param dbc database connection handle
 * @param stmt pointer to statement handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLAllocStmt(SQLHDBC dbc, SQLHSTMT *stmt)
{
    return drvallocstmt(dbc, stmt);
}

/**
 * Internal function to perform certain kinds of free/close on STMT.
 * @param stmt statement handle
 * @param opt SQL_RESET_PARAMS, SQL_UNBIND, SQL_CLOSE, or SQL_DROP
 * @result ODBC error code
 */

static SQLRETURN
drvfreestmt(SQLHSTMT stmt, SQLUSMALLINT opt)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    switch (opt) {
    case SQL_RESET_PARAMS:
	freeparams(s);
	break;
    case SQL_UNBIND:
	unbindcols(s);
	break;
    case SQL_CLOSE:
#ifdef ASYNC
	async_end_if(s);
#endif
	freeresult(s, 1);
	break;
    case SQL_DROP:
#ifdef ASYNC
	async_end_if(s);
#endif
	return freestmt(stmt);
    default:
	strcpy(s->logmsg, "unsupported option");
	strcpy(s->sqlstate, "S1C00");
	return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

/**
 * Free HSTMT.
 * @param stmt statement handle
 * @param opt SQL_RESET_PARAMS, SQL_UNBIND, SQL_CLOSE, or SQL_DROP
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLFreeStmt(SQLHSTMT stmt, SQLUSMALLINT opt)
{
    return drvfreestmt(stmt, opt);
}

/**
 * Cancel HSTMT closing cursor.
 * @param stmt statement handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLCancel(SQLHSTMT stmt)
{
    return drvfreestmt(stmt, SQL_CLOSE);
}

/**
 * Internal function to get cursor name of STMT.
 * @param stmt statement handle
 * @param cursor output buffer
 * @param buflen length of output buffer
 * @param lenp output length
 * @result ODBC error code
 */

static SQLRETURN
drvgetcursorname(SQLHSTMT stmt, SQLCHAR *cursor, SQLSMALLINT buflen,
		 SQLSMALLINT *lenp)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (lenp && !cursor) {
	*lenp = strlen(s->cursorname);
	return SQL_SUCCESS;
    }
    if (cursor) {
	strncpy(cursor, s->cursorname, buflen);
	if (lenp) {
	    *lenp = min(strlen(s->cursorname), buflen);
	}
    }
    return SQL_SUCCESS;
}

/**
 * Get cursor name of STMT.
 * @param stmt statement handle
 * @param cursor output buffer
 * @param buflen length of output buffer
 * @param lenp output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetCursorName(SQLHSTMT stmt, SQLCHAR *cursor, SQLSMALLINT buflen,
		 SQLSMALLINT *lenp)
{
    return drvgetcursorname(stmt, cursor, buflen, lenp);
}

/**
 * Internal function to set cursor name on STMT.
 * @param stmt statement handle
 * @param cursor new cursor name
 * @param len length of cursor name or SQL_NTS
 * @result ODBC error code
 */

static SQLRETURN
drvsetcursorname(SQLHSTMT stmt, SQLCHAR *cursor, SQLSMALLINT len)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!cursor ||
	!((cursor[0] >= 'A' && cursor[0] <= 'Z') ||
	  (cursor[0] >= 'a' && cursor[0] <= 'z'))) {
	strcpy(s->logmsg, "invalid cursor name");
	strcpy(s->sqlstate, "S1C00");
	return SQL_ERROR;
    }
    if (len == SQL_NTS) {
	len = sizeof (s->cursorname) - 1;
    } else {
	len = min(sizeof (s->cursorname) - 1, len);
    }
    strncpy(s->cursorname, cursor, len);
    s->cursorname[len] = '\0';
    return SQL_SUCCESS;
}

/**
 * Set cursor name on STMT.
 * @param stmt statement handle
 * @param cursor new cursor name
 * @param len length of cursor name or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLSetCursorName(SQLHSTMT stmt, SQLCHAR *cursor, SQLSMALLINT len)
{
    return drvsetcursorname(stmt, cursor, len);
}

/**
 * Function not implemented.
 */

SQLRETURN SQL_API
SQLCloseCursor(SQLHSTMT stmt)
{
    return drvunimplstmt(stmt);
}

#if defined(WITHOUT_DRIVERMGR) || !defined(HAVE_IODBC)

/**
 * Allocate a HENV, HDBC, or HSTMT handle.
 * @param type handle type
 * @param input input handle (HENV, HDBC)
 * @param output pointer to output handle (HENV, HDBC, HSTMT)
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE *output)
{
    switch (type) {
    case SQL_HANDLE_ENV:
	return drvallocenv((SQLHENV *) output);
    case SQL_HANDLE_DBC:
	return drvallocconnect((SQLHENV) input, (SQLHDBC *) output);
    case SQL_HANDLE_STMT:
	return drvallocstmt((SQLHDBC) input, (SQLHSTMT *) output);
    }
    return SQL_ERROR;
}
#endif

#if defined(WITHOUT_DRIVERMGR) || !defined(HAVE_IODBC)

/**
 * Free a HENV, HDBC, or HSTMT handle.
 * @param type handle type
 * @param h handle (HENV, HDBC, or HSTMT)
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLFreeHandle(SQLSMALLINT type, SQLHANDLE h)
{
    switch (type) {
    case SQL_HANDLE_ENV:
	return drvfreeenv((SQLHENV) h);
    case SQL_HANDLE_DBC:
	return drvfreeconnect((SQLHDBC) h);
    case SQL_HANDLE_STMT:
	return drvfreestmt((SQLHSTMT) h, SQL_DROP);
    }
    return SQL_ERROR;
}
#endif

/**
 * Free dynamically allocated column information in STMT.
 * @param s statement pointer
 */

static void
freedyncols(STMT *s)
{
    if (s->dyncols) {
	int i;

	for (i = 0; i < s->dcols; i++) {
	    freep(&s->dyncols[i].typename);
	}
	if (s->cols == s->dyncols) {
	    s->cols = NULL;
	    s->ncols = 0;
	}
	freep(&s->dyncols);
    }
    s->dcols = 0;
}

/* see doc on top */

static void
freeresult(STMT *s, int clrcols)
{
    if (s->rows) {
	if (s->rowfree) {
	    s->rowfree(s->rows);
	    s->rowfree = NULL;
	}
	s->rows = NULL;
    }
    s->nrows = 0;
    if (clrcols) {
	freep(&s->bindcols);
	freedyncols(s);
	s->cols = NULL;
	s->ncols = 0;
    }
}

/* see doc on top */

static void
unbindcols(STMT *s)
{
    int i;

    for (i = 0; s->bindcols && i < s->ncols; i++) {
	s->bindcols[i].type = -1;
	s->bindcols[i].max = 0;
	s->bindcols[i].lenp = NULL;
	s->bindcols[i].valp = NULL;
	s->bindcols[i].index = i;
	s->bindcols[i].offs = 0;
    }
}

/* see doc on top */

static void
mkbindcols(STMT *s)
{
    freep(&s->bindcols);
    if (s->ncols > 0) {
	s->bindcols = (BINDCOL *) xmalloc(s->ncols * sizeof (BINDCOL));
	unbindcols(s);
    }
}

/**
 * Internal function to retrieve row data, used by SQLFetch() and
 * friends and SQLGetData().
 * @param s statement pointer
 * @param col column number, 0 based
 * @param type output data type
 * @param val output buffer
 * @param len length of output buffer
 * @param lenp output length
 * @param partial flag for partial data retrieval
 * @result ODBC error code
 */

static SQLRETURN
getrowdata(STMT *s, SQLUSMALLINT col, SQLSMALLINT type,
	   SQLPOINTER val, SQLINTEGER len, SQLINTEGER *lenp, int partial)
{
    char **data, valdummy[16];
    SQLINTEGER dummy;
    int valnull = 0;

    if (!s->rows) {
	return SQL_NO_DATA;
    }
    if (col >= s->ncols) {
	strcpy(s->logmsg, "invalid column");
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    if (s->rowp < 0 || s->rowp >= s->nrows) {
	return SQL_NO_DATA;
    }
    if (type == SQL_C_DEFAULT) {
	int nosign = s->cols[col].nosign;

	switch (s->cols[col].type) {
	case SQL_INTEGER:
	    type = nosign ? SQL_C_ULONG : SQL_C_LONG;
	    break;
	case SQL_TINYINT:
	    type = nosign ? SQL_C_UTINYINT : SQL_C_TINYINT;
	    break;
	case SQL_SMALLINT:
	    type = nosign ? SQL_C_USHORT : SQL_C_SHORT;
	    break;
	case SQL_FLOAT:
	    type = SQL_C_FLOAT;
	    break;
	case SQL_DOUBLE:
	    type = SQL_C_DOUBLE;
	    break;
	case SQL_TIMESTAMP:
	    type = SQL_C_TIMESTAMP;
	    break;
	case SQL_TIME:
	    type = SQL_C_TIME;
	    break;
	case SQL_DATE:
	    type = SQL_C_DATE;
	    break;
	default:
	    type = SQL_C_CHAR;
	    break;
	}
    }
    data = s->rows + s->ncols + (s->rowp * s->ncols) + col;
    if (!lenp) {
	lenp = &dummy;
    }
    if (!val) {
	valnull = 1;
	val = (SQLPOINTER) valdummy;
    }
    if (*data == NULL) {
	*lenp = SQL_NULL_DATA;
	switch (type) {
	case SQL_C_UTINYINT:
	case SQL_C_TINYINT:
	case SQL_C_STINYINT:
	    *((char *) val) = 0;
	    break;
	case SQL_C_USHORT:
	case SQL_C_SHORT:
	case SQL_C_SSHORT:
	    *((short *) val) = 0;
	    break;
	case SQL_C_ULONG:
	case SQL_C_LONG:
	case SQL_C_SLONG:
	    *((long *) val) = 0;
	    break;
	case SQL_C_FLOAT:
	    *((float *) val) = 0;
	    break;
	case SQL_C_DOUBLE:
	    *((double *) val) = 0;
	    break;
	case SQL_C_BINARY:
	case SQL_C_CHAR:
	    *((char *) val) = '\0';
	    break;
	case SQL_C_DATE:
	    memset((DATE_STRUCT *) val, 0, sizeof (DATE_STRUCT));
	    break;
	case SQL_C_TIME:
	    memset((TIME_STRUCT *) val, 0, sizeof (TIME_STRUCT));
	    break;
	case SQL_C_TIMESTAMP:
	    memset((TIMESTAMP_STRUCT *) val, 0, sizeof (TIMESTAMP_STRUCT));
	    break;
	default:
	    return SQL_ERROR;
	}
    } else {
	char *endp = NULL;

	switch (type) {
	case SQL_C_UTINYINT:
	case SQL_C_TINYINT:
	case SQL_C_STINYINT:
	    *((char *) val) = strtol(*data, &endp, 0);
	    if (endp && endp == *data) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (char);
	    }
	    break;
	case SQL_C_USHORT:
	case SQL_C_SHORT:
	case SQL_C_SSHORT:
	    *((short *) val) = strtol(*data, &endp, 0);
	    if (endp && endp == *data) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (short);
	    }
	    break;
	case SQL_C_ULONG:
	case SQL_C_LONG:
	case SQL_C_SLONG:
	    *((int *) val) = strtol(*data, &endp, 0);
	    if (endp && endp == *data) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (int);
	    }
	    break;
	case SQL_C_FLOAT:
	    *((float *) val) = strtod(*data, &endp);
	    if (endp && endp == *data) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (float);
	    }
	    break;
	case SQL_C_DOUBLE:
	    *((double *) val) = strtod(*data, &endp);
	    if (endp && endp == *data) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (double);
	    }
	    break;
	case SQL_C_BINARY:
	case SQL_C_CHAR: {
	    int dlen = strlen(*data);
	    int offs = 0;

	    if (partial && len && s->bindcols) {
		if (dlen && s->bindcols[col].offs >= dlen) {
		    s->bindcols[col].offs = 0;
		    return SQL_NO_DATA;
		}
		offs = s->bindcols[col].offs;
		dlen -= offs;
	    }
	    if (val && !valnull) {
		strncpy(val, *data + offs, len);
	    }
	    if (valnull || len < 1) {
		*lenp = dlen;
	    } else {
		*lenp = min(len - 1, dlen);
		if (*lenp == len - 1 && *lenp != dlen) {
		    *lenp = SQL_NO_TOTAL;
		}
	    }
	    if (len) {
		((char *) val)[len - 1] = '\0';
	    }
	    if (partial && len && s->bindcols) {
		if (*lenp == SQL_NO_TOTAL) {
		    s->bindcols[col].offs += len - 1;
		    strcpy(s->logmsg, "data right truncated");
		    strcpy(s->sqlstate, "01004");
		    return SQL_SUCCESS_WITH_INFO;
		}
		s->bindcols[col].offs += *lenp;
	    }
	    break;
	}
	case SQL_C_DATE:
	    if (str2date(*data, (DATE_STRUCT *) val) < 0) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (DATE_STRUCT);
	    }
	    break;
	case SQL_C_TIME:
	    if (str2time(*data, (TIME_STRUCT *) val) < 0) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (TIME_STRUCT);
	    }
	    break;
	case SQL_C_TIMESTAMP:
	    if (str2timestamp(*data, (TIMESTAMP_STRUCT *) val) < 0) {
		*lenp = SQL_NULL_DATA;
	    } else {
		*lenp = sizeof (TIMESTAMP_STRUCT);
	    }
	    break;
	default:
	    return SQL_ERROR;
	}
    }
    return SQL_SUCCESS;
}

/**
 * Bind C variable to column of result set.
 * @param stmt statement handle
 * @param col column number, starting at 1
 * @param type output type
 * @param val output buffer
 * @param max length of output buffer
 * @param lenp output length pointer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLBindCol(SQLHSTMT stmt, SQLUSMALLINT col, SQLSMALLINT type,
	   SQLPOINTER val, SQLINTEGER max, SQLINTEGER *lenp)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!s->bindcols ||
	col < 1 || col > s->ncols) {
	sprintf(s->logmsg, "invalid column %d of %d", col, s->ncols);
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    --col;
    if (type == SQL_C_DEFAULT) {
	type = s->cols[col].type;
    } else {
	switch(type) {
	case SQL_C_LONG:
	case SQL_C_ULONG:
	case SQL_C_SLONG:
	case SQL_C_TINYINT:
	case SQL_C_UTINYINT:
	case SQL_C_STINYINT:
	case SQL_C_SHORT:
	case SQL_C_USHORT:
	case SQL_C_SSHORT:
	case SQL_C_FLOAT:
	case SQL_C_DOUBLE:
	case SQL_C_TIMESTAMP:
	case SQL_C_TIME:
	case SQL_C_DATE:
	case SQL_C_CHAR:
	    break;
	default:
	    sprintf(s->logmsg, "invalid type %d", type);
	    strcpy(s->sqlstate, "HY003");
	    return SQL_ERROR;
	}
    }
    if (max < 0) {
	strcpy(s->logmsg, "invalid length");
	strcpy(s->sqlstate, "HY090");
	return SQL_ERROR;
    }
    s->bindcols[col].type = type;
    s->bindcols[col].max = max;
    s->bindcols[col].lenp = lenp;
    s->bindcols[col].valp = val;
    s->bindcols[col].offs = 0;
    if (lenp) {
	*lenp = 0;
    }
    return SQL_SUCCESS; 
}

/**
 * Columns for result set of SQLTables().
 */

static COL tableSpec[] = {
    { "SYSTEM", "COLUMN", "TABLE_QUALIFIER", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "TABLE_OWNER", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "TABLE_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "COLUMN", "TABLE_TYPE", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "REMARKS", SQL_VARCHAR, 50 }
};

/**
 * Retrieve information on tables and/or views.
 * @param stmt statement handle
 * @param cat catalog name/pattern or NULL
 * @param catLen length of catalog name/pattern or SQL_NTS
 * @param schema schema name/pattern or NULL
 * @param schemaLen length of schema name/pattern or SQL_NTS
 * @param table table name/pattern or NULL
 * @param tableLen length of table name/pattern or SQL_NTS
 * @param type types of tables string or NULL
 * @param typeLen length of types of tables string or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLTables(SQLHSTMT stmt,
	  SQLCHAR *cat, SQLSMALLINT catLen,
	  SQLCHAR *schema, SQLSMALLINT schemaLen,
	  SQLCHAR *table, SQLSMALLINT tableLen,
	  SQLCHAR *type, SQLSMALLINT typeLen)
{
    STMT *s;
    DBC *d;
    char *errp;
    int ncols;
    char tname[512];
    char *where = "(type = 'table' or type = 'view')";

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
    freeresult(s, 1);
    s->ncols = array_size(tableSpec);
    s->cols = tableSpec;
    s->nrows = 0;
    mkbindcols(s);
    if (type && (typeLen > 0 || typeLen == SQL_NTS) && type[0] == '%') {
	int size = 3 * array_size(tableSpec);

	s->rows = xmalloc(size * sizeof (char *));
	if (!s->rows) {
	    s->nrows = 0;
	    return nomem(s);
	}
	memset(s->rows, 0, sizeof (char *) * size);
	s->ncols = array_size(tableSpec);
	s->rows[s->ncols + 0] = "";
	s->rows[s->ncols + 1] = "";
	s->rows[s->ncols + 2] = "";
	s->rows[s->ncols + 3] = "TABLE";
	s->rows[s->ncols + 5] = "";
	s->rows[s->ncols + 6] = "";
	s->rows[s->ncols + 7] = "";
	s->rows[s->ncols + 8] = "VIEW";
#ifdef MEMORY_DEBUG
	s->rowfree = xfree;
#else
	s->rowfree = free;
#endif
	s->nrows = 1;
	s->rowp = -1;
	return SQL_SUCCESS;
    }
    if (cat && (catLen > 0 || catLen == SQL_NTS) && cat[0] == '%') {
	int size = 2 * array_size(tableSpec);

	s->rows = xmalloc(size * sizeof (char *));
	if (!s->rows) {
	    s->nrows = 0;
	    return nomem(s);
	}
	memset(s->rows, 0, sizeof (char *) * size);
	s->ncols = array_size(tableSpec);
	s->rows[s->ncols + 0] = "";
	s->rows[s->ncols + 1] = "";
	s->rows[s->ncols + 2] = d->dbname;
	s->rows[s->ncols + 3] = "CATALOG";
#ifdef MEMORY_DEBUG
	s->rowfree = xfree;
#else
	s->rowfree = free;
#endif
	s->nrows = 1;
	s->rowp = -1;
	return SQL_SUCCESS;
    }
    if (schema && (schemaLen > 0 || schemaLen == SQL_NTS) && schema[0] == '%') {
	if ((!cat || catLen == 0 || !cat[0]) &&
	    (!table || tableLen == 0 || !table[0])) {
	    int size = 2 * array_size(tableSpec);

	    s->rows = xmalloc(size * sizeof (char *));
	    if (!s->rows) {
		s->nrows = 0;
		return nomem(s);
	    }
	    memset(s->rows, 0, sizeof (char *) * size);
	    s->ncols = array_size(tableSpec);
	    s->rows[s->ncols + 1] = "";
#ifdef MEMORY_DEBUG
	    s->rowfree = xfree;
#else
	    s->rowfree = free;
#endif
	    s->nrows = 1;
	    s->rowp = -1;
	    return SQL_SUCCESS;
	}
    }
    if (type) {
	char tmp[256], *t;
	int with_view = 0, with_table = 0;

	if (typeLen == SQL_NTS) {
	    strncpy(tmp, type, sizeof (tmp));
	    tmp[sizeof (tmp) - 1] = '\0';
	} else {
	    int len = min(sizeof (tmp) - 1, typeLen);

	    strncpy(tmp, type, len);
	    tmp[len] = '\0';
	}
	t = tmp;
	while (*t) {
	    *t = tolower((unsigned char) *t);
	    t++;
	}
	t = tmp;
	while (t) {
	    if (t[0] == '\'') {
		++t;
	    }
	    if (strncmp(t, "table", 5) == 0) {
		with_table++;
	    } else if (strncmp(t, "view", 4) == 0) {
		with_view++;
	    }
	    t = strchr(t, ',');
	    if (t) {
		++t;
	    }
	}
	if (with_view && with_table) {
	    /* where is already preset */
	} else if (with_view && !with_table) {
	    where = "type = 'view'";
	} else if (!with_view && with_table) {
	    where = "type = 'table'";
	} else {
	    s->rowp = -1;
	    return SQL_SUCCESS;
	}
    }
    strcpy(tname, "%");
    if (table && (tableLen > 0 || tableLen == SQL_NTS) && table[0] != '%') {
	int size;

	if (tableLen == SQL_NTS) {
	    size = sizeof (tname) - 1;
	} else {
	    size = min(sizeof (tname) - 1, tableLen);
	}
	strncpy(tname, table, size);
	tname[size] = '\0';
    }
    if (sqlite_get_table_printf(d->sqlite,
				"select '' as 'TABLE_QUALIFIER', "
				"'' as 'TABLE_OWNER', "
				"tbl_name as 'TABLE_NAME', "
				"upper(type) as 'TABLE_TYPE', "
				"NULL as 'REMARKS' "
				"from sqlite_master where %s"
				"and tbl_name like '%q'",
				&s->rows, &s->nrows, &ncols, &errp,
				where, tname)
	== SQLITE_OK) {
	if (ncols != s->ncols) {
	    sqlite_free_table(s->rows);
	    s->rows = NULL;
	    s->nrows = 0;
	    s->rowfree = NULL;
	} else {
	    s->rowfree = sqlite_free_table;
	}
    } else {
	s->nrows = 0;
	s->rows = NULL;
	s->rowfree = NULL;
    }
    freep(&errp);
    s->rowp = -1;
    return SQL_SUCCESS;
}

/**
 * Columns for result set of SQLColumns().
 */

static COL colSpec[] = {
    { "SYSTEM", "COLUMN", "TABLE_QUALIFIER", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "TABLE_OWNER", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "TABLE_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "COLUMN", "COLUMN_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "COLUMN", "DATA_TYPE", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "TYPE_NAME", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "PRECISION", SQL_INTEGER, 50 },
    { "SYSTEM", "COLUMN", "LENGTH", SQL_INTEGER, 50 },
    { "SYSTEM", "COLUMN", "RADIX", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "SCALE", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "NULLABLE", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "REMARKS", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "COLUMN_DEF", SQL_VARCHAR, 50 },
    { "SYSTEM", "COLUMN", "SQL_DATA_TYPE", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "SQL_DATETIME_SUB", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "CHAR_OCTET_LENGTH", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "ORDINAL_POSITION", SQL_SMALLINT, 50 },
    { "SYSTEM", "COLUMN", "IS_NULLABLE", SQL_VARCHAR, 50 }
};

/**
 * Retrieve column information on table.
 * @param stmt statement handle
 * @param cat catalog name/pattern or NULL
 * @param catLen length of catalog name/pattern or SQL_NTS
 * @param schema schema name/pattern or NULL
 * @param schemaLen length of schema name/pattern or SQL_NTS
 * @param table table name/pattern or NULL
 * @param tableLen length of table name/pattern or SQL_NTS
 * @param col column name/pattern or NULL
 * @param colLen length of column name/pattern or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLColumns(SQLHSTMT stmt,
	   SQLCHAR *cat, SQLSMALLINT catLen,
	   SQLCHAR *schema, SQLSMALLINT schemaLen,
	   SQLCHAR *table, SQLSMALLINT tableLen,
	   SQLCHAR *col, SQLSMALLINT colLen)
{
    STMT *s;
    DBC *d;
    int ret, nrows, ncols, size, i, k;
    char *errp, tname[512];
    char **rowp;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
#ifdef ASYNC
    async_end_if(s);
#endif
    if (!table || table[0] == '\0' || table[0] == '%') {
	strcpy(s->logmsg, "need table name");
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    if (tableLen == SQL_NTS) {
	size = sizeof (tname) - 1;
    } else {
	size = min(sizeof (tname) - 1, tableLen);
    }
    strncpy(tname, table, size);
    tname[size] = '\0';
    freeresult(s, 1);
    s->ncols = array_size(colSpec);
    s->cols = colSpec;
    mkbindcols(s);
    s->nrows = 0;
    s->rowp = -1;
    ret = sqlite_get_table_printf(d->sqlite, "PRAGMA table_info('%q')", &rowp,
				  &nrows, &ncols, &errp, tname);
    if (ret != SQLITE_OK) {
	if (errp) {
	    strncpy(s->logmsg, errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    sprintf(s->logmsg, "unknown error %d", ret);
	}
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;	
    }
    freep(&errp);
    if (ncols * nrows <= 0) {
	sqlite_free_table(rowp);
	return SQL_NO_DATA;
    }
    size = array_size(colSpec) * (nrows + 1);
    s->rows = xmalloc((size + 1) * sizeof (char *));
    if (!s->rows) {
	return nomem(s);
    }
    s->rows[0] = (char *) size;
    s->rows += 1;
    memset(s->rows, 0, sizeof (char *) * size);
    s->rowfree = freerows;
    s->nrows = nrows;
    for (i = 1; i <= s->nrows; i++) {
	s->rows[array_size(colSpec) * i + 0] = xstrdup("");
	s->rows[array_size(colSpec) * i + 1] = xstrdup("");
	s->rows[array_size(colSpec) * i + 2] = xstrdup(tname);
	s->rows[array_size(colSpec) * i + 8] = xstrdup("10");
	s->rows[array_size(colSpec) * i + 9] = xstrdup("0");
	s->rows[array_size(colSpec) * i + 15] = xstrdup("16384");
    }
    for (k = 0; k < ncols; k++) {
	if (strcmp(rowp[k], "cid") == 0) {
	    for (i = 1; i <= s->nrows; i++) {
		char buf[256];
		int coln = i;

		sscanf(rowp[i * ncols + k], "%d", &coln);
		sprintf(buf, "%d", coln + 1);
		s->rows[array_size(colSpec) * i + 16] = xstrdup(buf);
	    }
	} else if (strcmp(rowp[k], "name") == 0) {
	    for (i = 1; i <= s->nrows; i++) {
		s->rows[array_size(colSpec) * i + 3] =
		    xstrdup(rowp[i * ncols + k]);
	    }
	} else if (strcmp(rowp[k], "notnull") == 0) {
	    for (i = 1; i <= s->nrows; i++) {
		s->rows[array_size(colSpec) * i + 10] =
		    xstrdup(*rowp[i * ncols + k] != '0' ? "0" : "1");
		s->rows[array_size(colSpec) * i + 17] =
		    xstrdup(*rowp[i * ncols + k] != '0' ? "NO" : "YES");
	    }
	} else if (strcmp(rowp[k], "dflt_value") == 0) {
	    for (i = 1; i <= s->nrows; i++) {
		char *dflt = rowp[i * ncols + k];

		s->rows[array_size(colSpec) * i + 12] =
		    xstrdup(dflt ? dflt : "NULL");
	    }
	} else if (strcmp(rowp[k], "type") == 0) {
	    for (i = 1; i <= s->nrows; i++) {
		char *typename = rowp[i * ncols + k];
		int sqltype, m, d;
		char buf[256];

		s->rows[array_size(colSpec) * i + 5] = xstrdup(typename);
		sqltype = mapsqltype(typename, NULL);
		getmd(typename, sqltype, &m, &d);
#ifdef SQL_LONGVARCHAR
		if (sqltype == SQL_VARCHAR && m > 255) {
		    sqltype = SQL_LONGVARCHAR;
		}
#endif
		sprintf(buf, "%d", sqltype);
		s->rows[array_size(colSpec) * i + 4] = xstrdup(buf);
		s->rows[array_size(colSpec) * i + 13] = xstrdup(buf);
		sprintf(buf, "%d", m);
		s->rows[array_size(colSpec) * i + 7] = xstrdup(buf);
		sprintf(buf, "%d", d);
		s->rows[array_size(colSpec) * i + 6] = xstrdup(buf);
	    }
	}
    }
    sqlite_free_table(rowp);
    return SQL_SUCCESS;
}

/**
 * Columns for result set of SQLGetTypeInfo().
 */

static COL typeSpec[] = {
    { "SYSTEM", "TYPE", "TYPE_NAME", SQL_VARCHAR, 50 },
    { "SYSTEM", "TYPE", "DATA_TYPE", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "PRECISION", SQL_INTEGER, 4 },
    { "SYSTEM", "TYPE", "LITERAL_PREFIX", SQL_VARCHAR, 50 },
    { "SYSTEM", "TYPE", "LITERAL_SUFFIX", SQL_VARCHAR, 50 },
    { "SYSTEM", "TYPE", "CREATE_PARAMS", SQL_VARCHAR, 50 },
    { "SYSTEM", "TYPE", "NULLABLE", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "CASE_SENSITIVE", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "SEARCHABLE", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "UNSIGNED_ATTRIBUTE", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "MONEY", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "AUTO_INCREMENT", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "LOCAL_TYPE_NAME", SQL_VARCHAR, 50 },
    { "SYSTEM", "TYPE", "MINIMUM_SCALE", SQL_SMALLINT, 2 },
    { "SYSTEM", "TYPE", "MAXIMUM_SCALE", SQL_SMALLINT, 2 }
};

/**
 * Internal function to build up data type information as row in result set.
 * @param s statement pointer
 * @param row row number
 * @param typename name of type
 * @param type integer SQL type
 * @param sqltype string SQL type
 */

static void
mktypeinfo(STMT *s, int row, char *typename, int type, char *sqltype)
{
    int offs = row * array_size(typeSpec);

    s->rows[offs + 0] = typename;
    s->rows[offs + 1] = sqltype;
    switch (type) {
    default:
#ifdef SQL_LONGVARCHAR
	type = SQL_LONGVARCHAR;
    case SQL_LONGVARCHAR:
	s->rows[offs + 2] = "65536";
#else
	type = SQL_VARCHAR;
#endif
    case SQL_VARCHAR:
	s->rows[offs + 2] = "255";
	break;
    case SQL_TINYINT:
	s->rows[offs + 2] = "3";
	break;
    case SQL_SMALLINT:
	s->rows[offs + 2] = "5";
	break;
    case SQL_INTEGER:
	s->rows[offs + 2] = "7";
	break;
    case SQL_FLOAT:
	s->rows[offs + 2] = "7";
	break;
    case SQL_DOUBLE:
	s->rows[offs + 2] = "15";
	break;
    case SQL_DATE:
	s->rows[offs + 2] = "10";
	break;
    case SQL_TIME:
	s->rows[offs + 2] = "8";
	break;
    case SQL_TIMESTAMP:
	s->rows[offs + 2] = "32";
	break;
    }
    s->rows[offs + 3] = s->rows[offs + 4] = type == SQL_VARCHAR ? "'" : "";
#ifdef SQL_LONGVARCHAR
    if (type == SQL_LONGVARCHAR) {
	s->rows[offs + 3] = s->rows[offs + 4] = "'";
    }
#endif
    s->rows[offs + 5] = "";
    s->rows[offs + 6] = "1";
    s->rows[offs + 7] = "1";
    s->rows[offs + 8] = "1";
    s->rows[offs + 9] = "0";
    s->rows[offs + 10] = "0";
    s->rows[offs + 11] = "0";
    s->rows[offs + 12] = typename;
    s->rows[offs + 13] = "0";
    s->rows[offs + 14] = "0";
}

/**
 * Return data type information.
 * @param stmt statement handle
 * @param sqltype which type to retrieve
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetTypeInfo(SQLHSTMT stmt, SQLSMALLINT sqltype)
{
    STMT *s;
    DBC *d;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
    freeresult(s, 1);
    s->ncols = array_size(typeSpec);
    s->cols = typeSpec;
    mkbindcols(s);
    s->rowp = -1;
#ifdef SQL_LONGVARCHAR
    s->nrows = sqltype == SQL_ALL_TYPES ? 10 : 1;
#else
    s->nrows = sqltype == SQL_ALL_TYPES ? 9 : 1;
#endif
    s->rows = (char **) xmalloc(sizeof (char *) * (s->nrows + 1)
			       * array_size(colSpec));
    if (!s->rows) {
	s->nrows = 0;
	return nomem(s);
    }
#ifdef MEMORY_DEBUG
    s->rowfree = xfree;
#else
    s->rowfree = free;
#endif
    memset(s->rows, 0, sizeof (char *) * (s->nrows + 1) * array_size(colSpec));
    if (sqltype == SQL_ALL_TYPES) {
	mktypeinfo(s, 1, "varchar", SQL_VARCHAR, stringify(SQL_VARCHAR));
	mktypeinfo(s, 2, "tinyint", SQL_TINYINT, stringify(SQL_TINYINT));
	mktypeinfo(s, 3, "smallint", SQL_SMALLINT, stringify(SQL_SMALLINT));
	mktypeinfo(s, 4, "integer", SQL_INTEGER, stringify(SQL_INTEGER));
	mktypeinfo(s, 5, "float", SQL_FLOAT, stringify(SQL_FLOAT));
	mktypeinfo(s, 6, "double", SQL_DOUBLE, stringify(SQL_DOUBLE));
	mktypeinfo(s, 7, "date", SQL_DATE, stringify(SQL_DATE));
	mktypeinfo(s, 8, "time", SQL_TIME, stringify(SQL_TIME));
	mktypeinfo(s, 9, "timestamp", SQL_TIMESTAMP, stringify(SQL_TIMESTAMP));
#ifdef SQL_LONGVARCHAR
	mktypeinfo(s, 10, "longvarchar", SQL_LONGVARCHAR,
		   stringify(SQL_LONGVARCHAR));
#endif
    } else {
	switch (sqltype) {
	case SQL_VARCHAR:
	    mktypeinfo(s, 1, "varchar", SQL_VARCHAR, stringify(SQL_VARCHAR));
	    break;
	case SQL_TINYINT:
	    mktypeinfo(s, 1, "tinyint", SQL_TINYINT, stringify(SQL_TINYINT));
	    break;
	case SQL_SMALLINT:
	    mktypeinfo(s, 1, "smallint", SQL_SMALLINT,
		       stringify(SQL_SMALLINT));
	    break;
	case SQL_INTEGER:
	    mktypeinfo(s, 1, "integer", SQL_INTEGER, stringify(SQL_INTEGER));
	    break;
	case SQL_FLOAT:
	    mktypeinfo(s, 1, "float", SQL_FLOAT, stringify(SQL_FLOAT));
	    break;
	case SQL_DOUBLE:
	    mktypeinfo(s, 1, "double", SQL_DOUBLE, stringify(SQL_DOUBLE));
	    break;
	case SQL_DATE:
	    mktypeinfo(s, 1, "date", SQL_DATE, stringify(SQL_DATE));
	    break;
	case SQL_TIME:
	    mktypeinfo(s, 1, "time", SQL_TIME, stringify(SQL_TIME));
	    break;
	case SQL_TIMESTAMP:
	    mktypeinfo(s, 1, "timestamp", SQL_TIMESTAMP,
		       stringify(SQL_TIMESTAMP));
	    break;
#ifdef SQL_LONGVARCHAR
	case SQL_LONGVARCHAR:
	    mktypeinfo(s, 1, "longvarchar", SQL_LONGVARCHAR,
		       stringify(SQL_LONGVARCHAR));
#endif
	    break;
	default:
	    s->nrows = 0;
	    return SQL_NO_DATA;
	}
    }
    return SQL_SUCCESS;
}

/**
 * Columns for result set of SQLStatistics().
 */

static COL statSpec[] = {
    { "SYSTEM", "STATISTICS", "TABLE_QUALIFIER", SQL_VARCHAR, 50 },
    { "SYSTEM", "STATISTICS", "TABLE_OWNER", SQL_VARCHAR, 50 },
    { "SYSTEM", "STATISTICS", "TABLE_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "STATISTICS", "NON_UNIQUE", SQL_SMALLINT, 50 },
    { "SYSTEM", "STATISTICS", "INDEX_QUALIFIER", SQL_VARCHAR, 255 },
    { "SYSTEM", "STATISTICS", "INDEX_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "STATISTICS", "TYPE", SQL_SMALLINT, 50 },
    { "SYSTEM", "STATISTICS", "ORDINAL_POSITION", SQL_SMALLINT, 50 },
    { "SYSTEM", "STATISTICS", "COLUMN_NAME", SQL_VARCHAR, 255 },
    { "SYSTEM", "STATISTICS", "ASC_OR_DESC", SQL_CHAR, 1 },
    { "SYSTEM", "STATISTICS", "CARDINALITY", SQL_INTEGER, 50 },
    { "SYSTEM", "STATISTICS", "PAGES", SQL_INTEGER, 50 },
    { "SYSTEM", "STATISTICS", "FILTER_CONDITION", SQL_VARCHAR, 255 }
};

/**
 * Return statistic information on table indices.
 * @param stmt statement handle
 * @param cat catalog name/pattern or NULL
 * @param catLen length of catalog name/pattern or SQL_NTS
 * @param schema schema name/pattern or NULL
 * @param schemaLen length of schema name/pattern or SQL_NTS
 * @param table table name/pattern or NULL
 * @param tableLen length of table name/pattern or SQL_NTS
 * @param itype type of index information
 * @param resv reserved
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLStatistics(SQLHSTMT stmt, SQLCHAR *cat, SQLSMALLINT catLen,
	      SQLCHAR *schema, SQLSMALLINT schemaLen,
	      SQLCHAR *table, SQLSMALLINT tableLen,
	      SQLUSMALLINT itype, SQLUSMALLINT resv)
{
    STMT *s;
    DBC *d;
    int i, size, ret, nrows, ncols, offs;
    int namec = -1, uniquec = -1;
    char **rowp, *errp, tname[512];

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
#ifdef ASYNC
    async_end_if(s);
#endif
    if (!table || table[0] == '\0' || table[0] == '%') {
	strcpy(s->logmsg, "need table name");
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    if (tableLen == SQL_NTS) {
	size = sizeof (tname) - 1;
    } else {
	size = min(sizeof (tname) - 1, tableLen);
    }
    strncpy(tname, table, size);
    tname[size] = '\0';
    freeresult(s, 1);
    s->ncols = array_size(statSpec);
    s->cols = statSpec;
    s->nrows = 0;
    mkbindcols(s);
    s->rowp = -1;
    ret = sqlite_get_table_printf(d->sqlite,
				  "PRAGMA index_list('%q')", &rowp,
				  &nrows, &ncols, &errp, tname);
    if (ret != SQLITE_OK) {
	if (errp) {
	    strncpy(s->logmsg, errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    sprintf(s->logmsg, "unknown error %d", ret);
	}
	strcpy(s->sqlstate, "S1000");
	return SQL_ERROR;
    }
    freep(&errp);
    if (ncols * nrows <= 0) {
nodata:
	sqlite_free_table(rowp);
	return SQL_SUCCESS;
    }
    size = 0;
    for (i = 0; i < ncols; i++) {
	if (strcmp(rowp[i], "name") == 0) {
	    namec = i;
	} else if (strcmp(rowp[i], "unique") == 0) {
	    uniquec = i;
	}
    }
    if (namec < 0 || uniquec < 0) {
	goto nodata;
    }
    for (i = 1; i <= nrows; i++) {
	int nnrows, nncols;
	char **rowpp;
	int isuniq;

	isuniq = *rowp[i * ncols + uniquec] != 0 ||
	    (*rowp[i * ncols + namec] == '(' &&
	     strstr(rowp[i * ncols + namec], " autoindex "));
	if ((isuniq || itype == SQL_INDEX_ALL) &&
	    sqlite_get_table_printf(d->sqlite,
				    "PRAGMA index_info('%q')", &rowpp,
				    &nnrows, &nncols, NULL,
				    rowp[i * ncols + namec]) == SQLITE_OK) {
	    size += nnrows;
	    sqlite_free_table(rowpp);
	}
    }
    if (size == 0) {
	goto nodata;
    }
    s->nrows = size;
    size = (size + 1) * array_size(statSpec);
    s->rows = xmalloc((size + 1) * sizeof (char *));
    if (!s->rows) {
	s->nrows = 0;
	return nomem(s);
    }
    s->rows[0] = (char *) size;
    s->rows += 1;
    memset(s->rows, 0, sizeof (char *) * size);
    s->rowfree = freerows;
    offs = 0;
    for (i = 1; i <= nrows; i++) {
	int nnrows, nncols;
	char **rowpp;

	if ((*rowp[i * ncols + uniquec] != '0' ||
	     itype == SQL_INDEX_ALL) &&
	    sqlite_get_table_printf(d->sqlite,
				    "PRAGMA index_info('%q')", &rowpp,
				    &nnrows, &nncols, NULL,
				    rowp[i * ncols + namec]) == SQLITE_OK) {
	    int k;

	    for (k = 0; nnrows && k < nncols; k++) {
		if (strcmp(rowpp[k], "name") == 0) {
		    int m;

		    for (m = 1; m <= nnrows; m++) {
			int roffs = (offs + m) * s->ncols;
			int isuniq;

			isuniq = *rowp[i * ncols + uniquec] != 0 ||
			    (*rowp[i * ncols + namec] == '(' &&
			     strstr(rowp[i * ncols + namec], " autoindex "));
			s->rows[roffs + 0] = xstrdup("");
			s->rows[roffs + 1] = xstrdup("");
			s->rows[roffs + 2] = xstrdup(tname);
			s->rows[roffs + 3] = xstrdup(isuniq ? "0" : "1");
			s->rows[roffs + 4] = xstrdup(rowp[i * ncols + namec]);
			s->rows[roffs + 5] = xstrdup(rowp[i * ncols + namec]);
			s->rows[roffs + 6] =
			    xstrdup(stringify(SQL_INDEX_OTHER));
			s->rows[roffs + 8] = xstrdup(rowpp[m * nncols + k]);
			s->rows[roffs + 9] = xstrdup("A");
		    }
		} else if (strcmp(rowpp[k], "seqno") == 0) {
		    int m;

		    for (m = 1; m <= nnrows; m++) {
			int roffs = (offs + m) * s->ncols;
			int pos = m - 1;
			char buf[32];

			sscanf(rowpp[m * nncols + k], "%d", &pos);
			sprintf(buf, "%d", pos + 1);
			s->rows[roffs + 7] = xstrdup(buf);
		    }
		}
	    }
	    offs += nnrows;
	    sqlite_free_table(rowpp);
	}
    }
    sqlite_free_table(rowp);
    return SQL_SUCCESS;
}

/**
 * Retrieve row data after fetch.
 * @param stmt statement handle
 * @param col column number, starting at 1
 * @param type output type
 * @param val output buffer
 * @param len length of output buffer
 * @param lenp output length
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLGetData(SQLHSTMT stmt, SQLUSMALLINT col, SQLSMALLINT type,
	   SQLPOINTER val, SQLINTEGER len, SQLINTEGER *lenp)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (col < 1 || col > s->ncols) {
	strcpy(s->logmsg, "invalid column");
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    --col;
    return getrowdata(s, col, type, val, len, lenp, 1);
}

/**
 * Fetch next result row.
 * @param stmt statement handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLFetch(SQLHSTMT stmt)
{
    STMT *s;
    int i;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!s->bindcols) {
	return SQL_ERROR;
    }
#ifdef ASYNC
    if (*s->async_run && ((DBC *) (s->dbc))->async_stmt == s) {
	SQLRETURN ret = waitfordata(s);

	if (ret != SQL_SUCCESS) {
	    return ret;
	}
	if (s->nrows < 1) {
	    return SQL_NO_DATA;
	}
	s->rowp = 0;
	goto dofetch;
    }
#endif
    if (!s->rows || s->nrows < 1) {
	return SQL_NO_DATA;
    }
    ++s->rowp;
    if (s->rowp < 0 || s->rowp >= s->nrows) {
	return SQL_NO_DATA;
    }
dofetch:
    for (i = 0; s->bindcols && i < s->ncols; i++) {
	BINDCOL *b = &s->bindcols[i];

	b->offs = 0;
	if ((b->valp || b->lenp) &&
	    getrowdata(s, (SQLUSMALLINT) i, b->type, b->valp,
		       b->max, b->lenp, 0) != SQL_SUCCESS) {
	    return SQL_ERROR;
	}
    }
    return SQL_SUCCESS;
}

/**
 * Internal fetch function for SQLFetchScroll() and SQLExtendedFetch().
 * @param stmt statement handle
 * @param orient fetch direction
 * @param offset offset for fetch direction
 * @result ODBC error code
 */

static SQLRETURN
drvfetchscroll(SQLHSTMT stmt, SQLSMALLINT orient, SQLINTEGER offset)
{
    STMT *s;
    int i;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!s->bindcols) {
	return SQL_ERROR;
    }
#ifdef ASYNC
    if (s->curtype == SQL_CURSOR_FORWARD_ONLY && orient != SQL_FETCH_NEXT) {
	strcpy(s->logmsg, "wrong fetch direction");
	strcpy(s->sqlstate, "01000");
	return SQL_ERROR;
    }
    if (*s->async_run && ((DBC *) (s->dbc))->async_stmt == s) {
	SQLRETURN ret = waitfordata(s);

	if (ret != SQL_SUCCESS) {
	    return ret;
	}
	if (s->nrows < 1) {
	    return SQL_NO_DATA;
	}
	s->rowp = 0;
	goto dofetch;
    }
#endif
    if (!s->rows) {
	return SQL_NO_DATA;
    }
    switch (orient) {
    case SQL_FETCH_NEXT:
	if (s->nrows < 1 || ++s->rowp >= s->nrows) {
	    return SQL_NO_DATA;
	}
	break;
    case SQL_FETCH_PRIOR:
	if (s->nrows < 1 || --s->rowp < 0) {
	    s->rowp = -1;
	    return SQL_NO_DATA;
	}
	break;
    case SQL_FETCH_FIRST:
	if (s->nrows < 1) {
	    return SQL_NO_DATA;
	}
	s->rowp = 0;
	break;
    case SQL_FETCH_LAST:
	if (s->nrows < 1) {
	    return SQL_NO_DATA;
	}
	s->rowp = s->nrows - 1;
	break;
    case SQL_FETCH_ABSOLUTE:
	if (offset < 0 || offset >= s->nrows) {
	    return SQL_NO_DATA;
	}
	s->rowp = offset;
	break;
    default:
	return SQL_ERROR;
    }	
dofetch:
    for (i = 0; s->bindcols && i < s->ncols; i++) {
	BINDCOL *b = &s->bindcols[i];

	b->offs = 0;
	if ((b->valp || b->lenp) &&
	    getrowdata(s, (SQLUSMALLINT) i, b->type, b->valp,
		       b->max, b->lenp, 0) != SQL_SUCCESS) {
	    return SQL_ERROR;
	}
    }
    return SQL_SUCCESS;
}

/**
 * Fetch result row with scrolling.
 * @param stmt statement handle
 * @param orient fetch direction
 * @param offset offset for fetch direction
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLFetchScroll(SQLHSTMT stmt, SQLSMALLINT orient, SQLINTEGER offset)
{
    return drvfetchscroll(stmt, orient, offset);
}

/**
 * Fetch result row with scrolling and row status.
 * @param stmt statement handle
 * @param orient fetch direction
 * @param offset offset for fetch direction
 * @param rowcount output number of fetched rows
 * @param rowstatus array for row stati
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLExtendedFetch(SQLHSTMT stmt, SQLUSMALLINT orient, SQLINTEGER offset,
		 SQLUINTEGER *rowcount, SQLUSMALLINT *rowstatus)
{
    SQLRETURN ret;

    ret = drvfetchscroll(stmt, orient, offset);
    switch (ret) {
#ifdef SQL_ROW_SUCCESS_WITH_INFO
    case SQL_SUCCESS_WITH_INFO:
	if (rowstatus) {
	    *rowstatus = SQL_ROW_SUCCESS_WITH_INFO;
	}
	goto rowc;
#else
    case SQL_SUCCESS_WITH_INFO:
#endif
    case SQL_SUCCESS:
	if (rowstatus) {
	    *rowstatus = SQL_ROW_SUCCESS;
	}
#ifdef SQL_ROW_SUCCESS_WITH_INFO
    rowc:
#endif
	if (rowcount) {
	    *rowcount = 1;
	}
	break;
    default:
	if (rowstatus) {
	    *rowstatus = SQL_ROW_ERROR;
	}
	if (rowcount) {
	    *rowcount = 0;
	}
    }
    return ret;
}

/**
 * Return number of affected rows of HSTMT.
 * @param stmt statement handle
 * @param nrows output number of rows
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLRowCount(SQLHSTMT stmt, SQLINTEGER *nrows)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (nrows) {
	*nrows = s->nrows;
    }
    return SQL_SUCCESS;
}

/**
 * Return number of columns of result set given HSTMT.
 * @param stmt statement handle
 * @param ncols output number of columns
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLNumResultCols(SQLHSTMT stmt, SQLSMALLINT *ncols)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (ncols) {
	*ncols = s->ncols;
    }
    return SQL_SUCCESS;
}

/**
 * Describe column information.
 * @param stmt statement handle
 * @param col column number, starting at 1
 * @param name buffer for column name
 * @param nameLen output length of column name
 * @param type output SQL type
 * @param size output column size
 * @param digits output number of digits
 * @param nullable output NULL allowed indicator
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLDescribeCol(SQLHSTMT stmt, SQLUSMALLINT col, SQLCHAR *name,
	       SQLSMALLINT nameMax, SQLSMALLINT *nameLen,
	       SQLSMALLINT *type, SQLUINTEGER *size,
	       SQLSMALLINT *digits, SQLSMALLINT *nullable)
{
    STMT *s;
    COL *c;
    int didname = 0;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!s->cols) {
	strcpy(s->logmsg, "no columns");
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    if (col < 1 || col > s->ncols) {
	strcpy(s->logmsg, "invalid column");
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    c = s->cols + col - 1;
    if (name && nameMax > 0) {
	strncpy(name, c->column, nameMax);
	name[nameMax - 1] = '\0';
	didname = 1;
    }
    if (nameLen) {
	if (didname) {
	    *nameLen = strlen(name);
	} else {
	    *nameLen = strlen(c->column);
	}
    }
    if (type) {
	*type = c->type;
    }
    if (size) {
	*size = c->size;
    }
    if (digits) {
	*digits = 0;
    }
    if (nullable) {
	*nullable = 1;
    }
    return SQL_SUCCESS;
}

/**
 * Retrieve column attributes.
 * @param stmt statement handle
 * @param col column number, starting at 1
 * @param id attribute id
 * @param val output buffer
 * @param valMax length of output buffer
 * @param valLen output length
 * @param val2 integer output buffer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLColAttributes(SQLHSTMT stmt, SQLUSMALLINT col, SQLUSMALLINT id,
		 SQLPOINTER val, SQLSMALLINT valMax, SQLSMALLINT *valLen,
		 SQLINTEGER *val2)
{
    STMT *s;
    COL *c;
    SQLSMALLINT dummy;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!s->cols) {
	return SQL_ERROR;
    }
    if (!valLen) {
	valLen = &dummy;
    }
    if (id == SQL_COLUMN_COUNT) {
	if (val2) {
	    *val2 = s->ncols;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    }
    if (id == SQL_COLUMN_TYPE && col == 0) {
	if (val2) {
	    *val2 = SQL_INTEGER;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    }
#ifdef SQL_DESC_OCTET_LENGTH
    if (id == SQL_DESC_OCTET_LENGTH && col == 0) {
	if (val2) {
	    *val2 = 4;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    }
#endif
    if (col < 1 || col > s->ncols) {
	strcpy(s->logmsg, "invalid column");
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    c = s->cols + col - 1;

    switch (id) {
    case SQL_COLUMN_LABEL:
    case SQL_COLUMN_NAME:
    case SQL_DESC_NAME:
	if (val) {
	    strncpy(val, c->column, valMax);
	}
	*valLen = min(strlen(c->column), valMax);
	return SQL_SUCCESS;
    case SQL_COLUMN_TYPE:
    case SQL_DESC_TYPE:
	if (val2) {
	    *val2 = c->type;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_DISPLAY_SIZE:
	if (val2) {
	    *val2 = c->size;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_UNSIGNED:
	if (val2) {
	    *val2 = c->nosign ? SQL_TRUE : SQL_FALSE;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_SCALE:
	if (val2) {
	    *val2 = c->scale;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_PRECISION:
	if (val2) {
	    *val2 = c->prec;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_MONEY:
    case SQL_COLUMN_AUTO_INCREMENT:
	if (val2) {
	    *val2 = SQL_FALSE;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_LENGTH:
    case SQL_DESC_LENGTH:
	if (val2) {
	    *val2 = c->size;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_NULLABLE:
	if (val2) {
	    *val2 = SQL_NULLABLE;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_SEARCHABLE:
	if (val2) {
	    *val2 = SQL_SEARCHABLE;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_CASE_SENSITIVE:
	if (val2) {
	    *val2 = SQL_TRUE;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_UPDATABLE:
	if (val2) {
	    *val2 = SQL_TRUE;
	}
	*valLen = sizeof (int);
	return SQL_SUCCESS;
    case SQL_COLUMN_TYPE_NAME: {
	char *tn = c->typename ? c->typename : "varchar";

	if (val) {
	    strncpy(val, tn, valMax);
	}
	*valLen = min(strlen(tn), valMax);
	return SQL_SUCCESS;
    }
    case SQL_COLUMN_OWNER_NAME:
    case SQL_COLUMN_QUALIFIER_NAME: {
	char *z = "";

	if (val) {
	    strncpy(val, z, valMax);
	}
	*valLen = min(strlen(z), valMax);
	return SQL_SUCCESS;
    }
    case SQL_COLUMN_TABLE_NAME:
#if (SQL_COLUMN_TABLE_NAME != SQL_DESC_TABLE_NAME)
    case SQL_DESC_TABLE_NAME:
#endif
	if (val) {
	    strncpy(val, c->table, valMax);
	}
	*valLen = min(strlen(c->table), valMax);
	return SQL_SUCCESS;
    }
    sprintf(s->logmsg, "unsupported column attributes %d", id);
    strcpy(s->sqlstate, "HY091");
    return SQL_ERROR;
}

/**
 * Retrieve column attributes.
 * @param stmt statement handle
 * @param col column number, starting at 1
 * @param id attribute id
 * @param val output buffer
 * @param valMax length of output buffer
 * @param valLen output length
 * @param val2 integer output buffer
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLColAttribute(SQLHSTMT stmt, SQLUSMALLINT col, SQLUSMALLINT id,
		SQLPOINTER val, SQLSMALLINT valMax, SQLSMALLINT *valLen,
		SQLPOINTER val2)
{
    STMT *s;
    COL *c;
    int v = 0;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (!s->cols) {
	return SQL_ERROR;
    }
    if (col < 1 || col > s->ncols) {
	strcpy(s->logmsg, "invalid column");
	strcpy(s->sqlstate, "S1002");
	return SQL_ERROR;
    }
    c = s->cols + col - 1;
    switch (id) {
    case SQL_DESC_CATALOG_NAME:
	strncpy(val, c->db, valMax);
	if (valLen) {
	    *valLen = strlen(val);
	}
	break;
    case SQL_COLUMN_LENGTH:
    case SQL_DESC_LENGTH:
	v = c->size;
	break;
    case SQL_COLUMN_LABEL:
    case SQL_DESC_NAME:
	strncpy(val, c->column, valMax);
	if (valLen) {
	    *valLen = strlen(val);
	}
	break;
    case SQL_DESC_OCTET_LENGTH:
	v = c->size;
	break;
    case SQL_DESC_TABLE_NAME:
	strncpy(val, c->table, valMax);
	if (valLen) {
	    *valLen = strlen(val);
	}
	break;
    case SQL_DESC_TYPE:
	v = c->type;
	break;
    case SQL_DESC_CONCISE_TYPE:
	switch (c->type) {
	case SQL_INTEGER:
	    v = SQL_C_LONG;
	    break;
	case SQL_TINYINT:
	    v = SQL_C_TINYINT;
	    break;
	case SQL_SMALLINT:
	    v = SQL_C_SHORT;
	    break;
	case SQL_FLOAT:
	    v = SQL_C_FLOAT;
	    break;
	case SQL_DOUBLE:
	    v = SQL_C_DOUBLE;
	    break;
	case SQL_TIMESTAMP:
	    v = SQL_C_TIMESTAMP;
	    break;
	case SQL_TIME:
	    v = SQL_C_TIME;
	    break;
	case SQL_DATE:
	    v = SQL_C_DATE;
	    break;
	default:
	    v = SQL_C_CHAR;
	    break;
	}
	break;
    case SQL_DESC_UPDATABLE:
	v = 1;
	break;
    case SQL_COLUMN_DISPLAY_SIZE:
	v = c->size;
	break;
    default:
	sprintf(s->logmsg, "unsupported column attribute %d", id);
	strcpy(s->sqlstate, "HY091");
	return SQL_ERROR;
    }
    if (val2) {
	*(int *) val2 = v;
    }
    return SQL_SUCCESS;
}

/**
 * Return last HDBC or HSTMT error message.
 * @param env environment handle or NULL
 * @param dbc database connection handle or NULL
 * @param stmt statement handle or NULL
 * @param sqlState output buffer for SQL state
 * @param nativeErr output buffer for native error code
 * @param errmsg output buffer for error message
 * @param errmax length of output buffer for error message
 * @param errlen output length of error message
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLError(SQLHENV env, SQLHDBC dbc, SQLHSTMT stmt,
	 SQLCHAR *sqlState, SQLINTEGER *nativeErr,
	 SQLCHAR *errmsg, SQLSMALLINT errmax, SQLSMALLINT *errlen)
{
    SQLCHAR dummy0[6];
    SQLINTEGER dummy1;
    SQLSMALLINT dummy2;

    if (env == SQL_NULL_HENV &&
	dbc == SQL_NULL_HDBC &&
	stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    if (!sqlState) {
	sqlState = dummy0;
    }
    if (!nativeErr) {
	nativeErr = &dummy1;
    }
    if (!errlen) {
	errlen = &dummy2;
    }
    if (!errmsg) {
	errmsg = dummy0;
	errmax = 0;
    }
    if (stmt) {
	STMT *s = (STMT *) stmt;

	if (s->logmsg[0] == '\0') {
	    goto noerr;
	}
	*nativeErr = -1;
	strcpy(sqlState, s->sqlstate);
	if (errmax == SQL_NTS) {
	    strcpy(errmsg, "[SQLite]");
	    strcat(errmsg, s->logmsg);
	    *errlen = strlen(errmsg);
	} else {
	    strncpy(errmsg, "[SQLite]", errmax);
	    if (errmax - 8 > 0) {
		strncpy(errmsg + 8, s->logmsg, errmax - 8);
	    }
	    *errlen = min(strlen(s->logmsg) + 8, errmax);
	}
	s->logmsg[0] = '\0';
	return SQL_SUCCESS;
    }
    if (dbc) {
	DBC *d = (DBC *) dbc;

	if (d->magic != DBC_MAGIC || d->logmsg[0] == '\0') {
	    goto noerr;
	}
	*nativeErr = -1;
	strcpy(sqlState, d->sqlstate);
	if (errmax == SQL_NTS) {
	    strcpy(errmsg, "[SQLite]");
	    strcat(errmsg, d->logmsg);
	    *errlen = strlen(errmsg);
	} else {
	    strncpy(errmsg, "[SQLite]", errmax);
	    if (errmax - 8 > 0) {
		strncpy(errmsg + 8, d->logmsg, errmax - 8);
	    }
	    *errlen = min(strlen(d->logmsg) + 8, errmax);
	}
	d->logmsg[0] = '\0';
	return SQL_SUCCESS;
    }
noerr:
    sqlState[0] = '\0';
    *nativeErr = 0;
    *errlen = 0;
    return SQL_NO_DATA_FOUND;
}

/**
 * Return information for more result sets.
 * @param stmt statement handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLMoreResults(SQLHSTMT stmt)
{
    STMT *s;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    return (s->rowp < 0 && s->nrows > 0) ? SQL_SUCCESS : SQL_NO_DATA;
}

/**
 * SQLite callback during drvprepare(), used to collect column information.
 * @param arg user data, actually a statement pointer
 * @param ncols number of columns
 * @param values value string array
 * @param cols column name string array
 * @result always 1 to abort sqlite_exec()
 */

static int
selcb(void *arg, int ncols, char **values, char **cols)
{
    STMT *s = (STMT *) arg;

    if (ncols > 0) {
	int i, size;
	char *p;
	COL *dyncols;

	for (i = size = 0; i < ncols; i++) {
	    size += 2 + 2 * strlen(cols[i]);
	}
	dyncols = xmalloc(ncols * sizeof (COL) + size);
	if (!dyncols) {
	    freedyncols(s);
	    s->ncols = 0;
	    return 1;
	}
	p = (char *) (dyncols + ncols);
	for (i = 0; i < ncols; i++) {
	    char *q;

	    dyncols[i].db = ((DBC *) (s->dbc))->dbname;
	    q = strchr(cols[i], '.');
	    if (q) {
		dyncols[i].table = p;
		strncpy(p, cols[i], q - cols[i]);
		p[q - cols[i]] = '\0';
		p += strlen(p) + 1;
		strcpy(p, q + 1);
		dyncols[i].column = p;
		p += strlen(p) + 1;
	    } else {
		dyncols[i].table = "";
		strcpy(p, cols[i]);
		dyncols[i].column = p;
		p += strlen(p) + 1;
	    }
#ifdef SQL_LONGVARCHAR
	    dyncols[i].type = SQL_LONGVARCHAR;
	    dyncols[i].size = 65536;
#else
	    dyncols[i].type = SQL_VARCHAR;
	    dyncols[i].size = 256;
#endif
	    dyncols[i].index = i;
	    dyncols[i].scale = 0;
	    dyncols[i].prec = 0;
	    dyncols[i].nosign = 1;
	    dyncols[i].typename = NULL;
	}
	freedyncols(s);
	s->dyncols = s->cols = dyncols;
	s->dcols = ncols;
    }
    s->ncols = ncols;
    return 1;
}

/**
 * Internal query preparation used by SQLPrepare() and SQLExecDirect().
 * @param stmt statement handle
 * @param query query string
 * @param queryLen length of query string or SQL_NTS
 * @result ODBC error code
 */

static SQLRETURN
drvprepare(SQLHSTMT stmt, SQLCHAR *query, SQLINTEGER queryLen)
{
    STMT *s;
    DBC *d;
    char *errp;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
#ifdef ASYNC
    async_end(s);
#endif
    freep(&s->query);
    s->query = fixupsql(query, queryLen, &s->nparams, &s->isselect, &errp);
    if (!s->query) {
	if (errp) {
	    strcpy(s->logmsg, errp);
	    strcpy(s->sqlstate, "S1000");
	    return SQL_ERROR;
	}
	return nomem(s);
    }
    freeresult(s, 1);
    if (s->isselect) {
	int ret;
	char **params = NULL;

	if (s->nparams) {
	    int i;

	    params = xmalloc(s->nparams * sizeof (char *));
	    if (!params) {
		return nomem(s);
	    }
	    for (i = 0; i < s->nparams; i++) {
		params[i] = "NULL";
	    }
	}
	errp = NULL;
	ret = sqlite_exec_vprintf(d->sqlite, s->query, selcb, s,
				  &errp, (char *) params);
	if (ret != SQLITE_ABORT && ret != SQLITE_OK) {
	    freep(&params);
	    if (errp) {
		strncpy(s->logmsg, errp, sizeof (s->logmsg));
		s->logmsg[sizeof (s->logmsg) - 1] = '\0';
		freep(&errp);
	    } else {
		sprintf(s->logmsg, "unknown error %d", ret);
	    }
	    strcpy(s->sqlstate, "S1000");
	    return SQL_ERROR;
	}
	freep(&params);
	freep(&errp);
	fixupdyncols(s, d->sqlite);
    }
    mkbindcols(s);
    return SQL_SUCCESS;
}

/**
 * Internal query execution used by SQLExecute() and SQLExecDirect().
 * @param stmt statement handle
 * @result ODBC error code
 */

static SQLRETURN
drvexecute(SQLHSTMT stmt)
{
    STMT *s;
    DBC *d;
    char *errp = NULL, **params = NULL;
    int ret, i, size, ncols;

    if (stmt == SQL_NULL_HSTMT) {
	return SQL_INVALID_HANDLE;
    }
    s = (STMT *) stmt;
    if (s->dbc == SQL_NULL_HDBC) {
noconn:
	return noconn(s);
    }
    d = (DBC *) s->dbc;
    if (!d->sqlite) {
	goto noconn;
    }
    if (!s->query) {
	strcpy(s->logmsg, "no query prepared");
	strcpy(s->logmsg, "S1000");
	return SQL_ERROR;
    }
    if (s->nbindparms < s->nparams) {
	strcpy(s->logmsg, "unbound parameters in query");
	strcpy(s->logmsg, "S1000");
	return SQL_ERROR;
    }
#ifdef ASYNC
    async_end(s);
#endif
    for (i = size = 0; i < s->nparams; i++) {
	SQLRETURN ret;

	ret = substparam(s, i, NULL, &size);
	if (ret != SQL_SUCCESS) {
	    return ret;
	}
    }
    if (s->nparams) {
	char *p;

	params = xmalloc(s->nparams * sizeof (char *) + size);
	if (!params) {
	    return nomem(s);
	}
	p = (char *) (params + s->nparams);
	for (i = 0; i < s->nparams; i++) {
	    params[i] = p;
	    substparam(s, i, &p, NULL);
	}
    }
    freeresult(s, 0);
    if (!d->autocommit && !d->intrans) {
	ret = sqlite_exec(d->sqlite, "BEGIN TRANSACTION", NULL, NULL, &errp);
	if (ret != SQLITE_OK) {
	    freep(&params);
	    if (errp) {
		strncpy(s->logmsg, errp, sizeof (s->logmsg));
		s->logmsg[sizeof (s->logmsg) - 1] = '\0';
		freep(&errp);
	    } else {
		sprintf(s->logmsg, "unknown error %d", ret);
	    }
	    strcpy(s->sqlstate, "S1000");
	    return SQL_ERROR;
	}
	d->intrans = 1;
	freep(&errp);
    }
#ifdef ASYNC
    if (s->isselect && !d->intrans &&
	d->thread_enable &&
	s->curtype == SQL_CURSOR_FORWARD_ONLY) {
	ret = async_start(s, params);
	if (ret == SQL_SUCCESS) {
	    params = NULL;
	    goto done2;
	}
    }
#endif
    ret = sqlite_get_table_vprintf(d->sqlite, s->query, &s->rows,
				   &s->nrows, &ncols, &errp, (char *) params);
    if (ret != SQLITE_OK) {
	freep(&params);
	if (errp) {
	    strncpy(s->logmsg, errp, sizeof (s->logmsg));
	    s->logmsg[sizeof (s->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    sprintf(s->logmsg, "unknown error %d", ret);
	}
	strcpy(s->sqlstate, "S1000");
	if (d->intrans) {
	    sqlite_exec(d->sqlite, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
	    d->intrans = 0;
	}
	return SQL_ERROR;
    }
    freep(&params);
    freep(&errp);
    s->rowfree = sqlite_free_table;
    if (ncols == 1 && !s->isselect) {
	/*
	 * INSERT/UPDATE/DELETE results are immediately released,
	 * but the row count is retained for SQLRowCount().
	 */
	if (strcmp(s->rows[0], "rows inserted") == 0 ||
	    strcmp(s->rows[0], "rows updated") == 0 ||
	    strcmp(s->rows[0], "rows deleted") == 0) {
	    int nrows = 0;
	   
	    nrows = strtoul(s->rows[1], NULL, 0);
	    freeresult(s, 1);
	    s->nrows = nrows;
	    goto done;
	}
    }
    if (s->ncols != ncols) {
	int size;
	char *p;
	COL *dyncols;

	for (i = size = 0; i < ncols; i++) {
	    size += 2 + strlen(s->rows[i]);
	}
	if (size == 0) {
	    freeresult(s, 1);
	    goto done;
	}
	dyncols = xmalloc(ncols * sizeof (COL) + size);
	if (!dyncols) {
	    if (d->intrans) {
		sqlite_exec(d->sqlite, "ROLLBACK TRANSACTION",
			    NULL, NULL, NULL);
		d->intrans = 0;
	    }
	    return nomem(s);
	}
	p = (char *) (dyncols + ncols);
	for (i = 0; i < ncols; i++) {
	    char *q;

	    dyncols[i].db = d->dbname;
	    q = strchr(s->rows[i], '.');
	    if (q) {
		dyncols[i].table = p;
		strncpy(p, s->rows[i], q - s->rows[i]);
		p[q - s->rows[i]] = '\0';
		p += strlen(p) + 1;
		dyncols[i].column = q + 1;
	    } else {
		dyncols[i].table = "";
		dyncols[i].column = s->rows[i];
	    }
#ifdef SQL_LONGVARCHAR
	    dyncols[i].type = SQL_LONGVARCHAR;
	    dyncols[i].size = 65536;
#else
	    dyncols[i].type = SQL_VARCHAR;
	    dyncols[i].size = 255;
#endif
	    dyncols[i].index = i;
	    dyncols[i].scale = 0;
	    dyncols[i].prec = 0;
	    dyncols[i].nosign = 1;
	    dyncols[i].typename = NULL;
	}
	freedyncols(s);
	s->ncols = s->dcols = ncols;
	s->dyncols = s->cols = dyncols;
	fixupdyncols(s, d->sqlite);
    }
done:
    mkbindcols(s);
done2:
    s->rowp = -1;
    return SQL_SUCCESS;
}

/**
 * Prepare HSTMT.
 * @param stmt statement handle
 * @param query query string
 * @param queryLen length of query string or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLPrepare(SQLHSTMT stmt, SQLCHAR *query, SQLINTEGER queryLen)
{
    return drvprepare(stmt, query, queryLen);
}

/**
 * Execute query.
 * @param stmt statement handle
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLExecute(SQLHSTMT stmt)
{
    return drvexecute(stmt);
}

/**
 * Execute query directly.
 * @param stmt statement handle
 * @param query query string
 * @param queryLen length of query string or SQL_NTS
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLExecDirect(SQLHSTMT stmt, SQLCHAR *query, SQLINTEGER queryLen)
{
    if (drvprepare(stmt, query, queryLen) != SQL_SUCCESS) {
	return SQL_ERROR;
    }
    return drvexecute(stmt);
}

#ifdef _WIN32
#ifndef WITHOUT_DRIVERMGR

/*
 * Windows configuration dialog stuff.
 */

#include <windowsx.h>
#include <ctl3d.h>
#include <winuser.h>
#include "resource.h"

#define stricmp _stricmp

static HINSTANCE NEAR hModule;	/* Saved module handle for resources */
static int initialized = 0;

#define MAXPATHLEN      (255+1)           /* Max path length */
#define MAXKEYLEN       (15+1)            /* Max keyword length */
#define MAXDESC         (255+1)           /* Max description length */
#define MAXDSNAME       (32+1)            /* Max data source name length */
#define MAXTONAME       (32+1)            /* Max timeout length */
#define MAXDBNAME	(255+1)

/* Attribute key indexes into an array of Attr structs, see below */

#define KEY_DSN 		0
#define KEY_DESC		1
#define KEY_DBNAME		2
#define KEY_BUSY		3
#define KEY_DRIVER		4
#ifdef ASYNC
#define KEY_THR			5
#define NUMOFKEYS		6
#else
#define NUMOFKEYS		5
#endif

typedef struct {
    BOOL supplied;
    char attr[MAXPATHLEN];
} ATTR;

typedef struct {
    SQLHWND parent;
    LPCSTR  driver;
    ATTR    attr[NUMOFKEYS];
    char    DSN[MAXDSNAME];
    BOOL    newDSN;
    BOOL    defDSN;
} SETUPDLG;

static struct {
    char *key;
    int ikey;
} attrLookup[] = {
    { "DSN", KEY_DSN },
    { "DESC", KEY_DESC },
    { "Description", KEY_DESC},
    { "Database", KEY_DBNAME },
    { "Timeout", KEY_BUSY },
    { "Driver", KEY_DRIVER },
#ifdef ASYNC
    { "Threaded", KEY_THR },
#endif
    { NULL, 0 }
};

/**
 * Setup dialog data from datasource attributes.
 * @param attribs attribute string
 * @param setupdlg pointer to dialog data
 */

static void
ParseAttributes(LPCSTR attribs, SETUPDLG *setupdlg)
{
    char *str = (char *) attribs, *start;
    char	key[MAXKEYLEN];
    int elem, nkey;

    while (*str) {
	start = str;
	if ((str = strchr(str, '=')) == NULL) {
	    return;
	}
	elem = -1;
	nkey = str - start;
	if (nkey < sizeof (key)) {
	    int i;

	    memcpy(key, start, nkey);
	    key[nkey] = '\0';
	    for (i = 0; attrLookup[i].key; i++) {
		if (stricmp(attrLookup[i].key, key) == 0) {
		    elem = attrLookup[i].ikey;
		    break;
		}
	    }
	}
	start = ++str;
	while (*str && *str != ';') {
	    ++str;
	}
	if (elem >= 0) {
	    int end = min(str - start, sizeof (setupdlg->attr[elem].attr) - 1);

	    setupdlg->attr[elem].supplied = TRUE;
	    memcpy(setupdlg->attr[elem].attr, start, end);
	    setupdlg->attr[elem].attr[end] = '\0';
	}
	if (*str) {
	    ++str;
	}
    }
}

/**
 * Set datasource attributes in registry.
 * @param parent handle of parent window
 * @param setupdlg pointer to dialog data
 * @result true or false
 */

static BOOL
SetDSNAttributes(HWND parent, SETUPDLG *setupdlg)
{
    char *dsn = setupdlg->attr[KEY_DSN].attr;

    if (setupdlg->newDSN && strlen(dsn) == 0) {
	return FALSE;
    }
    if (!SQLWriteDSNToIni(dsn, setupdlg->driver)) {
	if (parent) {
	    char buf[MAXPATHLEN], msg[MAXPATHLEN];

	    LoadString(hModule, IDS_BADDSN, buf, sizeof (buf));
	    wsprintf(msg, buf, dsn);
	    LoadString(hModule, IDS_MSGTITLE, buf, sizeof (buf));
	    MessageBox(parent, msg, buf, MB_ICONEXCLAMATION | MB_OK);
	}
	return FALSE;
    }
    if (parent || setupdlg->attr[KEY_DESC].supplied) {
	SQLWritePrivateProfileString(dsn, "Description",
				     setupdlg->attr[KEY_DESC].attr,
				     ODBC_INI);
    }
    if (parent || setupdlg->attr[KEY_DBNAME].supplied) {
	SQLWritePrivateProfileString(dsn, "Database",
				     setupdlg->attr[KEY_DBNAME].attr,
				     ODBC_INI);
    }
    if (parent || setupdlg->attr[KEY_BUSY].supplied) {
	SQLWritePrivateProfileString(dsn, "Timeout",
				     setupdlg->attr[KEY_BUSY].attr,
				     ODBC_INI);
    }
#ifdef ASYNC
    if (parent || setupdlg->attr[KEY_THR].supplied) {
	SQLWritePrivateProfileString(dsn, "Threaded",
				     setupdlg->attr[KEY_THR].attr,
				     ODBC_INI);
    }
#endif
    if (setupdlg->attr[KEY_DSN].supplied &&
	stricmp(setupdlg->DSN, setupdlg->attr[KEY_DSN].attr)) {
	SQLRemoveDSNFromIni(setupdlg->DSN);
    }
    return TRUE;
}

/**
 * Get datasource attributes from registry.
 * @param setupdlg pointer to dialog data
 */

static void
GetAttributes(SETUPDLG *setupdlg)
{
    char *dsn = setupdlg->attr[KEY_DSN].attr;

    if (!setupdlg->attr[KEY_DESC].supplied) {
	SQLGetPrivateProfileString(dsn, "Description", "",
				   setupdlg->attr[KEY_DESC].attr,
				   sizeof (setupdlg->attr[KEY_DESC].attr),
				   ODBC_INI);
    }
    if (!setupdlg->attr[KEY_DBNAME].supplied) {
	SQLGetPrivateProfileString(dsn, "Database", "",
				   setupdlg->attr[KEY_DBNAME].attr,
				   sizeof (setupdlg->attr[KEY_DBNAME].attr),
				   ODBC_INI);
    }
    if (!setupdlg->attr[KEY_BUSY].supplied) {
	SQLGetPrivateProfileString(dsn, "Timeout", "1000",
				   setupdlg->attr[KEY_BUSY].attr,
				   sizeof (setupdlg->attr[KEY_BUSY].attr),
				   ODBC_INI);
    }
#ifdef ASYNC
    if (!setupdlg->attr[KEY_THR].supplied) {
	SQLGetPrivateProfileString(dsn, "Threaded", "1",
				   setupdlg->attr[KEY_THR].attr,
				   sizeof (setupdlg->attr[KEY_THR].attr),
				   ODBC_INI);
    }
#endif
}

/**
 * Open file dialog for selection of SQLite database file.
 * @param hdlg handle of originating dialog window
 */

static void
GetDBFile(HWND hdlg)
{
    SETUPDLG *setupdlg = (SETUPDLG *) GetWindowLong(hdlg, DWL_USER);
    OPENFILENAME ofn;

    memset(&ofn, 0, sizeof (ofn));
    ofn.lStructSize = sizeof (ofn);
    ofn.hwndOwner = hdlg;
    ofn.hInstance = (HINSTANCE) GetWindowLong(hdlg, GWL_HINSTANCE);
    ofn.lpstrFile = (LPTSTR) setupdlg->attr[KEY_DBNAME].attr;
    ofn.nMaxFile = MAXPATHLEN;
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST |
		OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    if (GetOpenFileName(&ofn)) {
	SetDlgItemText(hdlg, IDC_DBNAME, setupdlg->attr[KEY_DBNAME].attr);
	setupdlg->attr[KEY_DBNAME].supplied = TRUE;
    }
}

/**
 * Dialog procedure for ConfigDSN().
 * @param hdlg handle of dialog window
 * @param wmsg type of message
 * @param wparam wparam of message
 * @param lparam lparam of message
 * @result true or false
 */

static BOOL CALLBACK
ConfigDlgProc(HWND hdlg, WORD wmsg, WPARAM wparam, LPARAM lparam)
{
    SETUPDLG *setupdlg = NULL;

    switch (wmsg) {
    case WM_INITDIALOG:
	SetWindowLong(hdlg, DWL_USER, lparam);
	setupdlg = (SETUPDLG *) lparam;
	GetAttributes(setupdlg);
	SetDlgItemText(hdlg, IDC_DSNAME, setupdlg->attr[KEY_DSN].attr);
	SetDlgItemText(hdlg, IDC_DESC, setupdlg->attr[KEY_DESC].attr);
	SetDlgItemText(hdlg, IDC_DBNAME, setupdlg->attr[KEY_DBNAME].attr);
	SetDlgItemText(hdlg, IDC_TONAME, setupdlg->attr[KEY_BUSY].attr);
#ifdef ASYNC
	CheckDlgButton(hdlg, IDC_THREADED,
		       getbool(setupdlg->attr[KEY_THR].attr) ?
		       BST_CHECKED : BST_UNCHECKED);
#endif
	if (setupdlg->defDSN) {
	    EnableWindow(GetDlgItem(hdlg, IDC_DSNAME), FALSE);
	    EnableWindow(GetDlgItem(hdlg, IDC_DSNAMETEXT), FALSE);
	} else {
	    SendDlgItemMessage(hdlg, IDC_DSNAME,
			       EM_LIMITTEXT, (WPARAM) (MAXDSNAME - 1), 0L);
	    SendDlgItemMessage(hdlg, IDC_DESC,
			       EM_LIMITTEXT, (WPARAM) (MAXDESC - 1), 0L);
	    SendDlgItemMessage(hdlg, IDC_DBNAME,
			       EM_LIMITTEXT, (WPARAM) (MAXDBNAME - 1), 0L);
	    SendDlgItemMessage(hdlg, IDC_TONAME,
			       EM_LIMITTEXT, (WPARAM) (MAXTONAME - 1), 0L);
	}
	return TRUE;
    case WM_COMMAND:
	switch (GET_WM_COMMAND_ID(wparam, lparam)) {
	case IDC_DSNAME:
	    if (GET_WM_COMMAND_CMD(wparam, lparam) == EN_CHANGE) {
		char item[MAXDSNAME];

		EnableWindow(GetDlgItem(hdlg, IDOK),
			     GetDlgItemText(hdlg, IDC_DSNAME,
					    item, sizeof (item)));
		return TRUE;
	    }
	    break;
	case IDC_BROWSE:
	    GetDBFile(hdlg);
	    break;
	case IDOK:
	    setupdlg = (SETUPDLG *) GetWindowLong(hdlg, DWL_USER);
	    if (!setupdlg->defDSN) {
		GetDlgItemText(hdlg, IDC_DSNAME,
			       setupdlg->attr[KEY_DSN].attr,
			       sizeof (setupdlg->attr[KEY_DSN].attr));
	    }
	    GetDlgItemText(hdlg, IDC_DESC,
			   setupdlg->attr[KEY_DESC].attr,
			   sizeof (setupdlg->attr[KEY_DESC].attr));
	    GetDlgItemText(hdlg, IDC_DBNAME,
			   setupdlg->attr[KEY_DBNAME].attr,
			   sizeof (setupdlg->attr[KEY_DBNAME].attr));
	    GetDlgItemText(hdlg, IDC_TONAME,
			   setupdlg->attr[KEY_BUSY].attr,
			   sizeof (setupdlg->attr[KEY_BUSY].attr));
#ifdef ASYNC
	    strcpy(setupdlg->attr[KEY_THR].attr,
		   IsDlgButtonChecked(hdlg, IDC_THREADED) == BST_CHECKED ?
		   "1" : "0");
#endif
	    SetDSNAttributes(hdlg, setupdlg);
	    /* FALL THROUGH */
	case IDCANCEL:
	    EndDialog(hdlg, wparam);
	    return TRUE;
	}
	break;
    }
    return FALSE;
}

/**
 * ODBC INSTAPI procedure for DSN configuration.
 * @param hwnd parent window handle
 * @param request type of request
 * @param driver driver name
 * @param attribs attribute string of DSN
 * @result true or false
 */

BOOL INSTAPI
ConfigDSN(HWND hwnd, WORD request, LPCSTR driver, LPCSTR attribs)
{
    BOOL success;
    SETUPDLG *setupdlg;

    setupdlg = (SETUPDLG *) xmalloc(sizeof (SETUPDLG));
    if (setupdlg == NULL) {
	return FALSE;
    }
    memset(setupdlg, 0, sizeof (SETUPDLG));
    if (attribs) {
	ParseAttributes(attribs, setupdlg);
    }
    if (setupdlg->attr[KEY_DSN].supplied) {
	strcpy(setupdlg->DSN, setupdlg->attr[KEY_DSN].attr);
    } else {
	setupdlg->DSN[0] = '\0';
    }
    if (request == ODBC_REMOVE_DSN) {
	if (!setupdlg->attr[KEY_DSN].supplied) {
	    success = FALSE;
	} else {
	    success = SQLRemoveDSNFromIni(setupdlg->attr[KEY_DSN].attr);
	}
    } else {
	setupdlg->parent = hwnd;
	setupdlg->driver = driver;
	setupdlg->newDSN = request == ODBC_ADD_DSN;
	setupdlg->defDSN = stricmp(setupdlg->attr[KEY_DSN].attr,
				   "Default") == 0;
	if (hwnd) {
	    success = DialogBoxParam(hModule, MAKEINTRESOURCE(CONFIGDSN),
				     hwnd, (DLGPROC) ConfigDlgProc,
				     (LONG) setupdlg) == IDOK;
	} else if (setupdlg->attr[KEY_DSN].supplied) {
	    success = SetDSNAttributes(hwnd, setupdlg);
	} else {
	    success = FALSE;
	}
    }
    xfree(setupdlg);
    return success;
}

/**
 * Dialog procedure for SQLDriverConnect().
 * @param hdlg handle of dialog window
 * @param wmsg type of message
 * @param wparam wparam of message
 * @param lparam lparam of message
 * @result true or false
 */

static BOOL CALLBACK
DriverConnectProc(HWND hdlg, WORD wmsg, WPARAM wparam, LPARAM lparam)
{
    SETUPDLG *setupdlg;

    switch (wmsg) {
    case WM_INITDIALOG:
	SetWindowLong(hdlg, DWL_USER, lparam);
	setupdlg = (SETUPDLG *) lparam;
	SetDlgItemText(hdlg, IDC_DSNAME, setupdlg->attr[KEY_DSN].attr);
	SetDlgItemText(hdlg, IDC_DESC, setupdlg->attr[KEY_DESC].attr);
	SetDlgItemText(hdlg, IDC_DBNAME, setupdlg->attr[KEY_DBNAME].attr);
	SetDlgItemText(hdlg, IDC_TONAME, setupdlg->attr[KEY_BUSY].attr);
	SendDlgItemMessage(hdlg, IDC_DSNAME,
			   EM_LIMITTEXT, (WPARAM) (MAXDSNAME - 1), 0L);
	SendDlgItemMessage(hdlg, IDC_DESC,
			   EM_LIMITTEXT, (WPARAM) (MAXDESC - 1), 0L);
	SendDlgItemMessage(hdlg, IDC_DBNAME,
			   EM_LIMITTEXT, (WORD)(MAXDBNAME - 1), 0L);
	SendDlgItemMessage(hdlg, IDC_TONAME,
			   EM_LIMITTEXT, (WORD)(MAXTONAME - 1), 0L);
#ifdef ASYNC
	CheckDlgButton(hdlg, IDC_THREADED,
		       getbool(setupdlg->attr[KEY_THR].attr) ?
		       BST_CHECKED : BST_UNCHECKED);
#endif
	return TRUE;
    case WM_COMMAND:
	switch (GET_WM_COMMAND_ID(wparam, lparam)) {
	case IDC_BROWSE:
	    GetDBFile(hdlg);
	    break;
	case IDOK:
	    setupdlg = (SETUPDLG *) GetWindowLong(hdlg, DWL_USER);
	    GetDlgItemText(hdlg, IDC_DSNAME,
			   setupdlg->attr[KEY_DSN].attr,
			   sizeof (setupdlg->attr[KEY_DSN].attr));
	    GetDlgItemText(hdlg, IDC_DBNAME,
			   setupdlg->attr[KEY_DBNAME].attr,
			   sizeof (setupdlg->attr[KEY_DBNAME].attr));
	    GetDlgItemText(hdlg, IDC_TONAME,
			   setupdlg->attr[KEY_BUSY].attr,
			   sizeof (setupdlg->attr[KEY_BUSY].attr));
#ifdef ASYNC
	    strcpy(setupdlg->attr[KEY_THR].attr,
		   IsDlgButtonChecked(hdlg, IDC_THREADED) == BST_CHECKED ?
		   "1" : "0");
#endif
	    /* FALL THROUGH */
	case IDCANCEL:
	    EndDialog(hdlg, GET_WM_COMMAND_ID(wparam, lparam) == IDOK);
	    return TRUE;
	}
    }
    return FALSE;
}

/**
 * Connect using a driver connection string.
 * @param dbc database connection handle
 * @param hwnd parent window handle
 * @param connIn driver connect input string
 * @param connInLen length of driver connect input string or SQL_NTS
 * @param connOut driver connect output string
 * @param connOutMax length of driver connect output string
 * @param connOutLen output length of driver connect output string
 * @param drvcompl completion type
 * @result ODBC error code
 */

SQLRETURN SQL_API
SQLDriverConnect(SQLHDBC dbc, SQLHWND hwnd,
		 SQLCHAR *connIn, SQLSMALLINT connInLen,
		 SQLCHAR *connOut, SQLSMALLINT connOutMax,
		 SQLSMALLINT *connOutLen, SQLUSMALLINT drvcompl)
{
    BOOL maybeprompt, prompt = FALSE;
    DBC *d;
    SETUPDLG *setupdlg;
    short ret;
    int busyto = 1000, tmp;
    char *errp;

    if (dbc == SQL_NULL_HDBC) {
	return SQL_INVALID_HANDLE;
    }
    d = (DBC *) dbc;
    if (d->sqlite) {
	strcpy(d->logmsg, "connection already established");
	strcpy(d->sqlstate, "08002");
	return SQL_ERROR;
    }
    setupdlg = (SETUPDLG *) xmalloc(sizeof (SETUPDLG));
    if (setupdlg == NULL) {
	return SQL_ERROR;
    }
    memset(setupdlg, 0, sizeof (SETUPDLG));
    maybeprompt = drvcompl == SQL_DRIVER_COMPLETE ||
	drvcompl == SQL_DRIVER_COMPLETE_REQUIRED;
    if (connIn == NULL || !connInLen ||
	(connInLen == SQL_NTS && !connIn[0])) {
	prompt = TRUE;
    } else {
	ParseAttributes(connIn, setupdlg);
	if (!setupdlg->attr[KEY_DSN].attr[0] &&
	    drvcompl == SQL_DRIVER_COMPLETE_REQUIRED) {
	    strcpy(setupdlg->attr[KEY_DSN].attr, "DEFAULT");
	}
	GetAttributes(setupdlg);
	if (drvcompl == SQL_DRIVER_PROMPT ||
	    (maybeprompt &&
	     !setupdlg->attr[KEY_DSN].attr[0] ||
	     !setupdlg->attr[KEY_DBNAME].attr[0])) {
	    prompt = TRUE;
	}
    }
retry:
    if (prompt) {
	ret = DialogBoxParam(hModule, MAKEINTRESOURCE(DRIVERCONNECT),
			     hwnd, (DLGPROC) DriverConnectProc,
			     (LONG) setupdlg);
	if (!ret || ret == -1) {
	    xfree(setupdlg);
	    return SQL_NO_DATA_FOUND;
	}
    }
    if (connOut || connOutLen) {
	char buf[1024];
	int len;

	buf[0] = '\0';
	if (setupdlg->attr[KEY_DSN].attr[0]) {
	    strcat(buf, "DSN=");
	    strcat(buf, setupdlg->attr[KEY_DSN].attr);
	    strcat(buf, ";");
	}
	if (setupdlg->attr[KEY_DRIVER].attr[0]) {
	    strcat(buf, "Driver=");
	    strcat(buf, setupdlg->attr[KEY_DRIVER].attr);
	    strcat(buf, ";");
	}
	strcat(buf, "Database=");
	strcat(buf, setupdlg->attr[KEY_DBNAME].attr);
	strcat(buf, ";Timeout=");
	strcat(buf, setupdlg->attr[KEY_BUSY].attr);
#ifdef ASYNC
	strcat(buf, ";Threaded=");
	strcat(buf, setupdlg->attr[KEY_THR].attr);
#endif
	len = min(connOutMax - 1, strlen(buf));
	if (connOut) {
	    strncpy(connOut, buf, len);
	    connOut[len] = '\0';
	}
	if (connOutLen) {
	    *connOutLen = len;
	}
    }
    tmp = strtoul(setupdlg->attr[KEY_BUSY].attr, &errp, 0);
    if (errp && *errp == '\0' && errp != setupdlg->attr[KEY_BUSY].attr) {
	busyto = tmp;
    }
    errp = NULL;
    d->sqlite = sqlite_open(setupdlg->attr[KEY_DBNAME].attr, 0, &errp);
    if (d->sqlite == NULL) {
	if (maybeprompt && !prompt) {
	    prompt = TRUE;
	    freep(&errp);
	    goto retry;
	}
	if (errp) {
	    strncpy(d->logmsg, errp, sizeof (d->logmsg));
	    d->logmsg[sizeof (d->logmsg) - 1] = '\0';
	    freep(&errp);
	} else {
	    strcpy(d->logmsg, "connect failed");
	}
	strcpy(d->sqlstate, "S1000");
	xfree(setupdlg);
	return SQL_ERROR;
    }
    freep(&errp);
#ifdef ASYNC
    d->thread_enable = getbool(setupdlg->attr[KEY_THR].attr);
    if (d->thread_enable) {
	d->sqlite2 = sqlite_open(setupdlg->attr[KEY_DBNAME].attr, 0, NULL);
	if (d->sqlite2 == NULL) {
	    d->thread_enable = 0;
	}
    }
    d->curtype = d->thread_enable ? SQL_CURSOR_FORWARD_ONLY : SQL_CURSOR_STATIC;
#endif
    sqlite_busy_timeout(d->sqlite, busyto);
    setsqliteopts(d->sqlite);
#ifdef ASYNC
    if (d->sqlite2) {
	sqlite_busy_timeout(d->sqlite2, busyto);
	setsqliteopts(d->sqlite2);
    }
#endif
    freep(&d->dbname);
    d->dbname = xstrdup(setupdlg->attr[KEY_DBNAME].attr);
    freep(&d->dsn);
    d->dsn = xstrdup(setupdlg->attr[KEY_DSN].attr);
    xfree(setupdlg);
    return SQL_SUCCESS;
}

/**
 * DLL initializer for WIN32.
 * @param hinst instance handle
 * @param reason reason code for entry point
 * @param reserved
 * @result always true
 */

BOOL APIENTRY
LibMain(HANDLE hinst, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
	if (!initialized++) {
	    hModule = hinst;
	}
	break;
    case DLL_THREAD_ATTACH:
	break;
    case DLL_PROCESS_DETACH:
	--initialized;
	break;
    case DLL_THREAD_DETACH:
	break;
    default:
	break;
  }
  return TRUE;
}

/**
 * DLL entry point for WIN32.
 * @param hinst instance handle
 * @param reason reason code for entry point
 * @param reserved
 * @result always true
 */

int __stdcall
DllMain(HANDLE hinst, DWORD reason, LPVOID reserved)
{
    return LibMain(hinst, reason, reserved);
}

#endif /* WITHOUT_DRIVERMGR */
#endif /* _WIN32 */
