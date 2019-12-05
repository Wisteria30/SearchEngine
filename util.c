#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/time.h>

#include "util.h"

#define BUFFER_INIT_MIN 32 /* bufferを確保する際の初期バイト数 */

/**
 * エラーを標準出力に出力する。
 * @param[in] format printfに渡せるフォーマット文字列
 * @param[in] ... フォーマットに渡すパラメータ
 * @return 出力したバイト数
 */
int
print_error(const char *format, ...)
{
  int r;
  va_list l;

  va_start(l, format);
  vfprintf(stderr, format, l);
  r = fprintf(stderr, "\n");
  fflush(stderr);
  va_end(l);

  return r;
}

/**
 * バッファを確保する。
 * @return 確保されたバッファのポインタ
 */
buffer *
alloc_buffer(void)
{
  buffer *buf;
  if ((buf = malloc(sizeof(buffer)))) {
    if ((buf->head = malloc(BUFFER_INIT_MIN))) {
      buf->curr = buf->head;
      buf->tail = buf->head + BUFFER_INIT_MIN;
      buf->bit = 0;
    } else {
      free(buf);
      buf = NULL;
    }
  }
  return buf;
}

/**
 * エラーを標準出力に出力する。
 * @param[in] buf 拡大するbufferのポインタ
 * @retval 0 成功
 * @retval 1 失敗
 */
static int
enlarge_buffer(buffer *buf)
{
  int new_size;
  char *new_head;
  new_size = (buf->tail - buf->head) * 2;
  if ((new_head = realloc(buf->head, new_size))) {
    buf->curr = new_head + (buf->curr - buf->head);
    buf->tail = new_head + new_size;
    buf->head = new_head;
    return 0;
  } else {
    return 1;
  }
}

/**
 * バッファにデータを指定バイト数追加する。
 * @param[in] buf データを追加するbufferのポインタ
 * @param[in] data 追加されるデータへのポインタ
 * @param[in] data_size 追加されるデータのバイト数
 * @return 追加されたデータのバイト数
 */
int
append_buffer(buffer *buf, const void *data, unsigned int data_size)
{
  if (buf->bit) { buf->curr++; buf->bit = 0; }
  if (buf->curr + data_size > buf->tail) {
    if (enlarge_buffer(buf)) { return 0; }
  }
  if (data && data_size) {
    memcpy(buf->curr, data, data_size);
    buf->curr += data_size;
  }
  return data_size;
}

/**
 * バッファにデータを1ビット追加する。
 * @param[in] buf データを追加するbufferのポインタ
 * @param[in] bit 追加されるbit値。0か1。
 */
void
append_buffer_bit(buffer *buf, int bit)
{
  if (buf->curr >= buf->tail) {
    if (enlarge_buffer(buf)) { return; }
  }
  if (!buf->bit) { *buf->curr = 0; }
  if (bit) { *buf->curr |= 1 << (7 - buf->bit); }
  if (++(buf->bit) == 8) { buf->curr++; buf->bit = 0; }
}

/**
 * バッファを開放する。
 * @param[in] buf 開放するbufferのポインタ
 */
void
free_buffer(buffer *buf)
{
  free(buf->head);
  free(buf);
}

/**
 * UTF32CharをUTF-8化した場合に必要となるバイト数を計算する。
 * @param[in] ustr 入力文字列(UTF-32)
 * @param[in] ustr_len 入力文字列の文字長
 * @return utf-8でのバイト長
 **/
int
uchar2utf8_size(const UTF32Char *ustr, int ustr_len)
{
  int size = 0;
  const UTF32Char *ustr_end;
  for (ustr_end = ustr + ustr_len; ustr < ustr_end; ustr++) {
    if (*ustr < 0x800) {
      if (*ustr < 0x80) {
        size += 1;
      } else {
        size += 2;
      }
    } else {
      if (*ustr < 0x10000) {
        size += 3;
      } else if (*ustr < 0x200000) {
        size += 4;
      } else {
        abort();
      }
    }
  }
  return size;
}

/**
 * UTF32Charの長さ付き文字列を、NULL終端付のUTF-8に変換する。
 * バッファは呼び出し元で準備する。
 * @param[in] ustr 入力文字列(UTF-32)
 * @param[in] ustr_len 入力文字列の文字長。-1を指定した場合、NULL終端文字列。
 * @param[in,out] str 正規化された文字列(UTF-8)を格納するバッファ。
 *   ustr_len * MAX_UTF8_SIZE以上の充分なサイズが望まれる。
 * @param[out] str_size 正規化された文字列のバイト長。NULL指定可。
 * @return 正規化されたUnicode文字列
 */
char *
utf32toutf8(const UTF32Char *ustr, int ustr_len, char *str,
            int *str_size)
{
  int sbuf_size;
  sbuf_size = uchar2utf8_size(ustr, ustr_len);
  if (str_size) {
    *str_size = sbuf_size;
  }
  if (!str) {
    return NULL;
  } else {
    char *sbuf;
    const UTF32Char *ustr_end;
    for (sbuf = str, ustr_end = ustr + ustr_len; ustr < ustr_end;
         ustr++) {
      if (*ustr < 0x800) {
        if (*ustr < 0x80) {
          *sbuf++ = *ustr;
        } else {
          *sbuf++ = ((*ustr & 0x7c0) >> 6) | 0xc0;
          *sbuf++ = (*ustr & 0x3f) | 0x80;
        }
      } else {
        if (*ustr < 0x10000) {
          *sbuf++ = ((*ustr & 0xf000) >> 12) | 0xe0;
          *sbuf++ = ((*ustr & 0xfc0) >> 6) | 0x80;
          *sbuf++ = (*ustr & 0x3f) | 0x80;
        } else if (*ustr < 0x200000) {
          *sbuf++ = ((*ustr & 0x1c0000) >> 18) | 0xf0;
          *sbuf++ = ((*ustr & 0x3f000) >> 12) | 0x80;
          *sbuf++ = ((*ustr & 0xfc0) >> 6) | 0x80;
          *sbuf++ = (*ustr & 0x3f) | 0x80;
        } else {
          abort();
        }
      }
    }
    *sbuf = '\0';
  }
  return str;
}

/**
 * UTF-8文字列の1文字目が0x80-0xFFだった場合の文字数。0はエラー。
 **/
const static unsigned char utf8_skip_table[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 80-8F */
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 90-9F */
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* A0-AF */
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* B0-BF */
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, /* C0-CF */
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, /* D0-DF */
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, /* E0-EF */
  4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 0, 0, /* F0-FF */
};

/**
 * UTF-8文字列の文字数を計算する。
 * @param[in] str 入力文字列(UTF-8)
 * @param[in] str_size 入力文字列の文字長
 * @return utf-8でのバイト長
 **/
static int
utf8_len(const char *str, int str_size)
{
  int len = 0;
  const char *str_end;
  for (str_end = str + str_size; str < str_end;) {
    if (*str >= 0) {
      str += 1;
      len++;
    } else {
      unsigned char s = utf8_skip_table[*str + 0x80];
      if (!s) { abort(); }
      str += s;
      len++;
    }
  }
  return len;
}

/**
 * UTF-8の文字列を、UTF-32文字列に変換する。
 * UTF-32文字列は新しく確保されたバッファに格納される。
 * @param[in] str 入力文字列(UTF-8)
 * @param[in] str_size 入力文字列のバイト長。-1を指定した場合、NULL終端文字列。
 * @param[out] ustr 正規化された文字列(UTF-32)。NULL指定可。呼び出し側で開放する。
 * @param[out] ustr_len 正規化された文字列の文字長。NULL指定可。
 * @retval 0 成功
 */
int
utf8toutf32(const char *str, int str_size, UTF32Char **ustr,
            int *ustr_len)
{
  int ulen;
  ulen = utf8_len(str, str_size);
  if (ustr_len) { *ustr_len = ulen; }
  if (!ustr) { return 0; }
  if ((*ustr = malloc(sizeof(UTF32Char) * ulen))) {
    UTF32Char *u;
    const char *str_end;
    for (u = *ustr, str_end = str + str_size; str < str_end;) {
      if (*str >= 0) {
        *u++ = *str;
        str += 1;
      } else {
        unsigned char s = utf8_skip_table[*str + 0x80];
        if (!s) { abort(); }
        /* nバイトからなるUTF-8文字列の先頭から、下位(7 - n)bitを取り出す */
        *u = *str & ((1 << (7 - s)) - 1);
        /* 残りのUTF-8文字列から、6bitずつ取り出す */
        for (str++, s--; s--; str++) {
          *u = *u << 6;
          *u |= *str & 0x3f;
        }
        u++;
      }
    }
  } else {
    print_error("cannot allocate memory on utf8toutf32.");
  }
  return 0;
}

/**
 * struct timevalから時刻を表す文字列を作成する。
 * bufferの長さは37byte必要。
 */
void
timeval_to_str(struct timeval *clock, char *const buffer)
{
  struct tm result;
  localtime_r(&clock->tv_sec, &result);
  sprintf(buffer, "%04d/%02d/%02d %02d:%02d:%02d.%06d",
          result.tm_year + 1900,
          result.tm_mon + 1,
          result.tm_mday,
          result.tm_hour,
          result.tm_min,
          result.tm_sec,
          (int)(clock->tv_usec / 100000)
         );
}
#define TIMEVAL_TO_STR_BUFFER_SIZE 37

/**
 * struct timevalを浮動小数点表現にする
 * @return 浮動小数点表現
 */
double
timeval_to_double(struct timeval *tv)
{
  return ((double)(tv->tv_sec) + (double)(tv->tv_usec) * 0.000001);
}

/**
 * 現在時刻を取得し、前回取得した現在時刻との差分を計算する。
 * その両者を表示する。
 */
void
print_time_diff(void)
{
  char datetime_buf[TIMEVAL_TO_STR_BUFFER_SIZE];
  static double pre_time = 0.0;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  timeval_to_str(&tv, datetime_buf);
  double current_time = timeval_to_double(&tv);

  if (pre_time) {
    double time_diff = current_time - pre_time;
    print_error("[time] %s (diff %10.6lf)", datetime_buf, time_diff);
  } else {
    print_error("[time] %s", datetime_buf);
  }
  pre_time = current_time;
}
