/*
 * 2007 January 27
 *
 * The author disclaims copyright to this source code.  In place of
 * a legal notice, here is a blessing:
 *
 *    May you do good and not evil.
 *    May you find forgiveness for yourself and forgive others.
 *    May you share freely, never taking more than you give.
 *
 ********************************************************************
 *
 * SQLite extension module for importing/exporting
 * database information from/to SQL source text.
 *
 * Usage:
 *
 *  SQLite function:
 *       SELECT import_sql(filename);
 *
 *  C function (STANDALONE):
 *       int impexp_import_sql(sqlite3 *db, char *filename);
 *
 *       Reads SQL commands from filename and executes them
 *       against the current database. Returns the number
 *       of changes to the current database.
 *
 *
 *  SQLite function:
 *       SELECT export_sql(filename, [mode, tablename, ...]);
 *
 *  C function (STANDALONE):
 *       int impexp_export_sql(sqlite3 *db, char *filename, int mode, ...);
 *
 *       Writes SQL to filename similar to SQLite's shell
 *       ".dump" meta command. Mode selects the output format:
 *       Mode 0 (default): dump schema and data using the
 *       optional table names following the mode argument.
 *       Mode 1: dump data only using the optional table
 *       names following the mode argument.
 *       Mode 2: dump schema and data using the optional
 *       table names following the mode argument; each
 *       table name is followed by a WHERE clause, i.e.
 *       "mode, table1, where1, table2, where2, ..."
 *       Mode 3: dump data only, same rules as in mode 2.
 *       Returns approximate number of lines written or
 *       -1 when an error occurred.
 *
 * On Win32 the filename argument may be specified as NULL in order
 * to open a system file dialog for interactive filename selection.
 */

#ifdef STANDALONE
#include <sqlite3.h>
#else
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

static const char space_chars[] = " \f\n\r\t\v";

#define ISSPACE(c) ((c) && strchr(space_chars, (c)) != 0)

static char *
one_input_line(FILE *fin)
{
    char *line, *tmp;
    int nline;
    int n;
    int eol;

    nline = 256;
    line = sqlite3_malloc(nline);
    if (!line) {
	return 0;
    }
    n = 0;
    eol = 0;
    while (!eol) {
	if (n + 256 > nline) {
	    nline = nline * 2 + 256;
	    tmp = sqlite3_realloc(line, nline);
	    if (!tmp) {
		sqlite3_free(line);
		return 0;
	    }
	    line = tmp;
	}
	if (!fgets(line + n, nline - n, fin)) {
	    if (n == 0) {
		sqlite3_free(line);
		return 0;
	    }
	    line[n] = 0;
	    eol = 1;
	    break;
	}
	while (line[n]) {
	    n++;
	}
	if (n > 0 && line[n-1] == '\n') {
	    n--;
	    line[n] = 0;
	    eol = 1;
	}
    }
    tmp = sqlite3_realloc(line, n + 1);
    if (!tmp) {
	sqlite3_free(line);
	return 0;
    }
    return tmp;
}

static int
ends_with_semicolon(const char *str, int n)
{
    while (n > 0 && ISSPACE(str[n - 1])) {
	n--;
    }
    return n > 0 && str[n - 1] == ';';
}

static int
all_whitespace(const char *str)
{
    for (; str[0]; str++) {
	if (ISSPACE(str[0])) {
	    continue;
	}
	if (str[0] == '/' && str[1] == '*') {
	    str += 2;
	    while (str[0] && (str[0] != '*' || str[1] != '/')) {
		str++;
	    }
	    if (!str[0]) {
		return 0;
	    }
	    str++;
	    continue;
	}
	if (str[0] == '-' && str[1] == '-') {
	    str += 2;
	    while (str[0] && str[0] != '\n') {
		str++;
	    }
	    if (!str[0]) {
		return 1;
	    }
	    continue;
	}
	return 0;
    }
    return 1;
}

static int
process_input(sqlite3 *db, FILE *fin)
{
    char *line = 0;
    char *sql = 0;
    int nsql = 0;
    int rc;
    int errors = 0;

    while (1) {
	line = one_input_line(fin);
	if (!line) {
	    break;
	}
	if ((!sql || !sql[0]) && all_whitespace(line)) {
	    continue;
	}
	if (!sql) {
	    int i;
	    for (i = 0; line[i] && ISSPACE(line[i]); i++) {
		/* empty loop body */
	    }
	    if (line[i]) {
		nsql = strlen(line);
		sql = sqlite3_malloc(nsql + 1);
		if (!sql) {
		    errors++;
		    break;
		}
		strcpy(sql, line);
	    }
	} else {
	    int len = strlen(line);
	    char *tmp;

	    tmp = sqlite3_realloc(sql, nsql + len + 2);
	    if (!tmp) {
		errors++;
		break;
	    }
	    sql = tmp;
	    strcpy(sql + nsql, "\n");
	    nsql++;
	    strcpy(sql + nsql, line);
	    nsql += len;
	}
	sqlite3_free(line);
	line = 0;
	if (sql && ends_with_semicolon(sql, nsql) && sqlite3_complete(sql)) {
	    rc = sqlite3_exec(db, sql, 0, 0, 0);
	    if (rc != SQLITE_OK) {
		errors++;
	    }
	    sqlite3_free(sql);
	    sql = 0;
	    nsql = 0;
	}
    }
    if (sql) {
	sqlite3_free(sql);
    }
    if (line) {
	sqlite3_free(line);
    }
    return errors;
}

static void
import_func(sqlite3_context *ctx, int nargs, sqlite3_value **args)
{
    sqlite3 *db = (sqlite3 *) sqlite3_user_data(ctx);
    int changes0 = sqlite3_changes(db);
    char *filename = 0;
    FILE *fin;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (nargs > 0) {
	if (sqlite3_value_type(args[0]) != SQLITE_NULL) {
	    filename = (char *) sqlite3_value_text(args[0]);
	}
    }
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST |
		    OFN_EXPLORER | OFN_PATHMUSTEXIST;
	if (GetOpenFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    fin = fopen(filename, "r");
    if (!fin) {
	goto done;
    }
    process_input(db, fin);
    fclose(fin);
done:
    sqlite3_result_int(ctx, sqlite3_changes(db) - changes0);
}

#ifdef STANDALONE
int
impexp_import_sql(sqlite3 *db, char *filename)
{
    int changes0;
    FILE *fin;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (!db) {
	return 0;
    }
    changes0 = sqlite3_changes(db);
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST |
		    OFN_EXPLORER | OFN_PATHMUSTEXIST;
	if (GetOpenFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    fin = fopen(filename, "r");
    if (!fin) {
	goto done;
    }
    process_input(db, fin);
    fclose(fin);
done:
    return sqlite3_changes(db) - changes0;
}
#endif

typedef struct {
    sqlite3 *db;
    int with_schema;
    char *where;
    int nlines;
    FILE *out;
} DUMP_DATA;

static int
table_dump(DUMP_DATA *dd, char **errp, int fmt, const char *query, ...)
{
    sqlite3_stmt *select;
    int rc;
    const char *rest, *q = query;
    va_list ap;

    if (errp && *errp) {
	sqlite3_free(*errp);
	*errp = 0;
    }
    if (fmt) {
	va_start(ap, query);
	q = sqlite3_vmprintf(query, ap);
	va_end(ap);
	if (!q) {
	    return SQLITE_NOMEM;
	}
    }
    rc = sqlite3_prepare(dd->db, q, -1, &select, &rest);
    if (fmt) {
	sqlite3_free((char *) q);
    }
    if (rc != SQLITE_OK || !select) {
	return rc;
    }
    rc = sqlite3_step(select);
    while (rc == SQLITE_ROW) {
	if (fprintf(dd->out, "%s;\n", sqlite3_column_text(select, 0)) > 0) {
	    dd->nlines++;
	}
	rc = sqlite3_step(select);
    }
    if (rc != SQLITE_OK) 
    rc = sqlite3_finalize(select);
    if (errp && *errp) {
	sqlite3_free(*errp);
	*errp = 0;
    }
    if (rc != SQLITE_OK) {
	if (errp) {
	    *errp = sqlite3_mprintf("%s", sqlite3_errmsg(dd->db));
	}
    }
    return rc;
}

static char *
append(char *in, char const *append, char quote)
{
    int len, i;
    int nappend = strlen(append);
    int nin = in ? strlen(in) : 0;
    char *tmp;

    len = nappend + nin + 1;
    if (quote) {
	len += 2;
	for (i = 0; i < nappend; i++) {
	    if (append[i] == quote) {
		len++;
	    }
	}
    }
    tmp = (char *) sqlite3_realloc(in, len);
    if (!tmp) {
	sqlite3_free(in);
	return 0;
    }
    in = tmp;
    if (quote) {
	char *p = in + nin;

	*p++ = quote;
	for (i = 0; i < nappend; i++) {
	    *p++ = append[i];
	    if (append[i] == quote) {
		*p++ = quote;
	    }
	}
	*p++ = quote;
	*p++ = '\0';
    } else {
	memcpy(in + nin, append, nappend);
	in[len - 1] = '\0';
    }
    return in;
}

static int
dump_cb(void *udata, int nargs, char **args, char **cols)
{
    int rc;
    const char *table, *type, *sql;
    DUMP_DATA *dd = (DUMP_DATA *) udata;

    if (nargs != 3) {
	return 1;
    }
    table = args[0];
    type = args[1];
    sql = args[2];
    if (strcmp(table, "sqlite_sequence") == 0) {
	if (dd->with_schema) {
	    if (fputs("DELETE FROM sqlite_sequence;\n", dd->out) >= 0) {
		dd->nlines++;
	    }
	}
    } else if (strcmp(table, "sqlite_stat1") == 0) {
	if (dd->with_schema) {
	    if (fputs("ANALYZE sqlite_master;\n", dd->out) >= 0) {
		dd->nlines++;
	    }
	}
    } else if (strncmp(table, "sqlite_", 7) == 0) {
	return 0;
    } else if (strncmp(sql, "CREATE VIRTUAL TABLE", 20) == 0) {
	if (dd->with_schema) {
	    sqlite3_stmt *stmt = 0;
	    char *creat = 0, *table_info = 0;
   
	    table_info = append(table_info, "PRAGMA table_info(", 0);
	    table_info = append(table_info, table, '"');
	    table_info = append(table_info, ");", 0);
	    rc = sqlite3_prepare(dd->db, table_info, -1, &stmt, 0);
	    if (table_info) {
		sqlite3_free(table_info);
		table_info = 0;
	    }
	    if (rc != SQLITE_OK || !stmt) {
bailout0:
		if (creat) {
		    sqlite3_free(creat);
		}
		return 1;
	    }
	    creat = append(creat, table, '"');
	    creat = append(creat, "(", 0);
	    rc = sqlite3_step(stmt);
	    while (rc == SQLITE_ROW) {
		const char *p;

		p = (const char *) sqlite3_column_text(stmt, 1);
		creat = append(creat, p, '"');
		creat = append(creat, " ", 0);
		p = (const char *) sqlite3_column_text(stmt, 2);
		if (p && p[0]) {
		    creat = append(creat, p, 0);
		}
		if (sqlite3_column_int(stmt, 5)) {
		    creat = append(creat, " PRIMARY KEY", 0);
		}
		if (sqlite3_column_int(stmt, 3)) {
		    creat = append(creat, " NOT NULL", 0);
		}
		p = (const char *) sqlite3_column_text(stmt, 4);
		if (p && p[0]) {
		    creat = append(creat, " DEFAULT ", 0);
		    creat = append(creat, p, 0);
		}
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
		    creat = append(creat, ",", 0);
		}
	    }
	    rc = sqlite3_finalize(stmt);
	    if (rc != SQLITE_OK) {
		goto bailout0;
	    }
	    creat = append(creat, ")", 0);
	    if (creat && fprintf(dd->out, "CREATE TABLE %s;\n", creat) > 0) {
		dd->nlines++;
	    }
	    if (creat) {
		sqlite3_free(creat);
	    }
	}
    } else {
	if (dd->with_schema) {
	    if (fprintf(dd->out, "%s;\n", sql) > 0) {
		dd->nlines++;
	    }
	}
    }
    if (strcmp(type, "table") == 0) {
	sqlite3_stmt *stmt = 0;
	char *select = 0, *table_info = 0, *tmp = 0;
   
	table_info = append(table_info, "PRAGMA table_info(", 0);
	table_info = append(table_info, table, '"');
	table_info = append(table_info, ");", 0);
	rc = sqlite3_prepare(dd->db, table_info, -1, &stmt, 0);
	if (rc != SQLITE_OK || !stmt) {
bailout1:
	    if (select) {
		sqlite3_free(select);
	    }
	    if (table_info) {
		sqlite3_free(table_info);
	    }
	    return 1;
	}
	if (dd->with_schema) {
	    select = append(select, "SELECT 'INSERT INTO ' || ", 0);
	} else {
	    select = append(select, "SELECT 'INSERT OR REPLACE INTO ' || ", 0);
	}
	tmp = append(tmp, table, '"');
	if (tmp) {
	    select = append(select, tmp, '\'');
	    sqlite3_free(tmp);
	    tmp = 0;
	}
	if (!dd->with_schema) {
	    select = append(select, " || ' (' || ", 0);
	    rc = sqlite3_step(stmt);
	    while (rc == SQLITE_ROW) {
		const char *text = (const char *) sqlite3_column_text(stmt, 1);

		tmp = append(tmp, text, '"');
		if (tmp) {
		    select = append(select, tmp, '\'');
		    sqlite3_free(tmp);
		    tmp = 0;
		}
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
		    select = append(select, " || ',' || ", 0);
		}
	    }
	    rc = sqlite3_finalize(stmt);
	    if (rc != SQLITE_OK) {
		goto bailout1;
	    }
	    select = append(select, "|| ')'", 0);
	    rc = sqlite3_prepare(dd->db, table_info, -1, &stmt, 0);
	    if (rc != SQLITE_OK || !stmt) {
		goto bailout1;
	    }
	}
	select = append(select, " || ' VALUES(' || ", 0);
	rc = sqlite3_step(stmt);
	while (rc == SQLITE_ROW) {
	    const char *text = (const char *) sqlite3_column_text(stmt, 1);

	    select = append(select, "quote(", 0);
	    select = append(select, text, '"');
	    rc = sqlite3_step(stmt);
	    if (rc == SQLITE_ROW) {
		select = append(select, ") || ',' || ", 0);
	    } else {
		select = append(select, ") ", 0);
	    }
	}
	rc = sqlite3_finalize(stmt);
	if (rc != SQLITE_OK) {
	    goto bailout1;
	}
	select = append(select, "|| ')' FROM  ", 0);
	select = append(select, table, '"');
	if (dd->where) {
	    select = append(select, " ", 0);
	    select = append(select, dd->where, 0);
	}
	if (table_info) {
	    sqlite3_free(table_info);
	}
	rc = table_dump(dd, 0, 0, select);
	if (rc == SQLITE_CORRUPT) {
	    select = append(select, " ORDER BY rowid DESC", 0);
	    rc = table_dump(dd, 0, 0, select);
	}
	if (select) {
	    sqlite3_free(select);
	}
    }
    return 0;
}

static int
schema_dump(DUMP_DATA *dd, char **errp, const char *query, ...)
{
    int rc;
    char *q;
    va_list ap;

    if (errp) {
	sqlite3_free(*errp);
	*errp = 0;
    }
    va_start(ap, query);
    q = sqlite3_vmprintf(query, ap);
    va_end(ap);
    if (!q) {
	return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(dd->db, q, dump_cb, dd, errp);
    if (rc == SQLITE_CORRUPT) {
	char *tmp;

	tmp = sqlite3_mprintf("%s ORDER BY rowid DESC", q);
	sqlite3_free(q);
	if (!tmp) {
	    return rc;
	}
	q = tmp;
	if (errp) {
	    sqlite3_free(*errp);
	    *errp = 0;
	}
	rc = sqlite3_exec(dd->db, q, dump_cb, dd, errp);
    }
    sqlite3_free(q);
    return rc;
}

static void
export_func(sqlite3_context *ctx, int nargs, sqlite3_value **args)
{
    DUMP_DATA dd0, *dd = &dd0;
    sqlite3 *db = (sqlite3 *) sqlite3_user_data(ctx);
    int i, mode = 0;
    char *filename = 0;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
    if (nargs > 0) {
	if (sqlite3_value_type(args[0]) != SQLITE_NULL) {
	    filename = (char *) sqlite3_value_text(args[0]);
	}
    }
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    dd->out = fopen(filename, "w");
    if (!dd->out) {
	goto done;
    }
    if (nargs > 1) {
	mode = sqlite3_value_int(args[1]);
    }
    dd->with_schema = !(mode & 1);
    dd->nlines = 0;
    if (fputs("BEGIN TRANSACTION;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    if (nargs <= 2) {
	schema_dump(dd, 0,
		    "SELECT name, type, sql FROM sqlite_master "
		    "WHERE sql NOT NULL AND type = 'table'");
	if (dd->with_schema) {
	    table_dump(dd, 0, 0,
		       "SELECT sql FROM sqlite_master WHERE "
		       "sql NOT NULL AND type IN ('index','trigger','view')");
	}
    } else {
	for (i = 2; i < nargs; i += (mode & 2) ? 2 : 1) {
	    dd->where = 0;
	    if ((mode & 2) && i + 1 < nargs) {
		dd->where = (char *) sqlite3_value_text(args[i + 1]);
	    }
	    schema_dump(dd, 0,
			"SELECT name, type, sql FROM sqlite_master "
			"WHERE tbl_name = %Q AND type = 'table'"
			"  AND sql NOT NULL",
			sqlite3_value_text(args[i]));
	    if (dd->with_schema) {
		table_dump(dd, 0, 1,
			   "SELECT sql FROM sqlite_master "
			   "WHERE sql NOT NULL"
			   "  AND type IN ('index','trigger','view')"
			   "  AND tbl_name = %Q",
			   sqlite3_value_text(args[i]));
	    }
	}
    }
    if (fputs("COMMIT;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    fclose(dd->out);
done:
    sqlite3_result_int(ctx, dd->nlines);
}

#ifdef STANDALONE
int
impexp_export_sql(sqlite3 *db, char *filename, int mode, ...)
{
    DUMP_DATA dd0, *dd = &dd0;
    va_list ap;
    char *table;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (!db) {
	return 0;
    }
    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    dd->out = fopen(filename, "w");
    if (!dd->out) {
	goto done;
    }
    dd->with_schema = !(mode & 1);
    dd->nlines = 0;
    if (fputs("BEGIN TRANSACTION;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    va_start(ap, mode);
    table = va_arg(ap, char *);
    if (!table) {
	schema_dump(dd, 0,
		    "SELECT name, type, sql FROM sqlite_master "
		    "WHERE sql NOT NULL AND type = 'table'");
	if (dd->with_schema) {
	    table_dump(dd, 0, 0,
		       "SELECT sql FROM sqlite_master WHERE "
		       "sql NOT NULL AND type IN ('index','trigger','view')");
	}
    } else {
	while (table) {
	    dd->where = 0;
	    if ((mode & 2)) {
		dd->where = va_arg(ap, char *);
	    }
	    schema_dump(dd, 0,
			"SELECT name, type, sql FROM sqlite_master "
			"WHERE tbl_name LIKE %s AND type = 'table'"
			"  AND sql NOT NULL", table);
	    if (dd->with_schema) {
		table_dump(dd, 0, 1,
			   "SELECT sql FROM sqlite_master "
			   "WHERE sql NOT NULL"
			   "  AND type IN ('index','trigger','view')"
			   "  AND tbl_name LIKE %s", table);
	    }
	    table = va_arg(ap, char *);
	}
    }
    va_end(ap);
    if (fputs("COMMIT;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    fclose(dd->out);
done:
    return dd->nlines;
}
#endif

#ifdef STANDALONE
int
impexp_init(sqlite3 *db)
#else
int
sqlite3_extension_init(sqlite3 *db, char **errmsg, 
		       const sqlite3_api_routines *api)
#endif
{
    int rc;
#ifndef STANDALONE
    SQLITE_EXTENSION_INIT2(api);
#endif  

    rc = sqlite3_create_function(db, "import_sql", -1, SQLITE_UTF8,
				 db, import_func, 0, 0);
    if (rc != SQLITE_OK) {
	return rc;
    }
    rc = sqlite3_create_function(db, "export_sql", -1, SQLITE_UTF8,
				 db, export_func, 0, 0);
    if (rc != SQLITE_OK) {
	sqlite3_create_function(db, "import_sql", -1, SQLITE_UTF8,
				0, 0, 0, 0);
    }
    return rc;
}

