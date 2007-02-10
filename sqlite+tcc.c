/*
** 2006 August 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Experimental combination of SQLite 3.3.x and TinyCC 0.9.23
** using the new SQLite loadable extension mechanism.
**
** This module creates an SQLite scalar function 'tcc_compile'
** which takes one argument which is a string made up of
** C source code to be on-the-fly compiled using TinyCC.
** The SQLite API is visible during compilation when
** 'sqlite3.h' is included.
**
** WARNING: TinyCC's libtcc.a must be created with necessary
** compiler switches to be linked into a shared object. This
** usually means that "-fPIC" must be added to compiler flags
** in TinyCC's Makefile for the targets libtcc.o/libtcc.a.
*/

#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include "libtcc.h"
#include "sqlite3ext.h"

#define APIOFF(n) ((int) ((char *) &((sqlite3_api_routines *) 0)->n))
#define SYM(n) { "sqlite3_"#n, APIOFF(n) }
#define SYM2(n, n2) { "sqlite3_"#n, APIOFF(n2) }
#define SYM_END { 0, 0 }

static struct {
  const char *name;	/* TCC name for tcc_add_symbol() */
  int offs;		/* Offset into struct sqlite3_api_routines */
} symtab[] = {
  SYM(aggregate_context),
  SYM(aggregate_count),
  SYM(bind_blob),
  SYM(bind_double),
  SYM(bind_int),
  SYM(bind_int64),
  SYM(bind_null),
  SYM(bind_parameter_count),
  SYM(bind_parameter_index),
  SYM(bind_parameter_name),
  SYM(bind_text),
  SYM(bind_text16),
  SYM(bind_value),
  SYM(busy_handler),
  SYM(busy_timeout),
  SYM(changes),
  SYM(close),
  SYM(collation_needed),
  SYM(collation_needed16),
  SYM(column_blob),
  SYM(column_bytes),
  SYM(column_bytes16),
  SYM(column_count),
  SYM(column_database_name),
  SYM(column_database_name16),
  SYM(column_decltype),
  SYM(column_decltype16),
  SYM(column_double),
  SYM(column_int),
  SYM(column_int64),
  SYM(column_name),
  SYM(column_name16),
  SYM(column_origin_name),
  SYM(column_origin_name16),
  SYM(column_table_name),
  SYM(column_table_name16),
  SYM(column_text),
  SYM(column_text16),
  SYM(column_type),
  SYM(column_value),
  SYM(commit_hook),
  SYM(complete),
  SYM(complete16),
  SYM(create_collation),
  SYM(create_collation16),
  SYM(create_function),
  SYM(create_function16),
  SYM(create_module),
  SYM(data_count),
  SYM(db_handle),
  SYM(declare_vtab),
  SYM(enable_shared_cache),
  SYM(errcode),
  SYM(errmsg),
  SYM(errmsg16),
  SYM(exec),
  SYM(expired),
  SYM(finalize),
  SYM(free),
  SYM(free_table),
  SYM(get_autocommit),
  SYM(get_auxdata),
  SYM(get_table),
#if 0
  SYM(global_recover),
#endif
  SYM2(interrupt, interruptx),
  SYM(last_insert_rowid),
  SYM(libversion),
  SYM(libversion_number),
  SYM(malloc),
  SYM(mprintf),
  SYM(open),
  SYM(open16),
  SYM(prepare),
  SYM(prepare16),
  SYM(profile),
  SYM(progress_handler),
  SYM(realloc),
  SYM(reset),
  SYM(result_blob),
  SYM(result_double),
  SYM(result_error),
  SYM(result_error16),
  SYM(result_int),
  SYM(result_int64),
  SYM(result_null),
  SYM(result_text),
  SYM(result_text16),
  SYM(result_text16be),
  SYM(result_text16le),
  SYM(result_value),
  SYM(rollback_hook),
  SYM(set_authorizer),
  SYM(set_auxdata),
  SYM(snprintf),
  SYM(step),
  SYM(table_column_metadata),
  SYM(thread_cleanup),
  SYM(total_changes),
  SYM(trace),
  SYM(transfer_bindings),
  SYM(update_hook),
  SYM(user_data),
  SYM(value_blob),
  SYM(value_bytes),
  SYM(value_bytes16),
  SYM(value_double),
  SYM(value_int),
  SYM(value_int64),
  SYM(value_numeric_type),
  SYM(value_text),
  SYM(value_text16),
  SYM(value_text16be),
  SYM(value_text16le),
  SYM(value_type),
  SYM(vmprintf),
  SYM_END
};

#ifdef _WIN32
/* TCC is not thread safe, provide a guard */
static CRITICAL_SECTION tcc_mutex;
#endif

/* Module pointer to SQLite3 API function vector. */
static SQLITE_EXTENSION_INIT1

static void tcc_compile(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  TCCState *t;
  int i;
  unsigned long val;
  void (*xInit)(void *);
  sqlite3 *db = sqlite3_user_data(ctx);
#ifdef _WIN32
  EnterCriticalSection(&tcc_mutex);
#endif
  t = tcc_new();
  if( !t ){
    sqlite3_result_error(ctx, "no compile context", -1);
#ifdef _WIN32
    LeaveCriticalSection(&tcc_mutex);
#endif
    return;
  }
  tcc_set_output_type(t, TCC_OUTPUT_MEMORY);
  if( tcc_compile_string(t, sqlite3_value_text(argv[0])) ){
    sqlite3_result_error(ctx, "compile error", -1);
    goto error;
  }
  for( i = 0; symtab[i].name; i++ ){
    val = ((unsigned long *) ((char *) sqlite3_api + symtab[i].offs))[0];
    if( val ){
      tcc_add_symbol(t, symtab[i].name, val);
    } else {
#ifdef _WIN32
      char msg[512];

      sprintf(msg, "API function '%s' not available.", symtab[i].name);
      MessageBox(NULL, msg, "SQLITE+TCC", MB_ICONEXCLAMATION | MB_OK |
		 MB_TASKMODAL | MB_SETFOREGROUND);
#else
      fprintf(stderr, "API function '%s' not available\n", symtab[i].name);
#endif
    }
  }
  if( tcc_relocate(t) ){
    sqlite3_result_error(ctx, "link error", -1);
    goto error;
  }
  if( tcc_get_symbol(t, &val, "init") != 0 ){
    sqlite3_result_error(ctx, "no init function", -1);
    goto error;
  }
#ifdef _WIN32
  LeaveCriticalSection(&tcc_mutex);
#endif
  xInit = (void *) val;
  xInit(db);
  return;
error:
  tcc_delete(t);
#ifdef _WIN32
  LeaveCriticalSection(&tcc_mutex);
#endif
}

int sqlite3_extension_init(
  sqlite3 *db,				/* SQLite3 database connection */ 
  char **pzErrMsg,			/* Put error message here if not 0 */
  const sqlite3_api_routines *api	/* SQLite3 API entries */
){
  SQLITE_EXTENSION_INIT2(api);
  if( sqlite3_create_function(db, "tcc_compile", 1, SQLITE_UTF8, db,
      tcc_compile, 0, 0) != SQLITE_OK ){
    if( pzErrMsg ){
      *pzErrMsg = sqlite3_mprintf("cannot create function");
    }
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

#ifdef _WIN32

BOOL APIENTRY LibMain(
  HANDLE hinst,
  DWORD reason,
  LPVOID reserved
){
  static int initialized = 0;
  static long lock;
  if( reason==DLL_PROCESS_ATTACH ){
    while( !initialized ){
      if( InterlockedIncrement(&lock)==1 ){
        InitializeCriticalSection(&tcc_mutex);
        tcc_set_lib_path(hinst);
        ++initialized;
      }else{
        Sleep(1);
      }
    }
  }
  return TRUE;
}

int __stdcall DllMain(
  HANDLE hinst,
  DWORD reason,
  LPVOID reserved
){
  return LibMain(hinst, reason, reserved);
}

#endif
