#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

#include "util/buf.h"

Test(buf, append_and_length)
{
    buf_t *b = buf_new(16);
    cr_assert_not_null(b);

    cr_assert_eq(buf_append_str(b, "ab"), 0);
    cr_assert_eq(buf_append_str(b, "cd"), 0);
    cr_assert_eq(buf_len(b), 4);

    char *s = buf_detach(b);
    cr_assert_str_eq(s, "abcd");
    free(s);
    buf_free(b);
}

Test(buf, grow_beyond_initial_capacity)
{
    buf_t *b = buf_new(4);
    cr_assert_not_null(b);

    for (int i = 0; i < 1000; i++)
        cr_assert_eq(buf_append_str(b, "x"), 0);

    cr_assert_eq(buf_len(b), 1000);
    buf_free(b);
}

Test(buf, append_binary_bytes_with_embedded_nul)
{
    buf_t *b = buf_new(8);
    const char data[] = {'a', '\0', 'b'};

    cr_assert_eq(buf_append(b, data, sizeof(data)), 0);
    cr_assert_eq(buf_len(b), 3);
    cr_assert(memcmp(b->data, data, 3) == 0);

    buf_free(b);
}

Test(buf, appendf_formats)
{
    buf_t *b = buf_new(8);
    /* Returns the byte count so callers can track length without buf_len. Note
     * the buffer starts at 8 bytes and the formatted text is 9, so this also
     * exercises the grow-then-write path. */
    cr_assert_eq(buf_appendf(b, "%s=%d", "port", 8000), 9);
    cr_assert_eq(buf_len(b), 9);

    char *s = buf_detach(b);
    cr_assert_str_eq(s, "port=8000");
    free(s);
    buf_free(b);
}

Test(buf, reset_reuses_the_allocation)
{
    buf_t *b = buf_new(64);
    cr_assert_eq(buf_append_str(b, "something"), 0);
    void *before = b->data;

    buf_reset(b);
    cr_assert_eq(buf_len(b), 0);
    /* Keeping the allocation is the point of reset: a request loop must not
     * malloc/free per request. */
    cr_assert_eq(b->data, before);

    buf_free(b);
}

Test(buf, new_on_failure_returns_null_rather_than_corrupt_memory)
{
    /* Asking for a size that cannot be allocated must fail cleanly. A short
     * read of a size_t that overflowed on 32-bit would corrupt the heap. */
    buf_t *b = buf_new((size_t)-1);
    cr_assert_null(b);
}

Test(buf, append_to_null_buffer_is_rejected)
{
    cr_assert_eq(buf_append_str(NULL, "x"), -1);
    cr_assert_eq(buf_append(NULL, "x", 1), -1);
    cr_assert_eq(buf_appendf(NULL, "x"), -1);
    cr_assert_eq(buf_len(NULL), 0);
}
