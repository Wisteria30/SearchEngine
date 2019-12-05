#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "util.h"
#include "token.h"
#include "search.h"
#include "postings.h"
#include "database.h"
#include "wikiload.h"

/**
 * 文書をデータベースに追加し、転置インデックスを作成する
 * @param[in] env アプリケーション環境を保存する構造体
 * @param[in] title 文書タイトル、NULLの場合にはバッファをフラッシュする
 * @param[in] body 文書
 */
static void
add_document(wiser_env *env, const char *title, const char *body)
{
  if (title && body) {
    UTF32Char *body32;
    int body32_len, document_id;
    unsigned int title_size, body_size;

    title_size = strlen(title);
    body_size = strlen(body);

    /* DBに文書を格納し、その文書IDを取得する。 */
    db_add_document(env, title, title_size, body, body_size);
    document_id = db_get_document_id(env, title, title_size);

    /* documentのbodyについて、文字コードを変換する */
    if (!utf8toutf32(body, body_size, &body32, &body32_len)) {
      /* documentからposting_listを作成 */
      text_to_postings_lists(env, document_id, body32, body32_len,
                             env->token_len, &env->ii_buffer);
      env->ii_buffer_count++;
      free(body32);
    }
    env->indexed_count++;
    print_error("count:%d title: %s", env->indexed_count, title);
  }

  /* バッファに所定の文書数がたまったら、更新を行う */
  if (env->ii_buffer &&
      (env->ii_buffer_count > env->ii_buffer_update_threshold || !title)) {
    inverted_index_hash *p;

    print_time_diff();

    /* すべてのtokenについて、postingsを更新 */
    for (p = env->ii_buffer; p != NULL; p = p->hh.next) {
      update_postings(env, p);
    }
    free_inverted_index(env->ii_buffer);
    print_error("index flushed.");
    env->ii_buffer = NULL;
    env->ii_buffer_count = 0;

    print_time_diff();
  }
}

/**
 * アプリケーション環境を設定する。
 * @param[in] env アプリケーション環境を保存する構造体
 * @param[in] ii_buffer_update_threshold 転置索引のバッファをflushするしきい値
 * @param[in] enable_phrase_search フレーズ検索を有効にするかどうか
 * @param[in] db_path データベースのパス
 * @return エラーコード
 * @retval 0 成功
 */
static int
init_env(wiser_env *env,
         int ii_buffer_update_threshold, int enable_phrase_search,
         const char *db_path)
{
  int rc;
  memset(env, 0, sizeof(wiser_env));
  rc = init_database(env, db_path);
  if (!rc) {
    env->token_len = N_GRAM;
    env->ii_buffer_update_threshold = ii_buffer_update_threshold;
    env->enable_phrase_search = enable_phrase_search;
  }
  return rc;
}

/**
 * アプリケーション環境を破棄する
 * @param[in] env アプリケーション環境を保存する構造体
 */
static void
fin_env(wiser_env *env)
{
  fin_database(env);
}

/* あるアドレスtから始まる長さlのバイナリ列が、
   定数文字列cと一致するかどうか */
#define MEMSTRCMP(t,l,c) (l == (sizeof(c) - 1) && !memcmp(t, c, l))

/**
 * 全文検索を行う
 * @param[in] env アプリケーション環境を保存する構造体
 * @param[in] method postings listの圧縮方法
 * @param[in] method_size methodのバイト長
 */
static void
parse_compress_method(wiser_env *env, const char *method,
                      int method_size)
{
  if (method && method_size < 0) { method_size = strlen(method); }
  if (!method || !method_size
      || MEMSTRCMP(method, method_size, "golomb")) {
    env->compress = compress_golomb;
  } else if (MEMSTRCMP(method, method_size, "none")) {
    env->compress = compress_none;
  } else {
    print_error("invalid compress method(%.*s). use golomb instead.",
                method_size, method);
    env->compress = compress_golomb;
  }
  switch (env->compress) {
  case compress_none:
    db_replace_settings(env,
                        "compress_method", sizeof("compress_method") - 1,
                        "none", sizeof("none") - 1);
    break;
  case compress_golomb:
    db_replace_settings(env,
                        "compress_method", sizeof("compress_method") - 1,
                        "golomb", sizeof("golomb") - 1);
    break;
  }
}

/**
 * エントリポイント
 * @param[in] argc 引数の数
 * @param[in] argv 引数のポインタの配列
 */
int
main(int argc, char *argv[])
{
  wiser_env env;
  extern int optind;
  int max_index_count = -1; /* 無制限 */
  int ii_buffer_update_threshold = DEFAULT_II_BUFFER_UPDATE_THRESHOLD;
  int enable_phrase_search = TRUE;
  const char *compress_method_str = NULL, *wikipedia_dump_file = NULL,
              *query = NULL;
  /* オプション文字列の解析 */
  {
    int ch;
    extern int opterr;
    extern char *optarg;

    while ((ch = getopt(argc, argv, "c:x:q:m:t:s")) != -1) {
      switch (ch) {
      case 'c':
        compress_method_str = optarg;
        break;
      case 'x':
        wikipedia_dump_file = optarg;
        break;
      case 'q':
        query = optarg;
        break;
      case 'm':
        max_index_count = atoi(optarg);
        break;
      case 't':
        ii_buffer_update_threshold = atoi(optarg);
        break;
      case 's':
        enable_phrase_search = FALSE;
        break;
      }
    }
  }

  /* 解析したオプションを用いて実行 */
  if (argc != optind + 1) {
    printf(
      "usage: %s [options] db_file\n"
      "\n"
      "options:\n"
      "  -c compress_method            : compress method for postings list\n"
      "  -x wikipedia_dump_xml         : wikipedia dump xml path for indexing\n"
      "  -q search_query               : query for search\n"
      "  -m max_index_count            : max count for indexing document\n"
      "  -t ii_buffer_update_threshold : inverted index buffer merge threshold\n"
      "  -s                            : don't use tokens' positions for search\n"
      "\n"
      "compress_methods:\n"
      "  none   : don't compress.\n"
      "  golomb : Golomb-Rice coding(default).\n",
      argv[0]);
    return -1;
  }

  /* インデックス作成モードのときに、すでに既存のdbがあればエラーを出す */
  {
    struct stat st;
    if (wikipedia_dump_file && !stat(argv[optind], &st)) {
      printf("%s is already exists.\n", argv[optind]);
      return -2;
    }
  }

  {
    int rc = init_env(&env, ii_buffer_update_threshold, enable_phrase_search,
                  argv[optind]);
    if (!rc) {
      print_time_diff();

      /* Wikipediaの記事データを読み込む */
      if (wikipedia_dump_file) {
        parse_compress_method(&env, compress_method_str, -1);
        begin(&env);
        if (!load_wikipedia_dump(&env, wikipedia_dump_file, add_document,
                                 max_index_count)) {
          /* バッファをflushする */
          add_document(&env, NULL, NULL);
          commit(&env);
        } else {
          rollback(&env);
        }
      }

      /* 検索を行う */
      if (query) {
        int cm_size;
        const char *cm;
        db_get_settings(&env,
                        "compress_method", sizeof("compress_method") - 1,
                        &cm, &cm_size);
        parse_compress_method(&env, cm, cm_size);
        env.indexed_count = db_get_document_count(&env);
        search(&env, query);
      }
      fin_env(&env);

      print_time_diff();
    }
    return rc;
  }
}
