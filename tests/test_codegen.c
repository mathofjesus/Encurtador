#include <criterion/criterion.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"

Test(codegen, alphabet_is_base62)
{
    cr_assert_str_eq(CODE_ALPHABET,
                     "0123456789abcdefghijklmnopqrstuvwxyz"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    cr_assert_eq(strlen(CODE_ALPHABET), 62);
}

Test(codegen, roundtrip_across_magnitudes)
{
    unsigned long long ids[] = {1, 61, 62, 63, 3843, 1000000,
                                1000000000000ULL, 365000000000ULL};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char buf[CODE_MAX_LENGTH + 1];
        memset(buf, 0xEE, sizeof(buf));
        code_encode(ids[i], buf, sizeof(buf));

        unsigned long long back = 0;
        cr_assert_eq(code_decode(buf, &back), 0, "id=%llu code='%s'", ids[i], buf);
        cr_assert_eq(back, ids[i], "roundtrip failed for '%s'", buf);
    }
}

Test(codegen, minimum_length_is_seven)
{
    char buf[CODE_MAX_LENGTH + 1];

    code_encode(1, buf, sizeof(buf));
    cr_assert_eq(strlen(buf), 7);
    cr_assert_eq(buf[0], '0', "small ids are left-padded with the alphabet's first symbol");

    /* 62^6 - 1 is the largest id that still fits in six symbols. */
    code_encode(62ULL * 62 * 62 * 62 * 62 * 62 - 1, buf, sizeof(buf));
    cr_assert_eq(strlen(buf), 7, "the padding keeps every code at least 7 long");
}

Test(codegen, seven_chars_is_the_capacity_ten_years_needs)
{
    /* This is the arithmetic behind the 7-character minimum. At 100M URLs/day
     * for ten years the service must hand out 365e9 distinct codes. */
    const double per_day = 100000000.0;
    const double ten_years = per_day * 365.0 * 10.0;

    const double six = 62.0 * 62 * 62 * 62 * 62 * 62;
    const double seven = six * 62.0;

    /* Six symbols cannot hold ten years of codes. */
    cr_assert_lt(six, ten_years,
                 "62^6 = %.0f would be enough, which would contradict the 7-char minimum",
                 six);

    /* Seven symbols hold it with room to spare. */
    cr_assert_gt(seven, ten_years);
    cr_assert_eq(CODE_MIN_LENGTH, 7);
}

Test(codegen, code_just_past_62_to_6_becomes_seven_characters)
{
    char buf[CODE_MAX_LENGTH + 1];

    /* 62^6 is the first id that needs a seventh symbol; padding alone would not
     * produce it, so the encoder must actually use the extra position. */
    code_encode(62ULL * 62 * 62 * 62 * 62 * 62, buf, sizeof(buf));
    cr_assert_eq(strlen(buf), 7);
    cr_assert_neq(buf[0], '0', "id 62^6 must not be left-padded: it fills seven symbols");
}

Test(codegen, decode_rejects_characters_outside_the_alphabet)
{
    unsigned long long out = 0;
    cr_assert_neq(code_decode("abc-123", &out), 0);
    cr_assert_neq(code_decode("abc/123", &out), 0);
    cr_assert_neq(code_decode("abc 123", &out), 0);
    cr_assert_neq(code_decode("", &out), 0);
    cr_assert_neq(code_decode(NULL, &out), 0);
    /* Underscore is the classic confusion with base64 and is not in base62. */
    cr_assert_neq(code_decode("abc_123", &out), 0);
}

Test(codegen, decode_rejects_an_overflowing_number)
{
    /* 20 symbols is far past 2^64. Returning a truncated value here would hand
     * out a code that decodes to a different, already-used id. */
    unsigned long long out = 0;
    cr_assert_neq(code_decode("zzzzzzzzzzzzzzzzzzzz", &out), 0);
}

Test(codegen, is_valid_enforces_length_and_alphabet)
{
    /* Generated codes are always CODE_MIN_LENGTH because of the padding. */
    cr_assert_eq(code_is_valid("abc1234"), 1);         /* 7 */
    cr_assert_eq(code_is_valid("abc123"), 0);          /* 6, too short for generated */
    cr_assert_eq(code_is_valid("abc"), 0);             /* 3 */
    cr_assert_eq(code_is_valid(""), 0);
    cr_assert_eq(code_is_valid(NULL), 0);
    cr_assert_eq(code_is_valid("abcdefghijklmn"), 0);  /* 14, too long */
    cr_assert_eq(code_is_valid("abc-123"), 0);         /* bad character */
}

Test(codegen, custom_codes_may_be_shorter_than_generated_ones)
{
    /* Two different rules. A client picks "mycode" for memorability, which is
     * 6 characters and shorter than the generated minimum. Folding both rules
     * into one function would either reject that or accept malformed
     * generated codes. */
    cr_assert_eq(code_is_valid_custom("mycode"), 1);   /* 6 */
    cr_assert_eq(code_is_valid_custom("abc1234"), 1);  /* 7 */
    cr_assert_eq(code_is_valid_custom("abcd"), 1);     /* 4, the floor */
    cr_assert_eq(code_is_valid_custom("abc"), 0);      /* 3, below the floor */
    cr_assert_eq(code_is_valid_custom(""), 0);
    cr_assert_eq(code_is_valid_custom(NULL), 0);
    cr_assert_eq(code_is_valid_custom("abc-1234"), 0); /* bad character */
    cr_assert_eq(code_is_valid_custom("abcdefghijklmn"), 0);

    /* The generated validator is the stricter of the two. */
    cr_assert_eq(code_is_valid("mycode"), 0);
    cr_assert_eq(code_is_valid_custom("mycode"), 1);
}

Test(codegen, encode_into_a_buffer_that_is_too_small_fails_without_writing)
{
    char tiny[3];
    memset(tiny, 0x7F, sizeof(tiny));

    cr_assert_neq(code_encode(1234567, tiny, sizeof(tiny)), 0);
    /* Untouched: a partial write would look like a valid short code. */
    cr_assert(tiny[0] == 0x7F);
}

Test(codegen, codes_are_distinct_across_a_dense_id_range)
{
    /* A collision means two URLs sharing one code, which the uniqueness index
     * would then reject at write time. */
    char seen[4096][CODE_MAX_LENGTH + 1];
    const unsigned long long start = 1000000;

    for (unsigned long long i = 0; i < 4096; i++) {
        code_encode(start + i, seen[i], sizeof(seen[i]));
        for (unsigned long long j = 0; j < i; j++)
            cr_assert_str_neq(seen[i], seen[j],
                              "collision between id %llu and %llu", start + i, start + j);
    }
}
