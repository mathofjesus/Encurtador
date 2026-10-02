#include <criterion/criterion.h>
#include <string.h>

#include "observability/metrics.h"

Test(metrics, counters_start_at_zero_and_are_independent)
{
    metrics_t *m = metrics_new();
    cr_assert_not_null(m);

    for (int i = 0; i < METRIC_COUNT; i++)
        cr_assert_eq(metrics_get(m, (metric_id_t)i), 0, "counter %d was not zero", i);

    metrics_inc(m, METRIC_SHORTEN_TOTAL);
    metrics_add(m, METRIC_SHORTEN_TOTAL, 4);
    metrics_inc(m, METRIC_RATE_LIMITED_TOTAL);

    cr_assert_eq(metrics_get(m, METRIC_SHORTEN_TOTAL), 5);
    cr_assert_eq(metrics_get(m, METRIC_RATE_LIMITED_TOTAL), 1);
    cr_assert_eq(metrics_get(m, METRIC_SHORTEN_TOTAL), 5,
                 "one counter changed another");

    metrics_free(m);
}

Test(metrics, render_is_prometheus_text_and_includes_zero_series)
{
    metrics_t *m = metrics_new();
    metrics_inc(m, METRIC_REDIRECT_TOTAL);
    metrics_inc(m, METRIC_REDIRECT_TOTAL);
    metrics_inc(m, METRIC_REDIRECT_MISS_TOTAL);

    buf_t *out = buf_new(256);
    cr_assert_not_null(out);
    metrics_render(m, out);

    const char *s = out->data;
    cr_assert_not_null(strstr(s, "# TYPE shortener_redirect_total counter"));
    cr_assert_not_null(strstr(s, "shortener_redirect_total 2"));
    cr_assert_not_null(strstr(s, "shortener_redirect_miss_total 1"));
    /* A counter that has never moved must still be present, or a dashboard
     * shows "no data" where it should show zero. */
    cr_assert_not_null(strstr(s, "shortener_shorten_total 0"));
    cr_assert_not_null(strstr(s, "shortener_server_error_total 0"));

    buf_free(out);
    metrics_free(m);
}

Test(metrics, rendering_a_missing_metrics_object_yields_zero_series)
{
    buf_t *out = buf_new(128);
    cr_assert_not_null(out);

    metrics_render(NULL, out);
    cr_assert(strlen(out->data) > 0, "the metrics body was empty");
    cr_assert_not_null(strstr(out->data, "shortener_shorten_total 0"));

    buf_free(out);
}

Test(metrics, null_and_out_of_range_arguments_are_safe)
{
    metrics_free(NULL);
    metrics_inc(NULL, METRIC_SHORTEN_TOTAL);
    metrics_add(NULL, METRIC_SHORTEN_TOTAL, 3);
    cr_assert_eq(metrics_get(NULL, METRIC_SHORTEN_TOTAL), 0);
    metrics_render(NULL, NULL);

    metrics_t *m = metrics_new();
    /* Out of range must be ignored, not written past the array. */
    metrics_inc(m, (metric_id_t)999);
    metrics_add(m, (metric_id_t)-5, 7);
    for (int i = 0; i < METRIC_COUNT; i++)
        cr_assert_eq(metrics_get(m, (metric_id_t)i), 0);

    metrics_free(m);
}
