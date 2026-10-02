/* Rate limiter tests.
 *
 * Time is an argument, so every test is deterministic and none of them sleep.
 */
#include <criterion/criterion.h>
#include <stddef.h>

#include "services/rate_limit.h"

/* 60/minute is one token per second; a burst of 10 is a full bucket. */
#define PER_MINUTE 60
#define BURST 10

Test(rate_limit, a_full_bucket_allows_exactly_its_burst_then_throttles)
{
    rl_t *rl = rl_new(PER_MINUTE, BURST, 1024);
    cr_assert_not_null(rl);

    for (int i = 0; i < BURST; i++)
        cr_assert_eq(rl_allow(rl, "10.0.0.1", 0), 1,
                     "request %d of the burst was throttled", i + 1);

    /* The bucket is empty, so the next request is refused even though no time
     * has passed. */
    cr_assert_eq(rl_allow(rl, "10.0.0.1", 0), 0);

    rl_free(rl);
}

Test(rate_limit, tokens_refill_at_the_configured_rate)
{
    rl_t *rl = rl_new(PER_MINUTE, BURST, 1024);
    cr_assert_not_null(rl);

    for (int i = 0; i < BURST; i++)
        cr_assert_eq(rl_allow(rl, "10.0.0.1", 0), 1);

    /* One second at 60/minute restores exactly one token. */
    cr_assert_eq(rl_allow(rl, "10.0.0.1", 1000), 1,
                 "one second should have restored one token");
    cr_assert_eq(rl_allow(rl, "10.0.0.1", 1000), 0,
                 "only one token was restored");

    /* A full minute restores the whole bucket, and no more: a long idle period
     * must not accumulate credit beyond the burst. */
    cr_assert_eq(rl_allow(rl, "10.0.0.1", 1000 + 60 * 1000), 1);
    for (int i = 0; i < BURST; i++)
        cr_assert_eq(rl_allow(rl, "10.0.0.1", 1000 + 60 * 1000), i < BURST - 1,
                     "the bucket refilled past its capacity");

    rl_free(rl);
}

Test(rate_limit, buckets_are_per_key_so_one_client_cannot_throttle_another)
{
    rl_t *rl = rl_new(PER_MINUTE, BURST, 1024);
    cr_assert_not_null(rl);

    /* Exhaust one client. */
    for (int i = 0; i < BURST; i++)
        cr_assert_eq(rl_allow(rl, "10.0.0.1", 0), 1);
    cr_assert_eq(rl_allow(rl, "10.0.0.1", 0), 0);

    /* A different client still has its own full bucket. */
    for (int i = 0; i < BURST; i++)
        cr_assert_eq(rl_allow(rl, "10.0.0.2", 0), 1,
                     "a second client was throttled by the first client's usage");

    cr_assert_eq(rl_size(rl), 2);
    rl_free(rl);
}

Test(rate_limit, evicting_a_key_forgets_its_history_rather_than_refusing_it)
{
    /* The cap is a memory bound. When it is reached the least recently used key
     * goes, which can only ever make the limiter *more* permissive for an idle
     * client — never refuse a new one, which would be a denial of service
     * against whoever the cap happened to fall on. */
    rl_t *rl = rl_new(PER_MINUTE, BURST, 2);
    cr_assert_not_null(rl);

    cr_assert_eq(rl_allow(rl, "a", 0), 1);
    cr_assert_eq(rl_allow(rl, "b", 0), 1);
    cr_assert_eq(rl_size(rl), 2);

    /* Touching "a" makes "b" the least recently used. */
    cr_assert_eq(rl_allow(rl, "a", 0), 1);

    /* Inserting "c" evicts "b". */
    cr_assert_eq(rl_allow(rl, "c", 0), 1);
    cr_assert_eq(rl_size(rl), 2, "the table grew past its cap");

    /* "b" is gone, so it comes back with a full bucket. */
    for (int i = 0; i < BURST; i++)
        cr_assert_eq(rl_allow(rl, "b", 0), 1,
                     "an evicted key was not treated as unseen");

    rl_free(rl);
}

Test(rate_limit, refuses_degenerate_keys_and_tolerates_null)
{
    cr_assert_null(rl_new(0, BURST, 16), "per_minute 0 can never refill");
    cr_assert_null(rl_new(PER_MINUTE, 0, 16), "a bucket of 0 allows nothing");
    cr_assert_null(rl_new(PER_MINUTE, BURST, 0), "a cap of 0 holds nothing");

    rl_free(NULL);
    cr_assert_eq(rl_size(NULL), 0);

    rl_t *rl = rl_new(PER_MINUTE, BURST, 16);
    cr_assert_not_null(rl);
    cr_assert_eq(rl_allow(NULL, "a", 0), -1);
    cr_assert_eq(rl_allow(rl, NULL, 0), -1);
    cr_assert_eq(rl_allow(rl, "", 0), -1);

    /* One longer than the key buffer is refused, not truncated: two clients
     * sharing a truncated key would share one bucket. */
    char too_long[RL_MAX_KEY + 2];
    for (size_t i = 0; i < sizeof(too_long) - 1; i++)
        too_long[i] = 'x';
    too_long[sizeof(too_long) - 1] = '\0';
    cr_assert_eq(rl_allow(rl, too_long, 0), -1);

    rl_free(rl);
}
