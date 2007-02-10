/*
 * Use sqlite_main() entry point in sqliteodbc.dll/sqliteodbcu.dll
 * to provide an SQLite shell, compile with
 *
 *  tcc -o sqlite.exe -lsqlite sqlite.c
 *  tcc -o sqliteu.exe -lsqliteu sqlite.c
 */

extern int sqlite_main(int, char **);

int main(int argc, char **argv) { return sqlite_main(argc, argv); }
