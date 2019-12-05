#include "util.h"
#include "database.h"

/**
 * データーベースを初期化する
 * @param[in] env 環境
 * @param[in] db_path 初期化するデータベースファイル名
 * @return sqlite3のエラーコード
 * @retval 0 成功
 */
int
init_database(wiser_env *env, const char *db_path)
{
  int rc;
  if ((rc = sqlite3_open(db_path, &env->db))) {
    print_error("cannot open databases.");
    return rc;
  }

  sqlite3_exec(env->db,
               "CREATE TABLE settings (" \
               "  key   TEXT PRIMARY KEY," \
               "  value TEXT" \
               ");",
               NULL, NULL, NULL);

  sqlite3_exec(env->db,
               "CREATE TABLE documents (" \
               "  id      INTEGER PRIMARY KEY," /* auto increment */ \
               "  title   TEXT NOT NULL," \
               "  body    TEXT NOT NULL" \
               ");",
               NULL, NULL, NULL);

  sqlite3_exec(env->db,
               "CREATE TABLE tokens (" \
               "  id         INTEGER PRIMARY KEY," \
               "  token      TEXT NOT NULL," \
               "  docs_count INT NOT NULL," \
               "  postings   BLOB NOT NULL" \
               ");",
               NULL, NULL, NULL);

  sqlite3_exec(env->db,
               "CREATE UNIQUE INDEX token_index ON tokens(token);",
               NULL, NULL, NULL);

  sqlite3_exec(env->db,
               "CREATE UNIQUE INDEX title_index ON documents(title);" ,
               NULL, NULL, NULL);

  sqlite3_prepare(env->db,
                  "SELECT id FROM documents WHERE title = ?;",
                  -1, &env->get_document_id_st, NULL);
  sqlite3_prepare(env->db,
                  "SELECT title FROM documents WHERE id = ?;",
                  -1, &env->get_document_title_st, NULL);
  sqlite3_prepare(env->db,
                  "INSERT INTO documents (title, body) VALUES (?, ?);",
                  -1, &env->insert_document_st, NULL);
  sqlite3_prepare(env->db,
                  "UPDATE documents set body = ? WHERE id = ?;",
                  -1, &env->update_document_st, NULL);
  sqlite3_prepare(env->db,
                  "SELECT id, docs_count FROM tokens WHERE token = ?;",
                  -1, &env->get_token_id_st, NULL);
  sqlite3_prepare(env->db,
                  "SELECT token FROM tokens WHERE id = ?;",
                  -1, &env->get_token_st, NULL);
  sqlite3_prepare(env->db,
                  "INSERT OR IGNORE INTO tokens (token, docs_count, postings)"
                  " VALUES (?, 0, ?);",
                  -1, &env->store_token_st, NULL);
  sqlite3_prepare(env->db,
                  "SELECT docs_count, postings FROM tokens WHERE id = ?;",
                  -1, &env->get_postings_st, NULL);
  sqlite3_prepare(env->db,
                  "UPDATE tokens SET docs_count = ?, postings = ? WHERE id = ?;",
                  -1, &env->update_postings_st, NULL);
  sqlite3_prepare(env->db,
                  "SELECT value FROM settings WHERE key = ?;",
                  -1, &env->get_settings_st, NULL);
  sqlite3_prepare(env->db,
                  "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);",
                  -1, &env->replace_settings_st, NULL);
  sqlite3_prepare(env->db,
                  "SELECT COUNT(*) FROM documents;",
                  -1, &env->get_document_count_st, NULL);
  sqlite3_prepare(env->db,
                  "BEGIN;",
                  -1, &env->begin_st, NULL);
  sqlite3_prepare(env->db,
                  "COMMIT;",
                  -1, &env->commit_st, NULL);
  sqlite3_prepare(env->db,
                  "ROLLBACK;",
                  -1, &env->rollback_st, NULL);
  return 0;
}

/**
 * データベースを閉じる。
 * @param[in] env 環境
 */
void
fin_database(wiser_env *env)
{
  sqlite3_finalize(env->get_document_id_st);
  sqlite3_finalize(env->get_document_title_st);
  sqlite3_finalize(env->insert_document_st);
  sqlite3_finalize(env->update_document_st);
  sqlite3_finalize(env->get_token_id_st);
  sqlite3_finalize(env->get_token_st);
  sqlite3_finalize(env->store_token_st);
  sqlite3_finalize(env->get_postings_st);
  sqlite3_finalize(env->update_postings_st);
  sqlite3_finalize(env->get_settings_st);
  sqlite3_finalize(env->replace_settings_st);
  sqlite3_finalize(env->get_document_count_st);
  sqlite3_finalize(env->begin_st);
  sqlite3_finalize(env->commit_st);
  sqlite3_finalize(env->rollback_st);
  sqlite3_close(env->db);
}

/**
 * 指定のタイトルを持つ文書のIDを取得する。
 * @param[in] env 環境
 * @param[in] title 文書タイトル
 * @param[in] title_size 文書タイトルのバイト長
 * @return 文書ID
 */
int
db_get_document_id(const wiser_env *env,
                   const char *title, unsigned int title_size)
{
  int rc;
  sqlite3_reset(env->get_document_id_st);
  sqlite3_bind_text(env->get_document_id_st, 1,
                    title, title_size, SQLITE_STATIC);
  rc = sqlite3_step(env->get_document_id_st);
  if (rc == SQLITE_ROW) {
    return sqlite3_column_int(env->get_document_id_st, 0);
  } else {
    return 0;
  }
}

/**
 * 指定の文書IDを持つ文書のタイトルを取得する。
 * @param[in] env 環境
 * @param[in] document_id 文書ID
 * @param[out] title 文書タイトル
 * @param[out] title_size 文書タイトルのバイト長
 */
int
db_get_document_title(const wiser_env *env, int document_id,
                      const char **title, int *title_size)
{
  int rc;

  sqlite3_reset(env->get_document_title_st);
  sqlite3_bind_int(env->get_document_title_st, 1, document_id);

  rc = sqlite3_step(env->get_document_title_st);
  if (rc == SQLITE_ROW) {
    if (title) {
      *title = (const char *)sqlite3_column_text(env->get_document_title_st,
               0);
    }
    if (title_size) {
      *title_size = (int)sqlite3_column_bytes(env->get_document_title_st,
                                              0);
    }
  }
  return 0;
}

/**
 * documentsテーブルに、文書を登録する。
 * @param[in] env 環境
 * @param[in] title 文書タイトル
 * @param[in] title_size 文書タイトルのバイト長
 * @param[in] body 文書本体
 * @param[in] body_size 文書本体のバイト長
 */
int
db_add_document(const wiser_env *env,
                const char *title, unsigned int title_size,
                const char *body, unsigned int body_size)
{
  sqlite3_stmt *st;
  int rc, document_id;

  if ((document_id = db_get_document_id(env, title, title_size))) {
    st = env->update_document_st;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, body, body_size, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, document_id);
  } else {
    st = env->insert_document_st;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, title, title_size, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, body, body_size, SQLITE_STATIC);
  }
query:
  rc = sqlite3_step(st);
  switch (rc) {
  case SQLITE_BUSY:
    goto query;
  case SQLITE_ERROR:
    print_error("ERROR: %s", sqlite3_errmsg(env->db));
    break;
  case SQLITE_MISUSE:
    print_error("MISUSE: %s", sqlite3_errmsg(env->db));
    break;
  }
  return rc;
}

/**
 * tokensテーブルから、token文字列のidを取得する。
 * @param[in] env 環境
 * @param[in] str token文字列(UTF-8)
 * @param[in] str_size token文字列のバイト長。
 * @param[in] insert tokenが存在しないとき、tokenを登録するかどうか
 * @param[out] docs_count tokenを含むドキュメント数。
 */
int
db_get_token_id(const wiser_env *env,
                const char *str, unsigned int str_size, int insert,
                int *docs_count)
{
  int rc;
  if (insert) {
    sqlite3_reset(env->store_token_st);
    sqlite3_bind_text(env->store_token_st, 1, str, str_size,
                      SQLITE_STATIC);
    sqlite3_bind_blob(env->store_token_st, 2, "", 0, SQLITE_STATIC);
    rc = sqlite3_step(env->store_token_st);
  }
  sqlite3_reset(env->get_token_id_st);
  sqlite3_bind_text(env->get_token_id_st, 1, str, str_size,
                    SQLITE_STATIC);
  rc = sqlite3_step(env->get_token_id_st);
  if (rc == SQLITE_ROW) {
    if (docs_count) {
      *docs_count = sqlite3_column_int(env->get_token_id_st, 1);
    }
    return sqlite3_column_int(env->get_token_id_st, 0);
  } else {
    if (docs_count) {
      *docs_count = 0;
    }
    return 0;
  }
}

/**
 * tokensテーブルから、token文字列dを取得する。
 * @param[in] env 環境
 * @param[in] token_id token ID
 * @param[out] token token文字列(UTF-8)
 * @param[out] token_size token文字列のバイト長。
 */
int
db_get_token(const wiser_env *env,
             const int token_id,
             const char **const token, int *token_size)
{
  int rc;
  sqlite3_reset(env->get_token_st);
  sqlite3_bind_int(env->get_token_st, 1, token_id);
  rc = sqlite3_step(env->get_token_st);
  if (rc == SQLITE_ROW) {
    if (token) {
      *token = (const char *)sqlite3_column_text(env->get_token_st, 0);
    }
    if (token_size) {
      *token_size = (int)sqlite3_column_bytes(env->get_token_st, 0);
    }
  }
  return 0;
}

/**
 * データベースからpostings listを取得する。
 * @param[in] env 環境
 * @param[in] token_id トークンID
 * @param[out] docs_count 文書数
 * @param[out] postings 取得されたpostings list
 * @param[out] postings_size postings listのバイト長
 */
int
db_get_postings(const wiser_env *env, int token_id,
                int *docs_count, void **postings, int *postings_size)
{
  int rc;
  sqlite3_reset(env->get_postings_st);
  sqlite3_bind_int(env->get_postings_st, 1, token_id);
  rc = sqlite3_step(env->get_postings_st);
  if (rc == SQLITE_ROW) {
    if (docs_count) {
      *docs_count = sqlite3_column_int(env->get_postings_st, 0);
    }
    if (postings) {
      *postings = (void *)sqlite3_column_blob(env->get_postings_st, 1);
    }
    if (postings_size) {
      *postings_size = (int)sqlite3_column_bytes(env->get_postings_st, 1);
    }
    rc = 0;
  } else {
    if (docs_count) { *docs_count = 0; }
    if (postings) { *postings = NULL; }
    if (postings_size) { *postings_size = 0; }
    if (rc == SQLITE_DONE) { rc = 0; } /* no record found */
  }
  return rc;
}

/**
 * データベースにpostings listを保存する。
 * @param[in] env 環境
 * @param[in] token_id トークンID
 * @param[in] docs_count 文書数
 * @param[in] postings 保存するpostings list
 * @param[in] postings_size postings listのバイト長
 */
int
db_update_postings(const wiser_env *env, int token_id, int docs_count,
                   void *postings, int postings_size)
{
  int rc;
  sqlite3_reset(env->update_postings_st);
  sqlite3_bind_int(env->update_postings_st, 1, docs_count);
  sqlite3_bind_blob(env->update_postings_st, 2, postings,
                    (unsigned int)postings_size, SQLITE_STATIC);
  sqlite3_bind_int(env->update_postings_st, 3, token_id);
query:
  rc = sqlite3_step(env->update_postings_st);

  switch (rc) {
  case SQLITE_BUSY:
    goto query;
  case SQLITE_ERROR:
    print_error("ERROR: %s", sqlite3_errmsg(env->db));
    break;
  case SQLITE_MISUSE:
    print_error("MISUSE: %s", sqlite3_errmsg(env->db));
    break;
  }
  return rc;
}

/**
 * データベースから設定情報を取得する。
 * @param[in] env 環境
 * @param[in] key 設定項目名
 * @param[in] key_size 設定項目名のバイト長
 * @param[out] value 設定内容
 * @param[out] value_size 設定内容のバイト長
 */
int
db_get_settings(const wiser_env *env, const char *key, int key_size,
                const char **value, int *value_size)
{
  int rc;

  sqlite3_reset(env->get_settings_st);
  sqlite3_bind_text(env->get_settings_st, 1,
                    key, key_size, SQLITE_STATIC);
  rc = sqlite3_step(env->get_settings_st);
  if (rc == SQLITE_ROW) {
    if (value) {
      *value = (const char *)sqlite3_column_text(env->get_settings_st, 0);
    }
    if (value_size) {
      *value_size = (int)sqlite3_column_bytes(env->get_settings_st, 0);
    }
  }
  return 0;
}

/**
 * データベースに設定情報を上書き保存する。
 * @param[in] env 環境
 * @param[in] key 設定項目名
 * @param[in] key_size 設定項目名のバイト長
 * @param[in] value 設定内容
 * @param[in] value_size 設定内容のバイト長
 */
int
db_replace_settings(const wiser_env *env, const char *key,
                    int key_size,
                    const char *value, int value_size)
{
  int rc;
  sqlite3_reset(env->replace_settings_st);
  sqlite3_bind_text(env->replace_settings_st, 1,
                    key, key_size, SQLITE_STATIC);
  sqlite3_bind_text(env->replace_settings_st, 2,
                    value, value_size, SQLITE_STATIC);
query:
  rc = sqlite3_step(env->replace_settings_st);

  switch (rc) {
  case SQLITE_BUSY:
    goto query;
  case SQLITE_ERROR:
    print_error("ERROR: %s", sqlite3_errmsg(env->db));
    break;
  case SQLITE_MISUSE:
    print_error("MISUSE: %s", sqlite3_errmsg(env->db));
    break;
  }
  return rc;
}

/**
 * データベースに登録された文書数を取得する。
 * @param[in] env 環境
 */
int
db_get_document_count(const wiser_env *env)
{
  int rc;

  sqlite3_reset(env->get_document_count_st);
  rc = sqlite3_step(env->get_document_count_st);
  if (rc == SQLITE_ROW) {
    return sqlite3_column_int(env->get_document_count_st, 0);
  } else {
    return -1;
  }
}

/**
 * トランザクションを開始する。
 * @param[in] env 環境
 */
int
begin(const wiser_env *env)
{
  return sqlite3_step(env->begin_st);
}

/**
 * トランザクションをcommitする。
 * @param[in] env 環境
 */
int
commit(const wiser_env *env)
{
  return sqlite3_step(env->commit_st);
}

/**
 * トランザクションをrollbackする。
 * @param[in] env 環境
 */
int
rollback(const wiser_env *env)
{
  return sqlite3_step(env->rollback_st);
}
