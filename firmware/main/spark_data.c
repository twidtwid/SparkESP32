#include "spark_data.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

// Nothing measured until the first sample: every field reads as unknown ("--").
spark_t g_spark = {
    .cpu = SPARK_UNKNOWN, .gpu = SPARK_UNKNOWN, .mem_used_mb = SPARK_UNKNOWN,
    .mem_total_mb = SPARK_UNKNOWN, .temp_c = SPARK_UNKNOWN, .power_dw = SPARK_UNKNOWN,
    .gpu_mhz = SPARK_UNKNOWN, .gpu_stuck = SPARK_UNKNOWN,
};

uint32_t spark_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

uint32_t spark_age_ms(void)
{
    if (!g_spark.valid) return UINT32_MAX;
    return spark_now_ms() - g_spark.fed_ms;   // unsigned subtraction is wrap-safe
}

int spark_mem_pct(void)
{
    if (g_spark.mem_total_mb == SPARK_UNKNOWN || g_spark.mem_used_mb == SPARK_UNKNOWN ||
        g_spark.mem_total_mb <= 0)
        return SPARK_UNKNOWN;
    int64_t p = (int64_t)g_spark.mem_used_mb * 100 / g_spark.mem_total_mb;
    return (int)(p < 0 ? 0 : (p > 100 ? 100 : p));
}

const char *spark_src_name(spark_src_t src)
{
    return src == SRC_USB ? "usb" : src == SRC_WIFI ? "wifi" : "none";
}

// Strict integer field: the whole token must be a number in [lo, hi], or "-".
static bool parse_field(const char *tok, int32_t lo, int32_t hi, int32_t *out)
{
    if (strcmp(tok, "-") == 0) { *out = SPARK_UNKNOWN; return true; }
    char *end;
    errno = 0;
    long v = strtol(tok, &end, 10);
    if (errno || end == tok || *end || v < lo || v > hi) return false;
    *out = (int32_t)v;
    return true;
}

static bool parse_cores(const char *tok, uint8_t *cores, int *n)
{
    *n = 0;
    if (strcmp(tok, "-") == 0) return true;
    const char *p = tok;
    while (*p) {
        char *end;
        errno = 0;
        long v = strtol(p, &end, 10);
        if (errno || end == p || v < 0 || v > 100 || (*end && *end != ',')) return false;
        if (*n < SPARK_MAX_CORES) cores[(*n)++] = (uint8_t)v;
        p = *end ? end + 1 : end;
        if (*end == ',' && !*p) return false;     // trailing comma
    }
    return true;
}

// Hostname: letters, digits, '-', '_' and '.', up to SPARK_HOST_MAX; "-" = unknown.
static bool parse_host(const char *tok, char *out)
{
    out[0] = '\0';
    if (strcmp(tok, "-") == 0) return true;
    size_t n = strlen(tok);
    if (n == 0 || n > SPARK_HOST_MAX) return false;
    for (const char *c = tok; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.') return false;
    memcpy(out, tok, n + 1);
    return true;
}

int spark_apply_args(int argc, char **argv, spark_src_t src, const char **why)
{
    if ((argc != 9 && argc != 11) || strcmp(argv[0], "stat") != 0) {
        *why = "expected 8 or 10 fields";
        return -1;
    }

    spark_t s = {0};
    if (!parse_field(argv[1], 0, 100, &s.cpu))             { *why = "cpu";   return -1; }
    if (!parse_field(argv[2], 0, 100, &s.gpu))             { *why = "gpu";   return -1; }
    if (!parse_field(argv[3], 0, INT32_MAX, &s.mem_used_mb))  { *why = "mem_used";  return -1; }
    if (!parse_field(argv[4], 0, INT32_MAX, &s.mem_total_mb)) { *why = "mem_total"; return -1; }
    if (!parse_field(argv[5], -50, 150, &s.temp_c))        { *why = "temp";  return -1; }
    if (!parse_field(argv[6], 0, 100000, &s.power_dw))     { *why = "power"; return -1; }
    if (!parse_cores(argv[7], s.cores, &s.ncores))         { *why = "cores"; return -1; }
    if (!parse_host(argv[8], s.host))                      { *why = "host";  return -1; }
    s.gpu_mhz = s.gpu_stuck = SPARK_UNKNOWN;               // v1: an agent from before the clock
    if (argc == 11) {
        if (!parse_field(argv[9], 0, 10000, &s.gpu_mhz))   { *why = "gpu_mhz";   return -1; }
        if (!parse_field(argv[10], 0, 1, &s.gpu_stuck))    { *why = "gpu_stuck"; return -1; }
    }

    s.source = src;
    s.fed_ms = spark_now_ms();
    s.valid = true;
    g_spark = s;
    return 0;
}

void spark_demo(int load)
{
    if (load < 0) load = 0;
    if (load > 100) load = 100;
    spark_t s = {
        .valid = true, .cpu = load, .gpu = load / 2,
        .mem_used_mb = 98591, .mem_total_mb = 130661,
        .temp_c = 35 + load / 10, .power_dw = 106 + load * 4, .gpu_mhz = 2405, .gpu_stuck = 0,
        .ncores = 20, .host = "spark-demo", .source = SRC_USB, .fed_ms = spark_now_ms(),
    };
    for (int i = 0; i < 20; i++) {
        int v = load + ((i * 37) % 50) - 20;           // spread around `load`
        s.cores[i] = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
    }
    g_spark = s;
}
