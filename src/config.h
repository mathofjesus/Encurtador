/* Configuration loading.
 *
 * The reader covers the flat "section: key: value" shape of config.yaml and
 * nothing more. That is a deliberate limit: a general YAML parser is a large
 * attack surface for a configuration file nobody needs to be clever.
 *
 * Every value has a default, so a partial file still yields a usable config.
 * Values are validated at load, not at first use, so a bad config fails at
 * startup instead of halfway through serving traffic.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

typedef struct {
    char host[64];
    int port;
    int workers;
    char base_url[256];
} app_config_t;

typedef struct {
    char primary_dsn[512];
    char replica_dsn[512];
    int pool_size;
    int max_overflow;
} database_config_t;

typedef struct {
    char redis_url[256];
    int redis_ttl_seconds;
} cache_config_t;

typedef struct {
    int min_length;
    int max_length;
} codegen_config_t;

typedef struct {
    int per_minute;
    int burst;
} rate_limit_config_t;

typedef struct {
    int default_ttl_days;
    int max_ttl_days;
} retention_config_t;

typedef struct {
    app_config_t app;
    database_config_t database;
    cache_config_t cache;
    codegen_config_t codegen;
    rate_limit_config_t rate_limit;
    retention_config_t retention;
} config_t;

/* Loads config from path, applying defaults for absent keys. Environment
 * variables override the file where they exist: DATABASE_URL, REDIS_URL,
 * BASE_URL and PORT. Returns NULL on a missing or unreadable file, or when a
 * value is out of range. The caller frees with config_free. */
config_t *config_load(const char *path);

/* Safe on NULL. */
void config_free(config_t *c);

#endif /* CONFIG_H */
