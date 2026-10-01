#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

#include "util/json.h"

Test(json, parse_object_and_get_string)
{
    const char *src = "{\"url\":\"https://x.com\",\"ttl_days\":30}";
    json_obj_t *o = json_parse(src, strlen(src));
    cr_assert_not_null(o);

    cr_assert_str_eq(json_get_str(o, "url"), "https://x.com");

    long ttl = -1;
    cr_assert_eq(json_get_int(o, "ttl_days", &ttl), 0);
    cr_assert_eq(ttl, 30);

    json_free(o);
}

Test(json, reject_malformed)
{
    /* Returning NULL instead of guessing is the whole point: this parser sits
     * directly on attacker-controlled bytes from the network. */
    cr_assert_null(json_parse("{\"url\":", 7));
    cr_assert_null(json_parse("not json", 8));
    cr_assert_null(json_parse("{", 1));
    cr_assert_null(json_parse("", 0));
    cr_assert_null(json_parse(NULL, 10));
}

Test(json, handles_escapes_in_strings)
{
    const char *src = "{\"a\":\"line1\\nline2\",\"b\":\"say \\\"hi\\\"\"}";
    json_obj_t *o = json_parse(src, strlen(src));
    cr_assert_not_null(o);

    cr_assert_str_eq(json_get_str(o, "a"), "line1\nline2");
    cr_assert_str_eq(json_get_str(o, "b"), "say \"hi\"");

    json_free(o);
}

Test(json, missing_key_returns_null_rather_than_crashing)
{
    json_obj_t *o = json_parse("{\"url\":\"x\"}", 11);
    cr_assert_not_null(o);

    cr_assert_null(json_get_str(o, "nonexistent"));
    cr_assert_null(json_get_str(o, "url2"));
    cr_assert_null(json_get_str(NULL, "url"));

    long v;
    cr_assert_eq(json_get_int(o, "url", &v), -1);   /* wrong type */
    cr_assert_eq(json_get_int(o, "missing", &v), -1);

    json_free(o);
}

Test(json, negative_and_zero_integers)
{
    const char *src = "{\"a\":0,\"b\":-42,\"c\":3650}";
    json_obj_t *o = json_parse(src, strlen(src));
    cr_assert_not_null(o);

    long v = 999;
    cr_assert_eq(json_get_int(o, "a", &v), 0);
    cr_assert_eq(v, 0);

    cr_assert_eq(json_get_int(o, "b", &v), 0);
    cr_assert_eq(v, -42);

    cr_assert_eq(json_get_int(o, "c", &v), 0);
    cr_assert_eq(v, 3650);

    json_free(o);
}

Test(json, rejects_trailing_garbage)
{
    /* Truncation attacks append junk after a valid prefix. A parser that stops
     * at the closing brace would accept this and let the caller believe the
     * body was well-formed. */
    cr_assert_null(json_parse("{\"a\":1}garbage", 14));
    cr_assert_null(json_parse("{\"a\":1}{\"b\":2}", 15));
}

Test(json, ignores_whitespace)
{
    const char *src = "  {\n  \"url\"  :  \"https://y.com\" ,\n \"ttl_days\": 7 }  ";
    json_obj_t *o = json_parse(src, strlen(src));
    cr_assert_not_null(o);

    cr_assert_str_eq(json_get_str(o, "url"), "https://y.com");
    long v;
    cr_assert_eq(json_get_int(o, "ttl_days", &v), 0);
    cr_assert_eq(v, 7);

    json_free(o);
}

Test(json, length_is_respected_so_a_body_cannot_read_past_its_end)
{
    /* The HTTP layer hands over body_len, which may be shorter than the string
     * on some code paths. Parsing must stop at len, not at a NUL. The trailing
     * bytes are outside the declared length and must not be read. */
    const char *storage = "{\"url\":\"secret\"}TRAILING";
    const char *object = "{\"url\":\"secret\"}";
    json_obj_t *o = json_parse(storage, strlen(object));
    cr_assert_not_null(o);
    cr_assert_str_eq(json_get_str(o, "url"), "secret");
    json_free(o);
}
