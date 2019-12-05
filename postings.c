#include <stdio.h>

#include "util.h"
#include "database.h"

/**
 * ポスティングリスト(バイト列)からポスティングリストを復元する。
 * @param[in] postings_e ポスティングリスト(バイト列)
 * @param[in] postings_e_size ポスティングリスト(バイト列)のエントリ数
 * @param[out] postings 復元されたポスティングリスト
 * @param[out] postings_len 復元されたポスティングリストのエントリ数
 * @retval 0 成功
 *
 */
static int
decode_postings_none(const char *postings_e, int postings_e_size,
                     postings_list **postings, int *postings_len)
{
  const int *p, *pend;

  *postings = NULL;
  *postings_len = 0;
  for (p = (const int *)postings_e,
       pend = (const int *)(postings_e + postings_e_size); p < pend;) {
    postings_list *pl;
    int document_id, positions_count;

    document_id = *(p++);
    positions_count = *(p++);
    if ((pl = malloc(sizeof(postings_list)))) {
      int i;
      pl->document_id = document_id;
      pl->positions_count = positions_count;
      utarray_new(pl->positions, &ut_int_icd);
      LL_APPEND(*postings, pl);
      (*postings_len)++;

      /* decode positions */
      for (i = 0; i < positions_count; i++) {
        utarray_push_back(pl->positions, p);
        p++;
      }
    } else {
      p += positions_count;
    }
  }
  return 0;
}

/**
 * ポスティングリストをバイト列に変換する。
 * @param[in] postings ポスティングリスト
 * @param[in] postings_len ポスティングリストのエントリ数
 * @param[out] postings_e 変換されたポスティングリスト
 * @retval 0 成功
 */
static int
encode_postings_none(const postings_list *postings,
                     const int postings_len,
                     buffer *postings_e)
{
  const postings_list *p;
  LL_FOREACH(postings, p) {
    int *pos = NULL;
    append_buffer(postings_e, (void *)&p->document_id, sizeof(int));
    append_buffer(postings_e, (void *)&p->positions_count, sizeof(int));
    while ((pos = (int *)utarray_next(p->positions, pos))) {
      append_buffer(postings_e, (void *)pos, sizeof(int));
    }
  }
  return 0;
}

/**
 * データの指定位置から、1ビットを読みだす。
 * @param[in,out] buf データの先頭
 * @param[in] buf_end データの終端
 * @param[in,out] bit 読み出す際にマスクするbit
 * @return 読み出したビットの値
 */
static inline int
read_bit(const char **buf, const char *buf_end, unsigned char *bit)
{
  int r;
  if (*buf >= buf_end) { return -1; }
  r = (**buf & *bit) ? 1 : 0;
  *bit >>= 1;
  if (!*bit) {
    *bit = 0x80;
    (*buf)++;
  }
  return r;
}

/**
 * Golomb符号のmパラメータから、符号化・復号過程で必要となる
 * b/tパラメータを計算する。
 * @param[in] m Golomb符号のmパラメータ
 * @param[out] b Golomb符号のbパラメータ。ceil(log2(m))
 * @param[out] t pow2(b) - m
 */
static void
calc_golomb_params(int m, int *b, int *t)
{
  int l;
  assert(m > 0);
  for (*b = 0, l = 1; m > l ; (*b)++, l <<= 1) {}
  *t = l - m;
}

/**
 * Golomb符号で1つの数値を復号する。
 * @param[in] m Golomb符号のmパラメータ
 * @param[in] b Golomb符号のbパラメータ。ceil(log2(m))
 * @param[in] t pow2(b) - m
 * @param[in] buf 復号の対象となるデータ
 * @param[in] buf_end 復号の対象となるデータの終端
 * @param[in] bit 復号の対象となるデータの開始ビット
 * @return 復号された値
 */
static inline int
golomb_decoding(int m, int b, int t,
                const char **buf, const char *buf_end, unsigned char *bit)
{
  int n = 0;

  /* decode (n / m) with unary code */
  while (read_bit(buf, buf_end, bit) == 1) {
    n += m;
  }
  /* decode (n % m) */
  if (m > 1) {
    int i, r = 0;
    for (i = 0; i < b - 1; i++) {
      int z = read_bit(buf, buf_end, bit);
      if (z == -1) { print_error("invalid golomb code"); break; }
      r = (r << 1) | z;
    }
    if (r >= t) {
      int z = read_bit(buf, buf_end, bit);
      if (z == -1) {
        print_error("invalid golomb code");
      } else {
        r = (r << 1) | z;
        r -= t;
      }
    }
    n += r;
  }
  return n;
}

/**
 * Golomb符号で1つの数値を符号化する。
 * @param[in] m Golomb符号のmパラメータ
 * @param[in] b Golomb符号のbパラメータ。ceil(log2(m))
 * @param[in] t pow2(b) - m
 * @param[in] n 符号化する値
 * @param[in] buf 符号化したデータ
 */
static inline void
golomb_encoding(int m, int b, int t, int n, buffer *buf)
{
  int i;
  /* encode (n / m) with unary code */
  for (i = n / m; i; i--) { append_buffer_bit(buf, 1); }
  append_buffer_bit(buf, 0);
  /* encode (n % m) */
  if (m > 1) {
    int r = n % m;
    if (r < t) {
      for (i = 1 << (b - 2); i; i >>= 1) {
        append_buffer_bit(buf, r & i);
      }
    } else {
      r += t;
      for (i = 1 << (b - 1); i; i >>= 1) {
        append_buffer_bit(buf, r & i);
      }
    }
  }
}

/**
 * Golomb符号化されたポスティングリストを復号する。
 * @param[in] postings_e Golomb符号化されたポスティングリスト
 * @param[in] postings_e_size Golomb符号化されたポスティングリストの
                              エントリ数
 * @param[out] postings 復号されたポスティングリスト
 * @param[out] postings_len 復号されたポスティングリストのエントリ数
 * @retval 0 成功
 */
static int
decode_postings_golomb(const char *postings_e, int postings_e_size,
                       postings_list **postings, int *postings_len)
{
  const char *pend;
  unsigned char bit;

  pend = postings_e + postings_e_size;
  bit = 0x80;
  *postings = NULL;
  *postings_len = 0;
  {
    int i, docs_count;
    postings_list *pl;
    {
      int m, b, t, pre_document_id = 0;

      docs_count = *((int *)postings_e);
      postings_e += sizeof(int);
      m = *((int *)postings_e);
      postings_e += sizeof(int);
      calc_golomb_params(m, &b, &t);
      for (i = 0; i < docs_count; i++) {
        int gap = golomb_decoding(m, b, t, &postings_e, pend, &bit);
        if ((pl = malloc(sizeof(postings_list)))) {
          pl->document_id = pre_document_id + gap + 1;
          utarray_new(pl->positions, &ut_int_icd);
          LL_APPEND(*postings, pl);
          (*postings_len)++;
          pre_document_id = pl->document_id;
        } else {
          print_error("memory allocation failed.");
        }
      }
    }
    if (bit != 0x80) { postings_e++; bit = 0x80; }
    for (i = 0, pl = *postings; i < docs_count; i++, pl = pl->next) {
      int j, mp, bp, tp, position = -1;

      pl->positions_count = *((int *)postings_e);
      postings_e += sizeof(int);
      mp = *((int *)postings_e);
      postings_e += sizeof(int);
      calc_golomb_params(mp, &bp, &tp);
      for (j = 0; j < pl->positions_count; j++) {
        int gap = golomb_decoding(mp, bp, tp, &postings_e, pend, &bit);
        position += gap + 1;
        utarray_push_back(pl->positions, &position);
      }
      if (bit != 0x80) { postings_e++; bit = 0x80; }
    }
  }
  return 0;
}

/**
 * ポスティングリストをGolomb符号化する。
 * @param[in] documents_count 総ドキュメント数
 * @param[in] postings 符号化するポスティングリスト
 * @param[in] postings_len 符号化するポスティングリストのエントリ数
 * @param[in] postings_e 符号化されたポスティングリスト
 * @retval 0 成功
 */
static int
encode_postings_golomb(int documents_count,
                       const postings_list *postings, const int postings_len,
                       buffer *postings_e)
{
  const postings_list *p;

  append_buffer(postings_e, &postings_len, sizeof(int));
  if (postings && postings_len) {
    int m, b, t;
    m = documents_count / postings_len;
    append_buffer(postings_e, &m, sizeof(int));
    calc_golomb_params(m, &b, &t);
    {
      int pre_document_id = 0;

      LL_FOREACH(postings, p) {
        int gap = p->document_id - pre_document_id - 1;
        golomb_encoding(m, b, t, gap, postings_e);
        pre_document_id = p->document_id;
      }
    }
    append_buffer(postings_e, NULL, 0);
  }
  LL_FOREACH(postings, p) {
    append_buffer(postings_e, &p->positions_count, sizeof(int));
    if (p->positions && p->positions_count) {
      const int *pp;
      int mp, bp, tp, pre_position = -1;

      pp = (const int *)utarray_back(p->positions);
      mp = (*pp + 1) / p->positions_count;
      calc_golomb_params(mp, &bp, &tp);
      append_buffer(postings_e, &mp, sizeof(int));
      pp = NULL;
      while ((pp = (const int *)utarray_next(p->positions, pp))) {
        int gap = *pp - pre_position - 1;
        golomb_encoding(mp, bp, tp, gap, postings_e);
        pre_position = *pp;
      }
      append_buffer(postings_e, NULL, 0);
    }
  }
  return 0;
}

/**
 * ポスティングリストを復元または復号する。
 * @param[in] env アプリケーション環境
 * @param[in] postings_e 復元または復号するポスティングリスト
 * @param[in] postings_e_size 復元または復号するポスティングリストのバイト数
 * @param[out] postings 復元または復号されたポスティングリスト
 * @param[out] postings_len 復元または復号されたポスティングリストのエントリ数
 * @retval 0 成功
 */
static int
decode_postings(const wiser_env *env,
                const char *postings_e, int postings_e_size,
                postings_list **postings, int *postings_len)
{
  switch (env->compress) {
  case compress_none:
    return decode_postings_none(postings_e, postings_e_size,
                                postings, postings_len);
  case compress_golomb:
    return decode_postings_golomb(postings_e, postings_e_size,
                                  postings, postings_len);
  default:
    abort();
  }
}

/**
 * ポスティングリストを変換または符号化する。
 * @param[in] env アプリケーション環境
 * @param[in] postings 変換または符号化するポスティングリスト
 * @param[in] postings_len 変換または符号化するポスティングリストのエントリ数
 * @param[out] postings_e 変換または符号化されたポスティングリスト
 * @retval 0 成功
 */
static int
encode_postings(const wiser_env *env,
                const postings_list *postings, const int postings_len,
                buffer *postings_e)
{
  switch (env->compress) {
  case compress_none:
    return encode_postings_none(postings, postings_len, postings_e);
  case compress_golomb:
    return encode_postings_golomb(db_get_document_count(env),
                                  postings, postings_len, postings_e);
  default:
    abort();
  }
}

/**
 * DBから、特定のトークンに紐づいたポスティングリストを取得する。
 * @param[in] env アプリケーション環境
 * @param[in] token_id 取得するtokenのID
 * @param[out] postings 取得したポスティングリスト
 * @param[out] postings_len 取得したポスティングリストのエントリ数
 * @retval 0 成功
 * @retval -1 失敗
 */
int
fetch_postings(const wiser_env *env, const int token_id,
               postings_list **postings, int *postings_len)
{
  char *postings_e;
  int postings_e_size, docs_count, rc;

  rc = db_get_postings(env, token_id, &docs_count, (void **)&postings_e,
                       &postings_e_size);
  if (!rc && postings_e_size) {
    /* 空ではない場合、復号する */
    int decoded_len;
    if (decode_postings(env, postings_e, postings_e_size, postings,
                        &decoded_len)) {
      print_error("postings list decode error");
      rc = -1;
    } else if (docs_count != decoded_len) {
      print_error("postings list decode error: stored:%d decoded:%d.\n",
                  *postings_len, decoded_len);
      rc = -1;
    }
    if (postings_len) { *postings_len = decoded_len; }
  } else {
    *postings = NULL;
    if (postings_len) { *postings_len = 0; }
  }
  return rc;
}

/**
 * 二つのポスティングリストをマージしたポスティングリストを取得する。
 * @param[in] pa マージ対象のポスティングリスト
 * @param[in] pb マージ対象のポスティングリスト
 *
 * @return マージされたポスティングリスト
 *
 * @attention baseとto_be_addedどちらも破壊される。また、baseとto_be_addedに
 *            同一のdocument IDが含まれる場合には、動作が保障されない。
 */
static postings_list *
merge_postings(postings_list *pa, postings_list *pb)
{
  postings_list *ret = NULL, *p;
  /* baseとto_be_addedを走査して、小さい順にリストをつなげていく */
  while (pa || pb) {
    postings_list *e;
    if (!pb || (pa && pa->document_id <= pb->document_id)) {
      e = pa;
      pa = pa->next;
    } else if (!pa || pa->document_id >= pb->document_id) {
      e = pb;
      pb = pb->next;
    } else {
      abort();
    }
    e->next = NULL;
    if (!ret) {
      ret = e;
    } else {
      p->next = e;
    }
    p = e;
  }
  return ret;
}

/**
 * データベース上のポスティングリストと更新用の転置インデックスをマージし保存する。
 * @param[in] env アプリケーション環境
 * @param[in] p ポスティングリストを含んだinverted_indexのエントリ
 */
void
update_postings(const wiser_env *env, inverted_index_value *p)
{
  int old_postings_len;
  postings_list *old_postings;

  if (!fetch_postings(env, p->token_id, &old_postings,
                      &old_postings_len)) {
    buffer *buf;
    if (old_postings_len) {
      p->postings_list = merge_postings(old_postings, p->postings_list);
      p->docs_count += old_postings_len;
    }
    if ((buf = alloc_buffer())) {
      encode_postings(env, p->postings_list, p->docs_count, buf);
      db_update_postings(env, p->token_id, p->docs_count,
                         BUFFER_PTR(buf), BUFFER_SIZE(buf));
      free_buffer(buf);
    }
  } else {
    print_error("cannot fetch old postings list of token(%d) for update.",
                p->token_id);
  }
}

/**
 * 二つのinverted indexをマージし、片方を解放する。
 * @param[in] base マージされて要素が増えるinverted index
 * @param[in] to_be_added マージされて解放されるinverted index
 *
 */
void
merge_inverted_index(inverted_index_hash *base,
                     inverted_index_hash *to_be_added)
{
  inverted_index_value *p, *temp;

  HASH_ITER(hh, to_be_added, p, temp) {
    inverted_index_value *t;
    HASH_DEL(to_be_added, p);
    HASH_FIND_INT(base, &p->token_id, t);
    if (t) {
      t->postings_list = merge_postings(t->postings_list, p->postings_list);
      t->docs_count += p->docs_count;
      free(p);
    } else {
      HASH_ADD_INT(base, token_id, p);
    }
  }
}

/**
 * ポスティングリストの内容を表示する。デバッグ用に用いる。
 * @param[in] postings ダンプするポスティングリスト
 */
void
dump_postings_list(const postings_list *postings)
{
  const postings_list *pl;
  LL_FOREACH(postings, pl) {
    printf("doc_id %d (", pl->document_id);
    if (pl->positions) {
      const int *p = NULL;
      while ((p = (const int *)utarray_next(pl->positions, p))) {
        printf("%d ", *p);
      }
    }
    printf(")\n");
  }
}

/**
 * ポスティングリストを開放する。
 * @param[in] pl 開放するポスティングリストの先頭エントリ
 */
void
free_postings_list(postings_list *pl)
{
  postings_list *a, *a2;
  LL_FOREACH_SAFE(pl, a, a2) {
    if (a->positions) {
      utarray_free(a->positions);
    }
    LL_DELETE(pl, a);
    free(a);
  }
}

/**
 * 転置インデックスの中身を出力する。
 * @param[in] ii 転置インデックスへのポインタ
 */
void
dump_inverted_index(wiser_env *env, inverted_index_hash *ii)
{
  inverted_index_value *it;
  for (it = ii; it != NULL; it = it->hh.next) {
    int token_len;
    const char *token;

    if (it->token_id) {
      db_get_token(env, it->token_id, &token, &token_len);
      printf("TOKEN %d.%.*s(%d):\n", it->token_id, token_len, token,
             it->docs_count);
    } else {
      puts("TOKEN NONE:");
    }
    if (it->postings_list) {
      printf("POSTINGS: [\n  ");
      dump_postings_list(it->postings_list);
      puts("]");
    }
  }
}

/**
 * 転置インデックスを開放する。
 * @param[in] ii 転置インデックスへのポインタ
 */
void
free_inverted_index(inverted_index_hash *ii)
{
  inverted_index_value *cur;
  while (ii) {
    cur = ii;
    HASH_DEL(ii, cur);
    if (cur->postings_list) {
      free_postings_list(cur->postings_list);
    }
    free(cur);
  }
}
