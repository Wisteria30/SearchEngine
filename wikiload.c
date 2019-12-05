#include <stdio.h>
#include <errno.h>

#include <expat.h>
#include <utstring.h>

#include "util.h"
#include "wikiload.h"

/* Wikipediaの記事XMLタグに存在する部分 */
typedef enum {
  IN_DOCUMENT,          /* 下記以外の状態 */
  IN_PAGE,              /* <page>タグの中 */
  IN_PAGE_TITLE,        /* <page>タグの中の<title>タグの中 */
  IN_PAGE_ID,           /* <page>タグの中の<id>タグの中 */
  IN_PAGE_REVISION,     /* <page>タグの中の<revision>タグの中 */
  IN_PAGE_REVISION_TEXT /* <page>タグの中の<revision>タグの中の<text>タグの中 */
} wikipedia_status;

/* Wikipediaパーサで使う変数 */
typedef struct {
  wiser_env *env;             /* 環境 */
  wikipedia_status status;    /* 記事XMLタグのどの部分を読んでいるか */
  UT_string *title;           /* タイトルを一時保存する領域 */
  UT_string *body;            /* 本文を一時保存する領域 */
  int article_count;          /* 解析した記事の総数 */
  int max_article_count;      /* 解析する記事の最大数 */
  add_document_callback func; /* 解析後のドキュメントを渡す関数 */
} wikipedia_parser;

/**
 * XMLタグの開始時に呼ばれる関数
 * @param[in] user_data Wikipediaパーサの環境
 * @param[in] el XMLタグ名
 * @param[in] attr XMLの属性たち
 */
static void XMLCALL
start(void *user_data, const XML_Char *el, const XML_Char *attr[])
{
  wikipedia_parser *p = (wikipedia_parser *)user_data;
  switch (p->status) {
  case IN_DOCUMENT:
    if (!strcmp(el, "page")) {
      p->status = IN_PAGE;
    }
    break;
  case IN_PAGE:
    if (!strcmp(el, "title")) {
      p->status = IN_PAGE_TITLE;
      utstring_new(p->title);
    } else if (!strcmp(el, "id")) {
      p->status = IN_PAGE_ID;
    } else if (!strcmp(el, "revision")) {
      p->status = IN_PAGE_REVISION;
    }
    break;
  case IN_PAGE_TITLE:
  case IN_PAGE_ID:
    break;
  case IN_PAGE_REVISION:
    if (!strcmp(el, "text")) {
      p->status = IN_PAGE_REVISION_TEXT;
      utstring_new(p->body);
    }
    break;
  case IN_PAGE_REVISION_TEXT:
    break;
  }
}

/**
 * XMLタグの終了時に呼ばれる関数
 * @param[in] user_data Wikipediaパーサの環境
 * @param[in] el XMLタグ名
 */
static void XMLCALL
end(void *user_data, const XML_Char *el)
{
  wikipedia_parser *p = (wikipedia_parser *)user_data;
  switch (p->status) {
  case IN_DOCUMENT:
    break;
  case IN_PAGE:
    if (!strcmp(el, "page")) {
      p->status = IN_DOCUMENT;
    }
    break;
  case IN_PAGE_TITLE:
    if (!strcmp(el, "title")) {
      p->status = IN_PAGE;
    }
    break;
  case IN_PAGE_ID:
    if (!strcmp(el, "id")) {
      p->status = IN_PAGE;
    }
    break;
  case IN_PAGE_REVISION:
    if (!strcmp(el, "revision")) {
      p->status = IN_PAGE;
    }
    break;
  case IN_PAGE_REVISION_TEXT:
    if (!strcmp(el, "text")) {
      p->status = IN_PAGE_REVISION;
      if (p->max_article_count < 0 ||
          p->article_count < p->max_article_count) {
        p->func(p->env, utstring_body(p->title), utstring_body(p->body));
      }
      utstring_free(p->title);
      utstring_free(p->body);
      p->title = NULL;
      p->body = NULL;
      p->article_count++;
    }
    break;
  }
}

/**
 * XMLのelement dataを解析する際に呼ばれる関数
 * @param[in] user_data Wikipediaパーサの環境
 * @param[in] data element データ
 * @param[in] data_size データの長さ
 */
static void XMLCALL
element_data(void *user_data, const XML_Char *data, int data_size)
{
  wikipedia_parser *p = (wikipedia_parser *)user_data;
  switch (p->status) {
  case IN_PAGE_TITLE:
    utstring_bincpy(p->title, data, data_size);
    break;
  case IN_PAGE_REVISION_TEXT:
    utstring_bincpy(p->body, data, data_size);
    break;
  default:
    /* do nothing */
    break;
  }
}

#define LOAD_BUFFER_SIZE 0x2000

/**
 * Wikipediaのdumpファイル(XML)を読み込み、指定された関数にその内容を渡す。
 * @param[in] env 環境
 * @param[in] path dumpファイルのpath
 * @param[in] func env, 記事タイトル, 記事本文の3引数を取る関数
 * @param[in] max_article_count 読み込む最大記事数
 * @retval 0 成功
 * @retval 1 メモリ確保に失敗
 * @retval 2 ファイルオープンに失敗
 * @retval 3 ファイルの読み込みに失敗
 * @retval 4 XMLファイルのパースに失敗
 */
int
load_wikipedia_dump(wiser_env *env,
                    const char *path, add_document_callback func, int max_article_count)
{
  FILE *fp;
  int rc = 0;
  XML_Parser xp;
  char buffer[LOAD_BUFFER_SIZE];
  wikipedia_parser wp = {
    env,               /* 環境 */
    IN_DOCUMENT,       /* 初期状態 */
    NULL,              /* タイトルを一時保存する領域 */
    NULL,              /* 本文を一時保存する領域 */
    0,                 /* 解析した記事の総数を初期化 */
    max_article_count, /* 解析する記事の最大数 */
    func               /* 解析後のドキュメントを渡す関数 */
  };

  if (!(xp = XML_ParserCreate("UTF-8"))) {
    print_error("cannot allocate memory for parser.");
    return 1;
  }

  if (!(fp = fopen(path, "rb"))) {
    print_error("cannot open wikipedia dump xml file(%s).",
                strerror(errno));
    rc = 2;
    goto exit;
  }

  XML_SetElementHandler(xp, start, end);
  XML_SetCharacterDataHandler(xp, element_data);
  XML_SetUserData(xp, (void *)&wp);

  while (1) {
    int buffer_len, done;

    buffer_len = (int)fread(buffer, 1, LOAD_BUFFER_SIZE, fp);
    if (ferror(fp)) {
      print_error("wikipedia dump xml file read error.");
      rc = 3;
      goto exit;
    }
    done = feof(fp);

    if (XML_Parse(xp, buffer, buffer_len, done) == XML_STATUS_ERROR) {
      print_error("wikipedia dump xml file parse error.");
      rc = 4;
      goto exit;
    }

    if (done || (max_article_count >= 0 &&
                 max_article_count <= wp.article_count)) { break; }
  }
exit:
  if (fp) {
    fclose(fp);
  }
  if (wp.title) {
    utstring_free(wp.title);
  }
  if (wp.body) {
    utstring_free(wp.body);
  }
  XML_ParserFree(xp);
  return rc;
}
