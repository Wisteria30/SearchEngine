#include "util.h"
#include "token.h"
#include "postings.h"
#include "database.h"

#include <stdio.h>

/**
 * 渡されたUTF32の文字がインデックス対象の文字でないかどうかチェックする
 * @param[in] ustr 入力文字(UTF-32)
 * @return 空白であるかどうか
 * @retval 0 空白でない
 * @retval 1 空白
 */
static int
wiser_is_ignored_char(const UTF32Char ustr)
{
  switch (ustr) {
  case ' ': case '\f': case '\n': case '\r': case '\t': case '\v':
  case '!': case '"': case '#': case '$': case '%': case '&':
  case '\'': case '(': case ')': case '*': case '+': case ',':
  case '-': case '.': case '/':
  case ':': case ';': case '<': case '=': case '>': case '?': case '@':
  case '[': case '\\': case ']': case '^': case '_': case '`':
  case '{': case '|': case '}': case '~':
  case 0x3000: /* 全角スペース */
  case 0x3001: /* 、 */
  case 0x3002: /* 。 */
  case 0xFF08: /* （ */
  case 0xFF09: /* ） */
    return 1;
  default:
    return 0;
  }
}

/**
 * 渡された文字列を、N-gramに分解する。
 * @param[in] ustr 入力文字列(UTF-8)
 * @param[in] ustr_end 入力文字列の終端文字位置
 * @param[in] n 何-gramか。1以上の値を推奨。
 * @param[out] start トークンの開始位置
 * @return 分解されたトークンの文字長
 */
static int
ngram_next(const UTF32Char *ustr, const UTF32Char *ustr_end,
           unsigned int n, const UTF32Char **start)
{
  int i;
  const UTF32Char *p;

  /* 冒頭にある空白などを読み飛ばす */
  for (; ustr < ustr_end && wiser_is_ignored_char(*ustr); ustr++) {
  }

  /* インデックス対象外の文字か終端に達するまで、最大n文字のトークンを取り出す。 */
  for (i = 0, p = ustr; i < n && p < ustr_end
       && !wiser_is_ignored_char(*p); i++, p++) {
  }

  *start = ustr;
  return p - ustr;
}

/**
 * inverted_index_valueを確保・初期化する
 * @param[in] token_id トークンID
 * @param[in] docs_count トークンが存在する文書数
 * @return 作成されたinverted_index_value
 */
static inverted_index_value *
create_new_inverted_index(int token_id, int docs_count)
{
  inverted_index_value *ii_entry;

  ii_entry = malloc(sizeof(inverted_index_value));
  if (!ii_entry) {
    print_error("cannot allocate memory for an inverted index.");
    return NULL;
  }
  ii_entry->positions_count = 0;
  ii_entry->postings_list = NULL;
  ii_entry->token_id = token_id;
  ii_entry->docs_count = docs_count;

  return ii_entry;
}

/**
 * postings_listを確保・初期化する
 * @param[in] document_id ドキュメントID
 * @return 作成されたpostings_list
 */
static postings_list *
create_new_postings_list(int document_id)
{
  postings_list *pl;

  pl = malloc(sizeof(postings_list));
  if (!pl) {
    print_error("cannot allocate memory for a postings list.");
    return NULL;
  }
  pl->document_id = document_id;
  pl->positions_count = 1;
  utarray_new(pl->positions, &ut_int_icd);

  return pl;
}

/**
 * 渡されたトークン文字列から、postings listを作成。
 * @param[in] env 環境
 * @param[in] document_id ドキュメントID
 * @param[in] token トークン(UTF-8)
 * @param[in] token_size トークンのバイト長
 * @param[in] position トークンの出現位置
 * @param[in,out] postings postings listの配列
 * @retval 0 成功
 * @retval -1 失敗
 */
int
token_to_postings_list(wiser_env *env,
                       const int document_id, const char *token,
                       const unsigned int token_size,
                       const int position,
                       inverted_index_hash **postings)
{
  postings_list *pl;
  inverted_index_value *ii_entry;
  int token_id, token_docs_count;

  token_id = db_get_token_id(
               env, token, token_size, document_id, &token_docs_count);
  if (*postings) {
    HASH_FIND_INT(*postings, &token_id, ii_entry);
  } else {
    ii_entry = NULL;
  }
  if (ii_entry) {
    pl = ii_entry->postings_list;
    pl->positions_count++;
  } else {
    ii_entry = create_new_inverted_index(token_id,
                                         document_id ? 1 : token_docs_count);
    if (!ii_entry) { return -1; }
    HASH_ADD_INT(*postings, token_id, ii_entry);

    pl = create_new_postings_list(document_id);
    if (!pl) { return -1; }
    LL_APPEND(ii_entry->postings_list, pl);
  }
  /* 位置情報を保存する */
  utarray_push_back(pl->positions, &position);
  ii_entry->positions_count++;
  return 0;
}

/**
 * 渡された文字列から、postings listを作成。
 * @param[in] env 環境
 * @param[in] document_id ドキュメントID。0の場合は、検索キーワードを対象とする。
 * @param[in] text 入力文字列
 * @param[in] text_len 入力文字列の文字長
 * @param[in] n 何-gramか
 * @param[in,out] postings ミニ転置インデックス。NULLを指すポインタを渡すと新規作成
 * @retval 0 成功
 * @retval -1 失敗
 */
int
text_to_postings_lists(wiser_env *env,
                       const int document_id, const UTF32Char *text,
                       const unsigned int text_len,
                       const int n, inverted_index_hash **postings)
{
  /* FIXME: now same document update is broken. */
  int t_len, position = 0;
  const UTF32Char *t = text, *text_end = text + text_len;

  inverted_index_hash *buffer_postings = NULL;

  for (; (t_len = ngram_next(t, text_end, n, &t)); t++, position++) {
    /* 検索の場合は、最後のN-gramに満たない端文字のトークンを使わない */
    if (t_len >= n || document_id) {
      int retval, t_8_size;
      char t_8[n * MAX_UTF8_SIZE];

      utf32toutf8(t, t_len, t_8, &t_8_size);

      retval = token_to_postings_list(env, document_id, t_8, t_8_size,
                                      position, &buffer_postings);
      if (retval) { return retval; }
    }
  }

  if (*postings) {
    merge_inverted_index(*postings, buffer_postings);
  } else {
    *postings = buffer_postings;
  }

  return 0;
}

/**
 * tokenをダンプする。
 * @param[in] env 環境
 * @param[in] token_id トークンID
 */
void
dump_token(wiser_env *env, int token_id)
{
  int token_len;
  const char *token;

  db_get_token(env, token_id, &token, &token_len);
  printf("token: %.*s (id: %d)\n", token_len, token, token_id);
}
