/* Dynamic byte buffer.
 *
 * Every byte that reaches this code came off a socket, so the rules are:
 * length is tracked explicitly (bodies may contain NUL), growth is amortised,
 * and every function returns 0 or -1 rather than aborting. Callers must check:
 * a failed append leaves the buffer untouched and usable.
 */
#ifndef UTIL_BUF_H
#define UTIL_BUF_H

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
} buf_t;

/* Allocates a buffer with room for cap bytes. Returns NULL on allocation
 * failure or if cap overflows size_t. */
buf_t *buf_new(size_t cap);

void buf_free(buf_t *b);

/* Appends len bytes. Returns 0, or -1 on failure leaving b unchanged. */
int buf_append(buf_t *b, const void *data, size_t len);

int buf_append_str(buf_t *b, const char *s);

/* printf into the buffer. Returns the number of bytes appended, or -1. */
int buf_appendf(buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Number of bytes currently held. */
size_t buf_len(const buf_t *b);

/* Remaining capacity, useful before deciding whether to grow. */
size_t buf_avail(const buf_t *b);

/* Hands the NUL-terminated contents to the caller as a malloc'd string and
 * empties the buffer. The struct itself stays alive and must still be released
 * with buf_free, so the natural `p = buf_detach(b); ...; buf_free(b);` is
 * correct. The caller owns the returned string and frees it with free().
 * Returns NULL when the buffer is empty. */
char *buf_detach(buf_t *b);

/* Drops the contents but keeps the allocation, so a per-request loop can reuse
 * one buffer instead of allocating per request. */
void buf_reset(buf_t *b);

/* Appends a NUL without counting it. The buffer tracks its own length, so this
 * only exists for handing the bytes to a C API that expects a string. */
void buf_terminate(buf_t *b);

/* Allocates a buffer into *out, storing NULL when allocation fails. Saves the
 * three lines of check-then-assign that every caller would otherwise repeat. */
void buf_new_into(buf_t **out, size_t cap);

#endif /* UTIL_BUF_H */
