#include "services/rate_limit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bucket {
    char key[RL_MAX_KEY + 1];

    /* Fractional so a refill slower than one token per call is not rounded to
     * nothing. An integer token count would make any rate below one per call
     * become zero, which is a limiter that throttles forever. */
    double tokens;
    long long last_ms;

    struct bucket *hash_next;
    struct bucket *lru_prev;
    struct bucket *lru_next;
} bucket_t;

struct rl {
    bucket_t **table;
    size_t table_size; /* power of two, so the mask is a cheap modulo */
    size_t count;
    size_t max_buckets;

    double capacity;       /* tokens in a full bucket, == burst */
    double refill_per_ms;  /* per_minute / 60000 */

    bucket_t *lru_head; /* most recently used */
    bucket_t *lru_tail; /* least recently used, the eviction victim */
};

/* FNV-1a. Distribution only has to be good enough that a flood of similar
 * addresses does not all land in one chain; this is not a security boundary. */
static uint64_t hash_key(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static void lru_unlink(rl_t *rl, bucket_t *b)
{
    if (b->lru_prev)
        b->lru_prev->lru_next = b->lru_next;
    else
        rl->lru_head = b->lru_next;

    if (b->lru_next)
        b->lru_next->lru_prev = b->lru_prev;
    else
        rl->lru_tail = b->lru_prev;

    b->lru_prev = NULL;
    b->lru_next = NULL;
}

static void lru_push_front(rl_t *rl, bucket_t *b)
{
    b->lru_prev = NULL;
    b->lru_next = rl->lru_head;
    if (rl->lru_head)
        rl->lru_head->lru_prev = b;
    rl->lru_head = b;
    if (!rl->lru_tail)
        rl->lru_tail = b;
}

static size_t bucket_index(const rl_t *rl, const char *key)
{
    return (size_t)(hash_key(key) & (uint64_t)(rl->table_size - 1));
}

static bucket_t *bucket_find(const rl_t *rl, const char *key)
{
    for (bucket_t *b = rl->table[bucket_index(rl, key)]; b; b = b->hash_next)
        if (strcmp(b->key, key) == 0)
            return b;
    return NULL;
}

static void bucket_remove(rl_t *rl, bucket_t *b)
{
    bucket_t **link = &rl->table[bucket_index(rl, b->key)];
    while (*link) {
        if (*link == b) {
            *link = b->hash_next;
            return;
        }
        link = &(*link)->hash_next;
    }
}

static bucket_t *bucket_create(rl_t *rl, const char *key)
{
    /* At the cap the least recently used key gives way. Evicting rather than
     * refusing keeps a flood of distinct addresses from denying service to a
     * legitimate new client, and the client that loses its history is the one
     * that has been idle longest. */
    if (rl->count >= rl->max_buckets) {
        bucket_t *victim = rl->lru_tail;
        if (victim) {
            bucket_remove(rl, victim);
            lru_unlink(rl, victim);
            free(victim);
            rl->count--;
        }
    }

    bucket_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    snprintf(b->key, sizeof(b->key), "%s", key);
    size_t idx = bucket_index(rl, key);
    b->hash_next = rl->table[idx];
    rl->table[idx] = b;
    lru_push_front(rl, b);
    rl->count++;
    return b;
}

rl_t *rl_new(int per_minute, int burst, size_t max_buckets)
{
    /* Each of these would produce a limiter that silently never allows or
     * never bounds anything, so it is refused at construction instead. */
    if (per_minute <= 0 || burst <= 0 || max_buckets == 0)
        return NULL;

    rl_t *rl = calloc(1, sizeof(*rl));
    if (!rl)
        return NULL;

    /* Twice the bucket cap keeps chains short without wasting much on a table
     * that is mostly empty at realistic key counts. */
    size_t want = max_buckets > SIZE_MAX / 2 ? SIZE_MAX / 2 : max_buckets * 2;
    size_t size = 16;
    while (size < want) {
        if (size > SIZE_MAX / 2)
            break;
        size *= 2;
    }

    rl->table = calloc(size, sizeof(*rl->table));
    if (!rl->table) {
        free(rl);
        return NULL;
    }

    rl->table_size = size;
    rl->max_buckets = max_buckets;
    rl->capacity = (double)burst;
    rl->refill_per_ms = (double)per_minute / 60000.0;
    return rl;
}

void rl_free(rl_t *rl)
{
    if (!rl)
        return;

    for (size_t i = 0; i < rl->table_size; i++) {
        bucket_t *b = rl->table[i];
        while (b) {
            bucket_t *next = b->hash_next;
            free(b);
            b = next;
        }
    }
    free(rl->table);
    free(rl);
}

int rl_allow(rl_t *rl, const char *key, long long now_ms)
{
    if (!rl || !key || !*key)
        return -1;
    if (strlen(key) > RL_MAX_KEY)
        return -1;

    bucket_t *b = bucket_find(rl, key);
    if (!b) {
        b = bucket_create(rl, key);
        if (!b)
            return -1;
        b->tokens = rl->capacity;
        b->last_ms = now_ms;
    } else {
        long long elapsed = now_ms - b->last_ms;
        /* A backward clock is treated as no time passing rather than as a
         * negative refill, which would drain the bucket. */
        if (elapsed > 0)
            b->tokens += (double)elapsed * rl->refill_per_ms;
        b->last_ms = now_ms;
        if (b->tokens > rl->capacity)
            b->tokens = rl->capacity;
        lru_unlink(rl, b);
        lru_push_front(rl, b);
    }

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        return 1;
    }
    return 0;
}

size_t rl_size(const rl_t *rl)
{
    return rl ? rl->count : 0;
}
