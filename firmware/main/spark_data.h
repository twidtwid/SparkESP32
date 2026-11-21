#pragma once
// The vitals model and the one parser shared by both transports.
//
// Wire format, protocol v2 (11 space-separated tokens, one line per sample):
//   stat <cpu> <gpu> <mem_used_mb> <mem_total_mb> <temp_c> <power_dw> <cores> <host>
//        <gpu_mhz> <gpu_stuck>
// cpu/gpu/cores are percent 0-100; memory is decimal MB; power is deciwatts
// (106 = 10.6 W); <cores> is comma-separated per-core percent ("3,0,12,...");
// <host> is the sender's short hostname, the title when no name is saved;
// <gpu_mhz> is the GPU's SM clock; <gpu_stuck> is 1 when the agent has caught the
// GB10's stuck-clock state, 0 when it has seen the clock healthy under load.
// Any field may be "-" for "unknown" (rendered as "--"). A v1 line is the first 9
// tokens, what agents from before v2 send; the clock fields are then unknown.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPARK_PROTO     2            // the newest protocol the parser reads (docs/protocol.md)
#define SPARK_MAX_CORES 32           // shown in one heatmap row; extras ignored
#define SPARK_UNKNOWN   INT32_MIN    // a field the host couldn't measure
#define SPARK_HOST_MAX  63           // RFC 1123 label length

// The two clocks of the USB -> Wi-Fi handover. When USB samples stop, Wi-Fi
// polling resumes after SPARK_USB_FRESH_MS and has until SPARK_STALE_MS, when the
// screen stops saying ONLINE, for its first sample to land.
#define SPARK_USB_FRESH_MS 3000      // a USB sample this fresh suppresses Wi-Fi polling
#define SPARK_STALE_MS     5000      // no sample for this long: no longer ONLINE
_Static_assert(SPARK_USB_FRESH_MS < SPARK_STALE_MS,
               "Wi-Fi polling must resume before the screen gives up on a quiet USB feed");

typedef enum { SRC_NONE = 0, SRC_USB, SRC_WIFI } spark_src_t;

typedef struct {
    bool        valid;
    int32_t     cpu, gpu;                 // % or SPARK_UNKNOWN
    int32_t     mem_used_mb, mem_total_mb;
    int32_t     temp_c;
    int32_t     power_dw;
    int32_t     gpu_mhz;                  // SM clock, or SPARK_UNKNOWN
    int32_t     gpu_stuck;                // 1 stuck, 0 healthy, or SPARK_UNKNOWN
    int         ncores;
    uint8_t     cores[SPARK_MAX_CORES];
    char        host[SPARK_HOST_MAX + 1]; // "" if unknown
    spark_src_t source;
    uint32_t    fed_ms;                   // uptime ms of the last sample
} spark_t;

extern spark_t g_spark;   // written only under display_lock()

uint32_t    spark_now_ms(void);                  // monotonic, wraps after ~49 days
uint32_t    spark_age_ms(void);                  // UINT32_MAX if no sample yet
int         spark_mem_pct(void);                 // 0-100, or SPARK_UNKNOWN
const char *spark_src_name(spark_src_t src);

// Parse a tokenized `stat` line (argv[0] == "stat"; 9 tokens for v1, 11 for v2)
// and, if every field is well-formed, commit it to g_spark. On failure g_spark is untouched and
// *why names the offending field. Caller holds display_lock().
int spark_apply_args(int argc, char **argv, spark_src_t src, const char **why);

// Synthetic sample for bench QA.
void spark_demo(int load);

#ifdef __cplusplus
}
#endif
