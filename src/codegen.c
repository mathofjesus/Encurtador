#include "codegen.h"

#include <limits.h>

/* Position of each character in CODE_ALPHABET, or -1. Built once on first use
 * so decode does not scan the alphabet for every symbol of every code. */
static signed char index_of[256];
static int index_ready = 0;

static void ensure_index(void)
{
    if (index_ready)
        return;
    for (int i = 0; i < 256; i++)
        index_of[i] = -1;
    for (int i = 0; i < CODE_BASE; i++)
        index_of[(unsigned char)CODE_ALPHABET[i]] = (signed char)i;
    index_ready = 1;
}

int code_encode(unsigned long long id, char *out, size_t out_len)
{
    if (!out)
        return -1;
    /* Checked up front so a short buffer is never partially written: a
     * truncated code still parses, and would resolve to a different id. */
    if (out_len < CODE_MAX_LENGTH + 1)
        return -1;

    char tmp[CODE_MAX_LENGTH];
    int n = 0;

    do {
        tmp[n++] = CODE_ALPHABET[id % CODE_BASE];
        id /= CODE_BASE;
    } while (id > 0 && n < CODE_MAX_LENGTH);

    /* A value needing more than CODE_MAX_LENGTH symbols is refused rather than
     * silently truncated into a colliding shorter code. */
    if (id > 0)
        return -1;

    /* Left-pad with the alphabet's first symbol so every code is the same
     * length. */
    int pad = CODE_MIN_LENGTH - n;
    if (pad < 0)
        pad = 0;

    int pos = 0;
    for (int i = 0; i < pad; i++)
        out[pos++] = CODE_ALPHABET[0];
    for (int i = n - 1; i >= 0; i--)
        out[pos++] = tmp[i];
    out[pos] = '\0';

    return pos;
}

int code_decode(const char *code, unsigned long long *out)
{
    if (!code || !out)
        return -1;
    if (code[0] == '\0')
        return -1;

    ensure_index();

    unsigned long long value = 0;
    for (size_t i = 0; code[i] != '\0'; i++) {
        signed char d = index_of[(unsigned char)code[i]];
        if (d < 0)
            return -1;
        /* Refuse before wrapping. A silently truncated value would decode to
         * an id that already belongs to another URL. */
        if (value > (ULLONG_MAX - (unsigned long long)d) / CODE_BASE)
            return -1;
        value = value * CODE_BASE + (unsigned long long)d;
    }

    *out = value;
    return 0;
}

static int code_chars_ok(const char *code, size_t min_len)
{
    if (!code)
        return 0;

    size_t len = 0;
    ensure_index();

    for (size_t i = 0; code[i] != '\0'; i++) {
        if (i >= CODE_MAX_LENGTH)
            return 0;
        if (index_of[(unsigned char)code[i]] < 0)
            return 0;
        len++;
    }

    return len >= min_len;
}

int code_is_valid(const char *code)
{
    return code_chars_ok(code, CODE_MIN_LENGTH);
}

int code_is_valid_custom(const char *code)
{
    return code_chars_ok(code, CODE_CUSTOM_MIN_LENGTH);
}
