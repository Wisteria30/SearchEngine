#ifndef __WISER_H__
#define __WISER_H__

#include <utlist.h>
#include <uthash.h>
#include <utarray.h>
#include <sqlite3.h>

/* bi-gram */
#define N_GRAM 2

/* ポスティングスリスト。文書IDのリンクリスト */
typedef struct _postings_list {
  int document_id;             /* 文書のID */
  UT_array *positions;         /* 特定文書中の位置情報配列 */
  int positions_count;         /* 特定文書中の位置情報の数 */
  struct _postings_list *next; /* 次のpostings_listへのリンク */
} postings_list;

/* 転置インデックス */
typedef struct {
  int token_id;                 /* トークンID */
  postings_list *postings_list; /* トークンを含むpostings list */
  int docs_count;               /* トークンを含む文書数 */
  int positions_count;          /* 全文書内でのトークン出現数 */
  UT_hash_handle hh;            /* ハッシュテーブル管理用 */
} inverted_index_hash, inverted_index_value;

/* postings list等の圧縮方法 */
typedef enum {
  compress_none,  /* 圧縮なし */
  compress_golomb /* golomb符号での圧縮 */
} compress_method;

/* アプリケーション全体の設定 */
typedef struct _wiser_env {
  const char *db_path;            /* データベースのパス。*/

  int token_len;                  /* トークンの長さ。N-gramのN。 */
  compress_method compress;       /* postings list等の圧縮方法 */
  int enable_phrase_search;       /* フレーズ検索をするかどうか */

  inverted_index_hash *ii_buffer; /* 更新用の転置インデックスバッファ */
  int ii_buffer_count;            /* 更新用の転置インデックスの文書数 */
  int ii_buffer_update_threshold; /* 更新用の転置インデックスの文書数 */
  int indexed_count;              /* インデックス化された文書数 */

  /* sqlite3関連 */
  sqlite3 *db; /* インスタンス */
  /* sqlite3のプリペアドステートメント */
  sqlite3_stmt *get_document_id_st;
  sqlite3_stmt *get_document_title_st;
  sqlite3_stmt *insert_document_st;
  sqlite3_stmt *update_document_st;
  sqlite3_stmt *get_token_id_st;
  sqlite3_stmt *get_token_st;
  sqlite3_stmt *store_token_st;
  sqlite3_stmt *get_postings_st;
  sqlite3_stmt *update_postings_st;
  sqlite3_stmt *get_settings_st;
  sqlite3_stmt *replace_settings_st;
  sqlite3_stmt *get_document_count_st;
  sqlite3_stmt *begin_st;
  sqlite3_stmt *commit_st;
  sqlite3_stmt *rollback_st;
} wiser_env;

/* TRUE/FALSE */
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif

#define DEFAULT_II_BUFFER_UPDATE_THRESHOLD 2048

#endif /* __WISER_H__ */
