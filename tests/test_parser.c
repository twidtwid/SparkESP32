// Host tests for the firmware's protocol parser (firmware/main/spark_data.c).
// Built and run by `make check` with the system C compiler; no ESP-IDF needed.
#include <stdio.h>
#include <string.h>

#include "spark_data.h"

int64_t g_fake_us;
static int failures, total;

// split like esp_console does for these inputs (no quoting needed here)
static int split(char *line, char **argv, int max)
{
    int n = 0;
    for (char *t = strtok(line, " "); t && n < max; t = strtok(NULL, " ")) argv[n++] = t;
    return n;
}

static int apply(const char *text, const char **why)
{
    char buf[512];
    char *argv[16];
    snprintf(buf, sizeof buf, "%s", text);
    return spark_apply_args(split(buf, argv, 16), argv, SRC_USB, why);
}

#define CHECK(cond, ...) do { total++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void accepts(const char *line)
{
    const char *why = NULL;
    CHECK(apply(line, &why) == 0, "rejected (%s): %s", why ? why : "?", line);
}

static void rejects(const char *line, const char *want_why)
{
    spark_t before = g_spark;
    const char *why = NULL;
    int rc = apply(line, &why);
    CHECK(rc != 0, "accepted: %s", line);
    CHECK(rc == 0 || (why && strcmp(why, want_why) == 0), "why=%s want %s: %s",
          why ? why : "(null)", want_why, line);
    CHECK(memcmp(&before, &g_spark, sizeof before) == 0, "state changed on reject: %s", line);
}

int main(void)
{
    // ---- before any sample, every field is unknown (the screen shows "--")
    CHECK(!g_spark.valid && g_spark.cpu == SPARK_UNKNOWN && g_spark.gpu == SPARK_UNKNOWN &&
          g_spark.temp_c == SPARK_UNKNOWN && g_spark.power_dw == SPARK_UNKNOWN &&
          g_spark.gpu_mhz == SPARK_UNKNOWN && g_spark.gpu_stuck == SPARK_UNKNOWN &&
          g_spark.ncores == 0 && spark_mem_pct() == SPARK_UNKNOWN, "unfed state is unknown");

    // ---- a real 20-core Spark sample, as sparkesp32-agent sends it (protocol v1)
    g_fake_us = 5000000;
    accepts("stat 6 91 46328 130661 60 410 1,1,1,2,0,0,1,0,1,1,0,0,1,1,1,1,99,1,0,0 spark-1a2b");
    CHECK(g_spark.valid && g_spark.cpu == 6 && g_spark.gpu == 91, "cpu/gpu");
    CHECK(g_spark.mem_used_mb == 46328 && g_spark.mem_total_mb == 130661, "mem");
    CHECK(g_spark.temp_c == 60 && g_spark.power_dw == 410, "temp/power");
    CHECK(g_spark.ncores == 20 && g_spark.cores[16] == 99, "cores");
    CHECK(g_spark.source == SRC_USB && g_spark.fed_ms == 5000, "source/fed_ms");
    CHECK(spark_mem_pct() == 35, "mem pct %d", spark_mem_pct());
    CHECK(g_spark.gpu_mhz == SPARK_UNKNOWN && g_spark.gpu_stuck == SPARK_UNKNOWN, "v1: no clock");

    // ---- protocol v2: the GPU clock and the stuck-clock verdict on the end
    accepts("stat 6 91 46328 130661 60 410 1,1,1,2 spark-1a2b 2405 0");
    CHECK(g_spark.gpu_mhz == 2405 && g_spark.gpu_stuck == 0, "v2 clock %d stuck %d",
          (int)g_spark.gpu_mhz, (int)g_spark.gpu_stuck);
    CHECK(g_spark.gpu == 91 && g_spark.ncores == 4 && strcmp(g_spark.host, "spark-1a2b") == 0, "v2 rest");
    accepts("stat 6 96 46328 130661 45 165 1,1 spark-1a2b 663 1");
    CHECK(g_spark.gpu_mhz == 663 && g_spark.gpu_stuck == 1, "stuck");
    accepts("stat 6 96 46328 130661 45 165 1,1 spark-1a2b - -");
    CHECK(g_spark.gpu_mhz == SPARK_UNKNOWN && g_spark.gpu_stuck == SPARK_UNKNOWN, "v2 unknowns");
    accepts("stat 6 96 46328 130661 45 165 1,1 spark-1a2b 0 -");
    accepts("stat 6 96 46328 130661 45 165 1,1 spark-1a2b 10000 1");
    accepts("stat 6 91 46328 130661 60 410 1,1 spark-1a2b");               // back to v1: clock unknown
    CHECK(g_spark.gpu_mhz == SPARK_UNKNOWN && g_spark.gpu_stuck == SPARK_UNKNOWN, "v1 after v2");

    // ---- unknowns
    accepts("stat - - - - - - - -");
    CHECK(g_spark.cpu == SPARK_UNKNOWN && g_spark.ncores == 0, "all unknown");
    CHECK(spark_mem_pct() == SPARK_UNKNOWN, "mem pct unknown");
    accepts("stat 0 - 1 2 -50 0 0 h");
    accepts("stat 100 100 2147483647 2147483647 150 100000 100 a.b_c-d");

    // ---- more than 32 cores: extras dropped, line still valid
    accepts("stat 1 1 1 1 1 1 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,"
            "25,26,27,28,29,30,31,32,33,34,35 h");
    CHECK(g_spark.ncores == SPARK_MAX_CORES && g_spark.cores[31] == 31, "core cap");

    // ---- rejects, with the field named and g_spark untouched
    accepts("stat 50 50 1000 2000 40 100 5,5 good");
    rejects("stat 50 50 1000", "expected 8 or 10 fields");
    rejects("stat 50 50 1000 2000 40 100 5,5 h 2405", "expected 8 or 10 fields");     // half a v2
    rejects("stat 50 50 1000 2000 40 100 5,5 h 2405 0 x", "expected 8 or 10 fields");
    rejects("stat 50 50 1000 2000 40 100 5,5", "expected 8 or 10 fields");    // pre-host 8-token form
    rejects("stats 50 50 1000 2000 40 100 5", "expected 8 or 10 fields");
    rejects("stats 50 50 1000 2000 40 100 5 h 2405 0", "expected 8 or 10 fields");
    rejects("stat 50 50 1000 2000 40 100 5 h 10001 0", "gpu_mhz");
    rejects("stat 50 50 1000 2000 40 100 5 h -1 0", "gpu_mhz");
    rejects("stat 50 50 1000 2000 40 100 5 h 2.4G 0", "gpu_mhz");
    rejects("stat 50 50 1000 2000 40 100 5 h 2405 2", "gpu_stuck");
    rejects("stat 50 50 1000 2000 40 100 5 h 2405 yes", "gpu_stuck");
    rejects("stat 101 50 1000 2000 40 100 5 h", "cpu");
    rejects("stat -1 50 1000 2000 40 100 5 h", "cpu");
    rejects("stat 5x 50 1000 2000 40 100 5 h", "cpu");
    rejects("stat 0x10 50 1000 2000 40 100 5 h", "cpu");
    rejects("stat 50 -- 1000 2000 40 100 5 h", "gpu");
    rejects("stat 50 50 -5 2000 40 100 5 h", "mem_used");
    rejects("stat 50 50 1000 99999999999 40 100 5 h", "mem_total");
    rejects("stat 50 50 1000 2000 151 100 5 h", "temp");
    rejects("stat 50 50 1000 2000 -51 100 5 h", "temp");
    rejects("stat 50 50 1000 2000 40 100001 5 h", "power");
    rejects("stat 50 50 1000 2000 40 100 5,,5 h", "cores");
    rejects("stat 50 50 1000 2000 40 100 5,5, h", "cores");
    rejects("stat 50 50 1000 2000 40 100 ,5 h", "cores");
    rejects("stat 50 50 1000 2000 40 100 5,101 h", "cores");
    rejects("stat 50 50 1000 2000 40 100 5,- h", "cores");
    rejects("stat 50 50 1000 2000 40 100 5;6 h", "cores");
    rejects("stat 50 50 1000 2000 40 100 5 bad/host", "host");
    rejects("stat 50 50 1000 2000 40 100 5 caf\xc3\xa9", "host");
    rejects("stat 50 50 1000 2000 40 100 5 "
            "a234567890123456789012345678901234567890123456789012345678901234", "host");   // 64

    // ---- host: carried through for the title; "-" clears it
    accepts("stat 1 1 1 1 1 1 1 spark-1a2b");
    CHECK(strcmp(g_spark.host, "spark-1a2b") == 0, "host = %s", g_spark.host);
    accepts("stat 1 1 1 1 1 1 1 "
            "a23456789012345678901234567890123456789012345678901234567890123");           // 63
    CHECK(strlen(g_spark.host) == SPARK_HOST_MAX, "63-char host kept");
    accepts("stat 1 1 1 1 1 1 1 -");
    CHECK(g_spark.host[0] == '\0', "host '-' = unknown");

    // ---- staleness math survives the 32-bit ms wrap (~49.7 days of uptime)
    g_fake_us = (int64_t)UINT32_MAX * 1000 - 1000000;      // 1 s before the wrap
    accepts("stat 1 1 1 1 1 1 1 h");
    g_fake_us += 3000000;                                  // 2 s after it
    CHECK(spark_age_ms() == 3000, "age across wrap = %u", (unsigned)spark_age_ms());

    printf("%d/%d parser checks passed\n", total - failures, total);
    return failures != 0;
}
