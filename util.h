#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

typedef uint32_t
UTF32Char; /* UTF-32でコーディングされたUnicode文字列 */
#define MAX_UTF8_SIZE 4 /* UTF-8でUnicode文字1文字を表現するのに必要な最大バイト */

typedef struct {
  char *head;       /* バッファの先頭を指す */
  char *curr;       /* バッファの現在地を指す */
  const char *tail; /* バッファの末尾を指す */
  int bit;          /* バッファの現在地（ビット単位） */
} buffer;

#define BUFFER_PTR(b) ((b)->head) /* バッファの先頭を返す */
#define BUFFER_SIZE(b) ((b)->curr - (b)->head) /* バッファのサイズを返す */

int print_error(const char *format, ...);
buffer *alloc_buffer(void);
int append_buffer(buffer *buf, const void *data,
                  unsigned int data_size);
void free_buffer(buffer *buf);
void append_buffer_bit(buffer *buf, int bit);
char *utf32toutf8(const UTF32Char *ustr, int ustr_len, char *str,
                  int *str_size);
int utf8toutf32(const char *str, int str_size, UTF32Char **ustr,
                int *ustr_len);
void print_time_diff(void);

#endif /* __UTIL_H__ */
