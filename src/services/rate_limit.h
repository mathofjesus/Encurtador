/* Per-IP rate limiting.
 *
 * A token bucket rather than the fixed window the plan first sketched. A fixed
 * window has a known hole: a client can send the whole limit at the end of one
 * window and the whole limit again at the start of the next, so the busiest
 * second it can produce is twice the advertised limit. The bucket bounds that
 * busy second to `burst` and refills at `per_minute`, which is what the two
 * config values were for.
 *
 * The clock is passed in, not read here. A limiter that calls time() cannot be
 * tested without sleeping, and tests that sleep are tests that pass on a fast
 * machine and fail on a loaded one. Callers supply a monotonic millisecond
 * count; in production that is CLOCK_MONOTONIC.
 */
#ifndef SERVICES_RATE_LIMIT_H
#define SERVICES_RATE_LIMIT_H

#include <stddef.h>

typedef struct rl rl_t;

/* Largest client address kept, generous enough for an IPv6 literal in text
 * form. A longer key is refused rather than truncated, because truncation would
 * merge two clients into one bucket. */
#define RL_MAX_KEY 64

/* Creates a limiter with a bucket of `burst` tokens refilling at `per_minute`
 * tokens per minute, holding at most `max_buckets` keys. At the cap the least
 * recently used key is evicted, which only forgets an idle client and bounds
 * memory against a flood of distinct source addresses. Returns NULL on invalid
 * arguments or allocation failure. */
rl_t *rl_new(int per_minute, int burst, size_t max_buckets);

/* Safe on NULL. */
void rl_free(rl_t *rl);

/* Whether key may proceed at now_ms. Returns 1 to allow, 0 to throttle, and -1
 * when key is NULL, empty or too long to be a client address. */
int rl_allow(rl_t *rl, const char *key, long long now_ms);

/* Buckets currently held. Exposed so tests can prove eviction happens and so a
 * metrics endpoint could one day report it. */
size_t rl_size(const rl_t *rl);

#endif /* SERVICES_RATE_LIMIT_H */
