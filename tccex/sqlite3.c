/*
 * Use sqlite3_main() entry point in sqlite3odbc.dll
 * to provide an SQLite3 shell, compile with
 *
 *  tcc -o sqlite3.exe -lsqlite3 sqlite3.c
 */

extern int sqlite3_main(int, char **);

int main(int argc, char **argv) { return sqlite3_main(argc, argv); }
