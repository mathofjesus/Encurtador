#include "util/json.h"

#include <stdlib.h>
#include <string.h>

typedef enum { JV_STRING, JV_INT } jv_type_t;

typedef struct {
    char *key;
    jv_type_t type;
    char *s;   /* JV_STRING: NUL-terminated, unescaped */
    long i;    /* JV_INT */
} jmember_t;

struct json_obj {
    jmember_t *members;
    size_t count;
};

/* Parser state. Every helper takes (const char *src, size_t len, size_t *pos)
 * and refuses to advance past len, so no amount of malformed input can produce
 * an out-of-bounds read. */
typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    int depth;
} jp_t;

#define JSON_MAX_DEPTH 8 /* a request body is never a nested document */

static void skip_ws(jp_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static int at_end(const jp_t *p)
{
    return p->pos >= p->len;
}

static char peek(const jp_t *p)
{
    return at_end(p) ? '\0' : p->src[p->pos];
}

/* Appends one character to a growing buffer. Returns 0 or -1. */
static int push(char **out, size_t *len, size_t *cap, char c)
{
    if (*len + 1 >= *cap) {
        size_t want = *cap ? *cap * 2 : 32;
        char *np = realloc(*out, want);
        if (!np)
            return -1;
        *out = np;
        *cap = want;
    }
    (*out)[(*len)++] = c;
    return 0;
}

/* Reads a JSON string starting at the opening quote. Handles \" \\ \/ \b \f
 * \n \r \t \uXXXX. An invalid escape, an unterminated string, or a truncated
 * \u sequence is a hard error. Returns a malloc'd string, or NULL. */
static char *parse_string(jp_t *p)
{
    if (peek(p) != '"')
        return NULL;
    p->pos++;

    char *out = NULL;
    size_t len = 0, cap = 0;

    while (1) {
        if (at_end(p))
            goto fail;
        char c = p->src[p->pos++];

        if (c == '"')
            break;

        if (c != '\\') {
            /* Raw control characters are invalid inside a JSON string; accepting
             * them lets a caller inject bytes that break downstream logs. */
            if ((unsigned char)c < 0x20)
                goto fail;
            if (push(&out, &len, &cap, c) != 0)
                goto fail;
            continue;
        }

        if (at_end(p))
            goto fail;
        char e = p->src[p->pos++];
        char decoded;

        switch (e) {
        case '"':  decoded = '"';  break;
        case '\\': decoded = '\\'; break;
        case '/':  decoded = '/';  break;
        case 'b':  decoded = '\b'; break;
        case 'f':  decoded = '\f'; break;
        case 'n':  decoded = '\n'; break;
        case 'r':  decoded = '\r'; break;
        case 't':  decoded = '\t'; break;
        case 'u': {
            /* Full surrogate handling is out of scope for request bodies that
             * carry URLs. Accept the escape only when all four hex digits are
             * present, and reject surrogates rather than emitting invalid UTF-8
             * that would corrupt a database write. */
            if (p->len - p->pos < 4)
                goto fail;
            unsigned code = 0;
            for (int i = 0; i < 4; i++) {
                char h = p->src[p->pos++];
                unsigned d;
                if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                else goto fail;
                code = (code << 4) | d;
            }
            if (code >= 0xD800 && code <= 0xDFFF)
                goto fail; /* lone surrogate */
            if (code > 0x7F)
                goto fail; /* non-ASCII: not needed for URLs, reject explicitly */
            if (push(&out, &len, &cap, (char)code) != 0)
                goto fail;
            continue;
        }
        default:
            goto fail;
        }

        if (push(&out, &len, &cap, decoded) != 0)
            goto fail;
    }

    /* Always NUL-terminate so json_get_str can return it directly. */
    if (!out) {
        out = malloc(1);
        if (!out)
            return NULL;
        cap = 1;
    }
    if (len + 1 > cap) {
        char *np = realloc(out, len + 1);
        if (!np) {
            free(out);
            return NULL;
        }
        out = np;
    }
    out[len] = '\0';
    return out;

fail:
    free(out);
    return NULL;
}

/* Reads an integer. Rejects floats, exponents, and leading '+', because the
 * only numeric field is ttl_days and anything else is a client error. */
static int parse_int(jp_t *p, long *out)
{
    size_t start = p->pos;
    int negative = 0;

    if (peek(p) == '-') {
        negative = 1;
        p->pos++;
    }

    if (at_end(p))
        return -1;

    /* At least one digit required. */
    if (peek(p) < '0' || peek(p) > '9')
        return -1;

    long value = 0;
    while (!at_end(p) && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
        int d = p->src[p->pos++] - '0';
        /* Overflow guard: refuse before wrapping rather than storing garbage. */
        if (value > (2147483647L - d) / 10)
            return -1;
        value = value * 10 + d;
    }

    /* A '.' or exponent means this is not an integer. */
    char after = peek(p);
    if (after == '.' || after == 'e' || after == 'E')
        return -1;

    (void)start;
    *out = negative ? -value : value;
    return 0;
}

static int obj_add(json_obj_t *o, jmember_t m)
{
    jmember_t *nm = realloc(o->members, (o->count + 1) * sizeof(*nm));
    if (!nm)
        return -1;
    o->members = nm;
    o->members[o->count++] = m;
    return 0;
}

json_obj_t *json_parse(const char *src, size_t len)
{
    if (!src)
        return NULL;

    jp_t p = {.src = src, .len = len, .pos = 0, .depth = 0};
    json_obj_t *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;

    skip_ws(&p);
    if (peek(&p) != '{')
        goto fail;

    p.pos++;
    skip_ws(&p);

    if (peek(&p) == '}') {
        p.pos++;
        goto done;
    }

    while (1) {
        skip_ws(&p);

        char *key = parse_string(&p);
        if (!key)
            goto fail;

        skip_ws(&p);
        if (peek(&p) != ':') {
            free(key);
            goto fail;
        }
        p.pos++;
        skip_ws(&p);

        jmember_t m = {.key = key};
        char c = peek(&p);

        if (c == '"') {
            m.type = JV_STRING;
            m.s = parse_string(&p);
            if (!m.s) {
                free(key);
                goto fail;
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            m.type = JV_INT;
            if (parse_int(&p, &m.i) != 0) {
                free(key);
                goto fail;
            }
        } else {
            /* Nested objects, arrays, booleans and null are not part of this
             * API's contract. Rejecting them keeps the parser small and stops a
             * client smuggling structure past validation. */
            free(key);
            goto fail;
        }

        if (obj_add(o, m) != 0) {
            free(m.key);
            free(m.s);
            goto fail;
        }

        skip_ws(&p);
        char sep = peek(&p);
        if (sep == ',') {
            p.pos++;
            continue;
        }
        if (sep == '}') {
            p.pos++;
            break;
        }
        goto fail;
    }

done:
    /* Trailing content is an error, not something to ignore. A body that is a
     * valid object followed by junk is an attempt to smuggle something past a
     * parser that stopped early. */
    skip_ws(&p);
    if (!at_end(&p))
        goto fail;

    return o;

fail:
    json_free(o);
    return NULL;
}

void json_free(json_obj_t *o)
{
    if (!o)
        return;
    for (size_t i = 0; i < o->count; i++) {
        free(o->members[i].key);
        free(o->members[i].s);
    }
    free(o->members);
    free(o);
}

const char *json_get_str(const json_obj_t *o, const char *key)
{
    if (!o || !key)
        return NULL;
    for (size_t i = 0; i < o->count; i++) {
        if (o->members[i].type == JV_STRING && strcmp(o->members[i].key, key) == 0)
            return o->members[i].s;
    }
    return NULL;
}

int json_get_int(const json_obj_t *o, const char *key, long *out)
{
    if (!o || !key || !out)
        return -1;
    for (size_t i = 0; i < o->count; i++) {
        if (o->members[i].type == JV_INT && strcmp(o->members[i].key, key) == 0) {
            *out = o->members[i].i;
            return 0;
        }
    }
    return -1;
}

size_t json_count(const json_obj_t *o)
{
    return o ? o->count : 0;
}
