This folder contains the amalgamation (a10n) of SQLite3 >= 3.3.14

In order to use tcc to on-the-fly compile and execute it, do

  <Linux>
  tcc -Dsqlite3_main=main sqlite3.c -run [db_file_name [sql_text]]

Unfortunately, this bombs on Win32, but it is possible to
build a self-contained SQLite3 shell with this command line

  <Win32>
  ..\..\tcc -Dsqlite3_main=main -o sqlite3.exe sqlite3.c -lkernel32

