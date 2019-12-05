#include <math.h>
#include <stdio.h>

#include "util.h"
#include "token.h"
#include "database.h"
#include "postings.h"

/* inverted_index_hash/value型とpostings_list型を検索にも流用する */
typedef inverted_index_hash query_token_hash;
typedef inverted_index_value query_token_value;
typedef postings_list token_positions_list;

typedef struct {
  token_positions_list *documents; /* 文書IDの列 */
  token_positions_list *current;   /* 現在参照している文書ID */
} doc_search_cursor;

typedef struct {
  const UT_array *positions; /* 位置情報 */
  int base;                  /* クエリ内でのトークンの位置 */
  int *current;              /* 現在参照している位置情報 */
} phrase_search_cursor;

typedef struct {
  int document_id;           /* 検索された文書ID */
  double score;              /* 検索スコア */
  UT_hash_handle hh;         /* ハッシュの要素 */
} search_results;

/**
 * ふたつのtokenについて、それぞれが出現する文書数を比較する
 * @param[in] a トークンのエントリ
 * @param[in] b トークンのエントリ
 * @return 文書数の大小関係
 */
static int
query_token_value_docs_count_desc_sort(query_token_value *a,
                                       query_token_value *b)
{
  return b->docs_count - a->docs_count;
}

/**
 * 検索結果の２エントリを、スコアで比較する
 * @param[in] a 検索結果のエントリ
 * @param[in] b 検索結果のエントリ
 * @return スコアの大小関係
 */
static int
search_results_score_desc_sort(search_results *a, search_results *b)
{
  return (b->score > a->score) ? 1 : (b->score < a->score) ? -1 : 0;
}

/**
 * 検索結果に文書を追加する。
 * @param[in] results 検索結果へのポインタ
 * @param[in] document_id 追加する文書のID
 * @param[in] score スコア
 */
static void
add_search_result(search_results **results, const int document_id,
                  const double score)
{
  search_results *r;
  if (*results) {
    HASH_FIND_INT(*results, &document_id, r);
  } else {
    r = NULL;
  }
  if (!r) {
    if ((r = malloc(sizeof(search_results)))) {
      r->document_id = document_id;
      r->score = 0;
      HASH_ADD_INT(*results, document_id, r);
    }
  }
  if (r) {
    r->score += score;
  }
}

/**
 * フレーズ検索を行う。
 * @param[in] query_tokens 検索クエリから作ったトークン情報
 * @param[in] doc_cursors 文書検索でのカーソル群
 * @return 検索されたフレーズ数
 */
static int
search_phrase(const query_token_hash *query_tokens,
              doc_search_cursor *doc_cursors)
{
  int n_positions = 0;
  const query_token_value *qt;
  phrase_search_cursor *cursors;

  /* クエリの総トークン数を取得する。 */
  for (qt = query_tokens; qt; qt = qt->hh.next) {
    n_positions += qt->positions_count;
  }

  if ((cursors = (phrase_search_cursor *)malloc(sizeof(
                   phrase_search_cursor) * n_positions))) {
    int i, phrase_count = 0;
    phrase_search_cursor *cur;
    /* カーソルを初期化する */
    for (i = 0, cur = cursors, qt = query_tokens; qt;
         i++, qt = qt->hh.next) {
      int *pos = NULL;
      while ((pos = (int *)utarray_next(qt->postings_list->positions,
                                        pos))) {
        cur->base = *pos;
        cur->positions = doc_cursors[i].current->positions;
        cur->current = (int *)utarray_front(cur->positions);
        cur++;
      }
    }
    /* フレーズ検索 */
    while (cursors[0].current) {
      int rel_position, next_rel_position;
      rel_position = next_rel_position = *cursors[0].current -
                                         cursors[0].base;
      /* A以外のpositionについて、Aの相対position以上になるまで読み進める */
      for (cur = cursors + 1, i = 1; i < n_positions; cur++, i++) {
        for (; cur->current
             && (*cur->current - cur->base) < rel_position;
             cur->current = (int *)utarray_next(cur->positions, cur->current)) {}
        if (!cur->current) { goto exit; }

        /* A以外のpositionについて、Aと相対positionが違うならループを抜ける */
        if ((*cur->current - cur->base) != rel_position) {
          next_rel_position = *cur->current - cur->base;
          break;
        }
      }
      if (next_rel_position > rel_position) {
        /* Aのpositionがnext_rel_position以上になるまで読み進める */
        while (cursors[0].current &&
               (*cursors[0].current - cursors[0].base) < next_rel_position) {
          cursors[0].current = (int *)utarray_next(
                                 cursors[0].positions, cursors[0].current);
        }
      } else {
        /* フレーズが一致 */
        phrase_count++;
        cursors->current = (int *)utarray_next(
                             cursors->positions, cursors->current);
      }
    }
exit:
    free(cursors);
    return phrase_count;
  }
  return 0;
}

/**
 * TF-IDFでスコアを計算する
 * @param[in] query_tokens 検索クエリ
 * @param[in] doc_cursors 文書検索でのカーソル群
 * @param[in] n_query_tokens 検索クエリでのトークン数
 * @param[in] indexed_count インデックス化された全文書数
 * @return スコア
 */
static double
calc_tf_idf(
  const query_token_hash *query_tokens,
  doc_search_cursor *doc_cursors, const int n_query_tokens,
  const int indexed_count)
{
  int i;
  const query_token_value *qt;
  doc_search_cursor *dcur;
  double score = 0;
  for (qt = query_tokens, dcur = doc_cursors, i = 0;
       i < n_query_tokens;
       qt = qt->hh.next, dcur++, i++) {
    double idf = log2((double)indexed_count / qt->docs_count);
    score += (double)dcur->current->positions_count * idf;
  }
  return score;
}

/**
 * token positions listを開放する。
 * @param[in] pl 開放するtoken positions listの先頭エントリ
 */
void
free_token_positions_list(token_positions_list *list)
{
  free_postings_list((postings_list *)list);
}

/**
 * 文書検索を行う。
 * @param[in] env アプリケーション環境を保存する構造体
 * @param[in,out] results 検索結果
 * @param[in] tokens 検索クエリから作ったトークン情報
 */
void
search_docs(wiser_env *env, search_results **results,
            query_token_hash *tokens)
{
  int n_tokens;
  doc_search_cursor *cursors;

  if (!tokens) { return; }

  /* tokensについて、docs_countの昇順にソート */
  HASH_SORT(tokens, query_token_value_docs_count_desc_sort);

  /* 初期化 */
  n_tokens = HASH_COUNT(tokens);
  if (n_tokens &&
      (cursors = (doc_search_cursor *)calloc(
                   sizeof(doc_search_cursor), n_tokens))) {
    int i;
    doc_search_cursor *cur;
    query_token_value *token;
    for (i = 0, token = tokens; token; i++, token = token->hh.next) {
      if (!token->token_id) {
        /* 当該tokenがインデックス作成時に1回も出現していない */
        goto exit;
      }
      if (fetch_postings(env, token->token_id,
                         &cursors[i].documents, NULL)) {
        print_error("decode postings error!: %d\n", token->token_id);
        goto exit;
      }
      if (!cursors[i].documents) {
        /* tokenはあるが、postingsが空。更新・削除の結果 */
        goto exit;
      }
      cursors[i].current = cursors[i].documents;
    }
    while (cursors[0].current) {
      int doc_id, next_doc_id = 0;
      /* 最小のドキュメント数を持つtokenをAと呼ぶ。 */
      doc_id = cursors[0].current->document_id;
      /* A以外のtokenについて、Aのdocument_id以上になるまで読み進める */
      for (cur = cursors + 1, i = 1; i < n_tokens; cur++, i++) {
        while (cur->current && cur->current->document_id < doc_id) {
          cur->current = cur->current->next;
        }
        if (!cur->current) { goto exit; }
        /* A以外のtokenについて、Aとdocument_idが違うならnext_doc_idを設定 */
        if (cur->current->document_id != doc_id) {
          next_doc_id = cur->current->document_id;
          break;
        }
      }
      if (next_doc_id > 0) {
        /* Aのdocument_idが、next_doc_id以上になるまで読み進める */
        while (cursors[0].current
               && cursors[0].current->document_id < next_doc_id) {
          cursors[0].current = cursors[0].current->next;
        }
      } else {
        int phrase_count = -1;
        if (env->enable_phrase_search) {
          phrase_count = search_phrase(tokens, cursors);
        }
        if (phrase_count) {
          double score = calc_tf_idf(tokens, cursors, n_tokens,
                                     env->indexed_count);
          add_search_result(results, doc_id, score);
        }
        cursors[0].current = cursors[0].current->next;
      }
    }
exit:
    for (i = 0; i < n_tokens; i++) {
      if (cursors[i].documents) {
        free_token_positions_list(cursors[i].documents);
      }
    }
    free(cursors);
  }
  free_inverted_index(tokens);

  HASH_SORT(*results, search_results_score_desc_sort);
}

/**
 * クエリ文字列から、トークンの情報を取り出す
 * @param[in] env 環境
 * @param[in] text クエリ文字列
 * @param[in] text_len クエリ文字列の文字長
 * @param[in] n 何-gramか
 * @param[in,out] query_tokens トークンIDごとに位置情報列を保存する連想配列
 *                             NULLを指すポインタを渡すと新規作成
 * @retval 0 成功
 * @retval -1 失敗
 */
int
split_query_to_tokens(wiser_env *env,
                      const UTF32Char *text,
                      const unsigned int text_len,
                      const int n, query_token_hash **query_tokens)
{
  return text_to_postings_lists(env,
                                0, /* document_id は 0とする */
                                text, text_len, n,
                                (inverted_index_hash **)query_tokens);
}

/**
 * 検索結果を表示する
 * @param[in] env アプリケーション環境を保存する構造体
 * @param[in] results 検索結果
 */
void
print_search_results(wiser_env *env, search_results *results)
{
  int num_search_results;

  if (!results) { return; }
  num_search_results = HASH_COUNT(results);

  while (results) {
    int title_len;
    const char *title;
    search_results *r;

    r = results;
    HASH_DEL(results, r);
    db_get_document_title(env, r->document_id, &title, &title_len);
    printf("document_id: %d title: %.*s score: %lf\n",
           r->document_id, title_len, title, r->score);
    free(r);
  }

  printf("Total %u documents are found!\n", num_search_results);
}

/**
 * 全文検索を行う
 * @param[in] env アプリケーション環境を保存する構造体
 * @param[in] query 検索クエリ
 */
void
search(wiser_env *env, const char *query)
{
  int query32_len;
  UTF32Char *query32;

  if (!utf8toutf32(query, strlen(query), &query32, &query32_len)) {
    search_results *results = NULL;

    if (query32_len < env->token_len) {
      print_error("too short query.");
    } else {
      query_token_hash *query_tokens = NULL;
      split_query_to_tokens(
        env, query32, query32_len, env->token_len, &query_tokens);
      search_docs(env, &results, query_tokens);
    }

    print_search_results(env, results);

    free(query32);
  }
}
