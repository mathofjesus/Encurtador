/* Integration tests for the cache. These need a real Redis, because what is
 * being tested — TTLs, eviction, absent keys — is server behaviour. */
#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

#include "cache/redis.h"

#define REDIS_MAX_VALUE 4096

static const char *test_url(void)
{
    const char *url = getenv("TEST_REDIS_URL");
    if (!url || !*url)
        url = getenv("REDIS_URL");
    return url;
}

static redis_client_t *fresh_client(void)
{
    const char *url = test_url();
    cr_assert_not_null(url, "neither TEST_REDIS_URL nor REDIS_URL is set");

    redis_client_t *c = redis_init(url);
    cr_assert_not_null(c);
    return c;
}

static void forget(redis_client_t *c, const char *key)
{
    (void)redis_invalidate(c, key);
}

Test(redis, set_then_get_roundtrips)
{
    redis_client_t *c = fresh_client();
    forget(c, "test:roundtrip");

    cr_assert_eq(redis_setex(c, "test:roundtrip", "https://example.com/abc", 60),
                 REDIS_OK);

    char out[REDIS_MAX_VALUE];
    cr_assert_eq(redis_get(c, "test:roundtrip", out, sizeof(out)), REDIS_OK);
    cr_assert_str_eq(out, "https://example.com/abc");

    forget(c, "test:roundtrip");
    redis_free(c);
}

Test(redis, an_absent_key_is_a_miss_rather_than_an_error)
{
    redis_client_t *c = fresh_client();
    forget(c, "test:absent");

    char out[REDIS_MAX_VALUE];
    cr_assert_eq(redis_get(c, "test:absent", out, sizeof(out)), REDIS_MISS);

    redis_free(c);
}

Test(redis, invalidate_removes_the_entry)
{
    redis_client_t *c = fresh_client();

    cr_assert_eq(redis_setex(c, "test:invalidate", "v", 60), REDIS_OK);
    char out[REDIS_MAX_VALUE];
    cr_assert_eq(redis_get(c, "test:invalidate", out, sizeof(out)), REDIS_OK);

    cr_assert_eq(redis_invalidate(c, "test:invalidate"), REDIS_OK);

    cr_assert_eq(redis_get(c, "test:invalidate", out, sizeof(out)), REDIS_MISS);

    redis_free(c);
}

Test(redis, invalidating_an_absent_key_is_not_an_error)
{
    redis_client_t *c = fresh_client();
    /* Deleting something that was never there is a success: the caller's intent
     * is that the key is gone, and it is. */
    cr_assert_eq(redis_invalidate(c, "test:never-existed"), REDIS_OK);
    redis_free(c);
}

Test(redis, every_entry_carries_a_ttl)
{
    redis_client_t *c = fresh_client();
    forget(c, "test:ttl");

    cr_assert_eq(redis_setex(c, "test:ttl", "v", 120), REDIS_OK);

    int ttl = redis_ttl(c, "test:ttl");
    cr_assert_neq(ttl, REDIS_ERROR);
    cr_assert(ttl > 0, "expected a live TTL, got %d", ttl);
    cr_assert_leq(ttl, 120);

    forget(c, "test:ttl");
    redis_free(c);
}

Test(redis, a_non_positive_ttl_is_refused)
{
    redis_client_t *c = fresh_client();
    /* An entry with no expiry is the leak this cache exists to avoid: the URLs
     * are permanent, so a cached copy with no TTL would never be collected. */
    cr_assert_eq(redis_setex(c, "test:nottl", "v", 0), REDIS_ERROR);
    cr_assert_eq(redis_setex(c, "test:nottl", "v", -1), REDIS_ERROR);

    char out[REDIS_MAX_VALUE];
    cr_assert_eq(redis_get(c, "test:nottl", out, sizeof(out)), REDIS_MISS);

    redis_free(c);
}

Test(redis, a_value_too_large_for_the_buffer_is_an_error_not_a_truncation)
{
    redis_client_t *c = fresh_client();
    forget(c, "test:toobig");

    char big[REDIS_MAX_VALUE];
    memset(big, 'u', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    cr_assert_eq(redis_setex(c, "test:toobig", big, 60), REDIS_OK);

    /* A short buffer must not yield a shortened URL: the caller would then
     * redirect to a different site. */
    char small[16];
    cr_assert_eq(redis_get(c, "test:toobig", small, sizeof(small)), REDIS_ERROR);

    forget(c, "test:toobig");
    redis_free(c);
}

Test(redis, an_unreachable_server_is_refused_at_init)
{
    cr_assert_null(redis_init("redis://127.0.0.1:1/0"));
    cr_assert_null(redis_init("not-a-url"));
    cr_assert_null(redis_init(""));
    cr_assert_null(redis_init(NULL));
}

Test(redis, freeing_null_is_safe)
{
    redis_free(NULL);
}