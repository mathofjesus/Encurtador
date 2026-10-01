#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

Test(config, loads_the_values_the_plan_pins)
{
    config_t *c = config_load("config.yaml");
    cr_assert_not_null(c);

    cr_assert_str_eq(c->app.host, "0.0.0.0");
    cr_assert_eq(c->app.port, 8000);
    cr_assert_str_eq(c->app.base_url, "https://short.ly");
    cr_assert_eq(c->app.workers, 1);

    /* Sized for the 1 vCPU target, not copied from a larger host. */
    cr_assert_eq(c->database.pool_size, 2);
    cr_assert_eq(c->database.max_overflow, 2);

    cr_assert_eq(c->cache.redis_ttl_seconds, 86400);
    cr_assert_str_eq(c->cache.redis_url, "redis://sb_redis:6379/0");

    cr_assert_eq(c->codegen.min_length, 7);
    cr_assert_eq(c->codegen.max_length, 12);

    cr_assert_eq(c->rate_limit.per_minute, 60);
    cr_assert_eq(c->rate_limit.burst, 10);

    /* Ten years, per the retention requirement. */
    cr_assert_eq(c->retention.default_ttl_days, 3650);
    cr_assert_eq(c->retention.max_ttl_days, 3650);

    config_free(c);
}

Test(config, missing_file_returns_null)
{
    cr_assert_null(config_load("/nonexistent/config.yaml"));
    cr_assert_null(config_load(NULL));
}

Test(config, free_accepts_null)
{
    config_free(NULL); /* must not crash */
}

Test(config, loads_from_an_explicit_environment_override)
{
    /* The sandbox addresses services by container name while a local run uses
     * localhost, so the DSNs must be overridable without editing the file. */
    config_t *c = config_load("config.yaml");
    cr_assert_not_null(c);

    /* The file itself names the sandbox containers, which is what CI uses. */
    cr_assert(strstr(c->database.primary_dsn, "sb_pg") != NULL);
    cr_assert(strstr(c->database.primary_dsn, "shortener") != NULL);
    cr_assert(strstr(c->cache.redis_url, "sb_redis") != NULL);

    config_free(c);
}

Test(config, rejects_a_file_whose_values_are_out_of_range)
{
    /* A pool size larger than the connection count would silently never queue,
     * and a zero port would make the server unbindable. Bad config must fail at
     * load rather than halfway through startup. */
    const char *path = "/tmp/config_bad.yaml";
    FILE *f = fopen(path, "w");
    cr_assert_not_null(f);
    fputs("app:\n  host: 0.0.0.0\n  port: 0\n", f);
    fclose(f);

    cr_assert_null(config_load(path));
    remove(path);
}

Test(config, defaults_fill_in_absent_keys)
{
    /* A partial file must still produce a usable config with the documented
     * defaults, rather than leaving fields uninitialised. */
    const char *path = "/tmp/config_partial.yaml";
    FILE *f = fopen(path, "w");
    cr_assert_not_null(f);
    fputs("app:\n  port: 9000\n", f);
    fclose(f);

    config_t *c = config_load(path);
    cr_assert_not_null(c);
    cr_assert_eq(c->app.port, 9000);
    /* Untouched keys keep their defaults. */
    cr_assert_eq(c->retention.default_ttl_days, 3650);
    cr_assert_eq(c->database.pool_size, 2);

    config_free(c);
    remove(path);
}
