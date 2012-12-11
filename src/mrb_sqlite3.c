#include <errno.h>
#include <memory.h>
#include <mruby.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <sqlite3.h>

static struct RClass *_class_sqlite3;
static struct RClass *_class_sqlite3_database;
static struct RClass *_class_sqlite3_statement;

typedef struct {
  mrb_state *mrb;
  sqlite3* db;
} mrb_sqlite3_database;

typedef struct {
  mrb_state *mrb;
  sqlite3* db;
  sqlite3_stmt* stmt;
} mrb_sqlite3_statement;

static void
mrb_sqlite3_database_free(mrb_state *mrb, void *p) {
  mrb_sqlite3_database* db = (mrb_sqlite3_database*) p;
  if (db->db) {
    sqlite3_close(db->db);
  }
  free(db);
}

static void
mrb_sqlite3_statement_free(mrb_state *mrb, void *p) {
  mrb_sqlite3_statement* stmt = (mrb_sqlite3_statement*) p;
  if (stmt->stmt) {
  	sqlite3_finalize(stmt->stmt);
  }
  free(stmt);
}

static const struct mrb_data_type mrb_sqlite3_database_type = {
  "mrb_sqlite3_database", mrb_sqlite3_database_free,
};

static const struct mrb_data_type mrb_sqlite3_statement_type = {
  "mrb_sqlite3_statement", mrb_sqlite3_statement_free,
};

static mrb_value
mrb_sqlite3_database_init(mrb_state *mrb, mrb_value self) {
  char* name = NULL;
  mrb_value arg_file;
  mrb_get_args(mrb, "o", &arg_file);
  if (!mrb_nil_p(arg_file)) {
    char* data = RSTRING_PTR(arg_file);
    size_t len = RSTRING_LEN(arg_file);
    name = malloc(len + 1);
    strncpy(name, data, len);
  }

  sqlite3* sdb = NULL;
  int rv = sqlite3_open_v2(name ? name : ":memory:", &sdb,
      SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
      NULL);
  free(name);

  mrb_sqlite3_database* db = (mrb_sqlite3_database*)
    malloc(sizeof(mrb_sqlite3_database));
  if (!db) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "can't memory alloc");
  }
  memset(db, 0, sizeof(mrb_sqlite3_database));
  db->mrb = mrb;
  db->db = sdb;
  mrb_iv_set(mrb, self, mrb_intern(mrb, "context"), mrb_obj_value(
    Data_Wrap_Struct(mrb, mrb->object_class,
    &mrb_sqlite3_database_type, (void*) db)));
  return self;
}

static mrb_value
row_to_value(mrb_state* mrb, sqlite3_stmt* stmt) {
  int i;
  int count = sqlite3_column_count(stmt);
  mrb_value a = mrb_ary_new(mrb);
  for (i = 0; i < count; i++) {
		switch (sqlite3_column_type(stmt, i)) {
    case SQLITE_INTEGER:
      {
        sqlite3_int64 value = sqlite3_column_int64(stmt, i);
        mrb_ary_push(mrb, a, mrb_fixnum_value(value));
      }
      break;
    case SQLITE_FLOAT:
      {
        double value = sqlite3_column_double(stmt, i);
        mrb_ary_push(mrb, a, mrb_float_value(value));
      }
      break;
    case SQLITE_BLOB:
      {
		    int size = sqlite3_column_bytes(stmt, i);
		    const char* ptr = sqlite3_column_blob(stmt, i);
        mrb_ary_push(mrb, a, mrb_str_new(mrb, ptr, size));
      }
      break;
    case SQLITE_NULL:
      mrb_ary_push(mrb, a, mrb_nil_value());
      break;
    case SQLITE_TEXT:
      {
        const char* value = sqlite3_column_text(stmt, i);
        mrb_ary_push(mrb, a, mrb_str_new_cstr(mrb, value));
      }
      break;
    }
  }
  return a;
}

static mrb_value
mrb_sqlite3_database_execute(mrb_state *mrb, mrb_value self) {
  mrb_value *argv;
  int argc = 0;
  mrb_value value_context;
  mrb_sqlite3_database* db = NULL;
  mrb_sqlite3_statement* stmt = NULL;
  struct RProc* b = NULL;
	sqlite3_stmt* sstmt = NULL;
  int i;

  mrb_get_args(mrb, "*&", &argv, &argc, &b);

  if (argc == 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }

  int r;
  mrb_value query = argv[0];
	const char* error = NULL;
	r = sqlite3_prepare_v2(db->db, RSTRING_PTR(query), RSTRING_LEN(query), &sstmt, &error);
	if (r != SQLITE_OK) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
  }

  for (i = 1; i < argc; i++) {
    int rv = SQLITE_MISMATCH;
		switch (mrb_type(argv[i])) {
		case MRB_TT_UNDEF:
			rv = sqlite3_bind_null(sstmt, i);
      break;
		case MRB_TT_STRING:
		  rv = sqlite3_bind_text(sstmt, i, RSTRING_PTR(argv[i]), RSTRING_LEN(argv[i]), NULL);
      break;
		case MRB_TT_FIXNUM:
			rv = sqlite3_bind_int(sstmt, i, mrb_fixnum(argv[i]));
      break;
		case MRB_TT_FLOAT:
			rv = sqlite3_bind_double(sstmt, i, mrb_float(argv[i]));
      break;
		case MRB_TT_TRUE:
			rv = sqlite3_bind_int(sstmt, i, 1);
      break;
		case MRB_TT_FALSE:
			rv = sqlite3_bind_int(sstmt, i, 0);
      break;
    default:
      mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
      break;
		}
		if (rv != SQLITE_OK) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, sqlite3_errmsg(db->db));
		}
  }

  mrb_value fields = mrb_ary_new(mrb);
  int count = sqlite3_column_count(sstmt);
  for (i = 0; i < count; i++) {
    const char* name = sqlite3_column_name(sstmt, i);
    mrb_ary_push(mrb, fields, mrb_str_new_cstr(mrb, name));
  }

  if (!b) {
    stmt = (mrb_sqlite3_statement*) malloc(sizeof(mrb_sqlite3_statement));
    if (!stmt) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "can't memory alloc");
    }
    memset(stmt, 0, sizeof(mrb_sqlite3_statement));
    stmt->mrb = mrb;
    stmt->db = db->db;
    stmt->stmt = sstmt;
  
    mrb_value c = mrb_class_new_instance(mrb, 0, NULL, _class_sqlite3_statement);
    mrb_iv_set(mrb, c, mrb_intern(mrb, "context"), mrb_obj_value(
      Data_Wrap_Struct(mrb, mrb->object_class,
      &mrb_sqlite3_statement_type, (void*) stmt)));
    mrb_iv_set(mrb, c, mrb_intern(mrb, "fields"), fields);
    return c;
  }
  mrb_value args[2];
  mrb_value proc = mrb_obj_value(b);
  while ((r = sqlite3_step(sstmt)) == SQLITE_ROW) {
    int ai = mrb_gc_arena_save(mrb);
    args[0] = row_to_value(mrb, sstmt);
    args[1] = fields;
    mrb_yield_argv(mrb, proc, 2, args);
    mrb_gc_arena_restore(mrb, ai);
  }
  if (r != SQLITE_OK && r != SQLITE_DONE) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
  }
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_database_close(mrb_state *mrb, mrb_value self) {
  mrb_value value_context;
  mrb_sqlite3_database* db = NULL;

  value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
	sqlite3_stmt* stmt = sqlite3_next_stmt(db->db, NULL);
  while (stmt != NULL) {
		sqlite3_finalize(stmt);
		stmt = sqlite3_next_stmt(db->db, NULL);
  }
  if (sqlite3_close(db->db) != SQLITE_OK) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
  }
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_statement_next(mrb_state *mrb, mrb_value self) {
  mrb_value value_context;
  mrb_sqlite3_statement* stmt = NULL;

  value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_statement_type, stmt);
  if (!stmt) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  int r = sqlite3_step(stmt->stmt);
  if (r != SQLITE_ROW && r != SQLITE_OK && r != SQLITE_DONE) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(stmt->db));
  }
  return row_to_value(mrb, stmt->stmt);
}

static mrb_value
mrb_sqlite3_statement_close(mrb_state *mrb, mrb_value self) {
  mrb_value value_context;
  mrb_sqlite3_statement* stmt = NULL;

  value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_statement_type, stmt);
  if (!stmt) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  if (sqlite3_finalize(stmt->stmt) != SQLITE_OK) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(stmt->db));
  }
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_statement_fields(mrb_state *mrb, mrb_value self) {
  return mrb_iv_get(mrb, self, mrb_intern(mrb, "fields"));
}

void
mrb_mruby_sqlite3_gem_init(mrb_state* mrb) {
  int ai;

  _class_sqlite3 = mrb_define_module(mrb, "SQLite3");

  ai = mrb_gc_arena_save(mrb);
  _class_sqlite3_database = mrb_define_class_under(mrb, _class_sqlite3, "Database", mrb->object_class);
  mrb_define_method(mrb, _class_sqlite3_database, "initialize", mrb_sqlite3_database_init, ARGS_OPT(1));
  mrb_define_method(mrb, _class_sqlite3_database, "execute", mrb_sqlite3_database_execute, ARGS_OPT(1));
  mrb_define_method(mrb, _class_sqlite3_database, "close", mrb_sqlite3_database_close, ARGS_NONE());
  mrb_gc_arena_restore(mrb, ai);

  ai = mrb_gc_arena_save(mrb);
  _class_sqlite3_statement = mrb_define_class_under(mrb, _class_sqlite3, "Statement", mrb->object_class);
  mrb_define_method(mrb, _class_sqlite3_statement, "next", mrb_sqlite3_statement_next, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_statement, "close", mrb_sqlite3_statement_close, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_statement, "fields", mrb_sqlite3_statement_fields, ARGS_NONE());
  mrb_gc_arena_restore(mrb, ai);
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */