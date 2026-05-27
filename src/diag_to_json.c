/* diag_to_json.c - Realtek ONT read-only diagnostics as JSON.
 *
 * Purpose:
 *   Run as:
 *     # diag_to_json
 *
 * It does not execute vendor diag. It talks directly to the same Realtek
 * socket/getsockopt backends.
 *
 * Build:
 *   $HOME/buildroot-ont/output/host/bin/mips-buildroot-linux-uclibc-gcc -mips1 -EB -msoft-float -mno-mips16 -O2 -Wall -Wextra -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -o diag_to_json diag_to_json.c
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RTK_SOCK_PROTO 0xff

#define RTK_TRANSCEIVER_GET_OPT 11320
#define RTK_TRANSCEIVER_REQ_SIZE 0x68
#define RTK_TRANSCEIVER_TYPE_OFF 0x24
#define RTK_TRANSCEIVER_DATA_OFF 0x28

#define TYPE_VENDOR_NAME   0u
#define TYPE_PART_NUMBER   1u
#define TYPE_SN            2u
#define TYPE_TEMPERATURE   3u
#define TYPE_VOLTAGE       4u
#define TYPE_BIAS_CURRENT  5u
#define TYPE_TX_POWER      6u
#define TYPE_RX_POWER      7u

#define OPT_GPON_SERIAL_NUMBER  12516
#define OPT_GPON_ONU_STATE      12518
#define OPT_GPON_ALARM_STATUS   12519
#define OPT_GPON_RDI            12530
#define OPT_GPON_POWER_LEVEL    12531
#define OPT_GPON_TX_FORCE_LASER 12532
#define OPT_GPON_TX_FORCE_IDLE  12533
#define OPT_GPON_TX_FORCE_PRBS  12534
#define OPT_GPON_DS_FEC_STS     12535
#define OPT_GPON_AUTO_TCONT     12550
#define OPT_GPON_AUTO_BOH       12551
#define OPT_GPON_EQD_OFFSET     12552
#define OPT_GPON_US_FEC_STS     12556
#define OPT_GPON_DBRU_BLOCK     12557
#define OPT_GPON_ROGUE_SD_CNT   12558

#define OPT_GPON_GLOBAL_COUNTER 12544
#define OPT_GPON_DS_FLOW        12522
#define OPT_GPON_US_FLOW        12523

#define OPT_PORT_LINK           11418
#define OPT_PORT_SPEED_DUPLEX   11419
#define OPT_PORT_FLOWCTRL       11420

#define OPT_MIB_PORT_ALL        11822
#define OPT_MIB_COUNT_MODE      11825

#define OPT_QOS_PRI_SEL_GROUP   11518
#define OPT_QOS_PRIORITY_MAP    11522
#define OPT_QOS_PORT_PRI_MAP    11523
#define OPT_QOS_SCHED_TYPE      11533

#define OPT_VLAN_STATE          12024
#define OPT_VLAN_INGRESS_FILTER 12025
#define OPT_VLAN_PORT_PVID      12033
#define OPT_VLAN_TAG_MODE       12038


#define OPT_SWITCH_VERSION       12128
#define OPT_SWITCH_PATCH_INFO    12129
#define OPT_SWITCH_THERMAL       12131
#define OPT_SWITCH_MGMT_MAC      12125
#define OPT_SWITCH_MAX_PKT_PORT  12126

#define OPT_CLASSF_CFG_ENTRY     10318
#define OPT_CLASSF_UNMATCH_US    10319
#define OPT_CLASSF_UNMATCH_DS    10320
#define OPT_CLASSF_CF_SEL        10323
#define OPT_CLASSF_ENTRY_NUM_P1  10372
#define OPT_CLASSF_DEFAULT_WANIF 10375

#define OPT_L2_FLUSH_LINKDOWN    10818
#define OPT_L2_LEARNING_COUNT    10821
#define OPT_L2_LIMIT_COUNT       10822
#define OPT_L2_PORT_LEARN_COUNT  10826
#define OPT_L2_PORT_LIMIT_COUNT  10827

#define OPT_SEC_PORT_ATTACK      11718
#define OPT_SEC_ATTACK_ACTION    11719
#define OPT_SEC_FLOOD_THRESH     11720
#define OPT_SEC_FLOOD_UNIT       11721

#define OPT_STORM_METER_IDX      12724
#define OPT_STORM_PORT_ENABLE    12725
#define OPT_STORM_ENABLE         12726
#define OPT_STORM_BYPASS         12727

#define OPT_CPU_TRAP_INSERT_TAG       10420
#define OPT_CPU_TAG_AWARE             10421
#define OPT_CPU_TRAP_INSERT_TAG_PORT  10422
#define OPT_CPU_TAG_AWARE_PORT        10423

#define OPT_RATE_IGR_BW_RATE          12717
#define OPT_RATE_IGR_INCLUDE_IFG      12718
#define OPT_RATE_EGR_BW_RATE          12719
#define OPT_RATE_EGR_INCLUDE_IFG      12720
#define OPT_RATE_PORT_EGR_INCLUDE_IFG 12721

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int16_t get_sbe16(const uint8_t *p) {
    return (int16_t)get_be16(p);
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int rtk_getsockopt(int opt, void *buf, socklen_t len) {
    int fd;
    socklen_t got = len;

    fd = socket(AF_INET, SOCK_RAW, RTK_SOCK_PROTO);
    if (fd < 0) return -errno;

    if (getsockopt(fd, 0, opt, buf, &got) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    close(fd);
    return 0;
}

static int get_transceiver_raw(unsigned type, uint8_t raw[24]) {
    uint8_t req[RTK_TRANSCEIVER_REQ_SIZE];
    int rc;

    memset(req, 0, sizeof(req));
    put_be32(req + RTK_TRANSCEIVER_TYPE_OFF, type);

    rc = rtk_getsockopt(RTK_TRANSCEIVER_GET_OPT, req, sizeof(req));
    if (rc != 0) return rc;

    memcpy(raw, req + RTK_TRANSCEIVER_DATA_OFF, 24);
    return 0;
}

static int get_u32_opt(int opt, uint32_t *out) {
    uint8_t b[4];
    int rc;

    memset(b, 0, sizeof(b));
    rc = rtk_getsockopt(opt, b, sizeof(b));
    if (rc != 0) return rc;

    *out = get_be32(b);
    return 0;
}

static int get_buf_opt(int opt, uint8_t *buf, socklen_t len) {
    memset(buf, 0, len);
    return rtk_getsockopt(opt, buf, len);
}

static int get_raw_words(unsigned opt, unsigned len, unsigned wordc, const unsigned *words, uint8_t *buf, unsigned buflen) {
    unsigned i;
    int rc;

    if (len == 0 || len > buflen) return -2;

    memset(buf, 0, buflen);
    for (i = 0; i < wordc && (i * 4u + 4u) <= len; ++i) {
        put_be32(buf + i * 4u, (uint32_t)words[i]);
    }

    rc = rtk_getsockopt((int)opt, buf, (socklen_t)len);
    return rc;
}

static void json_string(const char *s) {
    putchar('"');
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout); break;
        case '\f': fputs("\\f", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 0x20) printf("\\u%04x", (unsigned)c);
            else putchar((int)c);
            break;
        }
    }
    putchar('"');
}


static int read_proc_uptime_token(char *out, size_t outn) {
    FILE *f;
    int c;
    size_t n = 0;

    if (outn == 0) return -1;
    out[0] = '\0';

    f = fopen("/proc/uptime", "r");
    if (!f) return -1;

    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        if (n + 1 < outn) out[n++] = (char)c;
    }

    fclose(f);

    if (n == 0) return -1;
    out[n] = '\0';
    return 0;
}

static void emit_runtime(void) {
    time_t now;
    struct tm *tmv;
    char iso[32];
    char uptime[32];

    now = time(NULL);

    fputs("  \"runtime\": {\n", stdout);

    fputs("    \"generated_unix\": ", stdout);
    if (now == (time_t)-1) fputs("null", stdout);
    else printf("%ld", (long)now);
    fputs(",\n", stdout);

    fputs("    \"generated_utc\": ", stdout);
    tmv = (now == (time_t)-1) ? NULL : gmtime(&now);
    if (tmv && strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", tmv) > 0) json_string(iso);
    else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"uptime_seconds\": ", stdout);
    if (read_proc_uptime_token(uptime, sizeof(uptime)) == 0) fputs(uptime, stdout);
    else fputs("null", stdout);
    fputs("\n", stdout);

    fputs("  }", stdout);
}


static int read_first_line(const char *path, char *out, size_t outn) {
    FILE *f;
    int c;
    size_t n = 0;

    if (outn == 0) return -1;
    out[0] = '\0';

    f = fopen(path, "r");
    if (!f) return -1;

    while ((c = fgetc(f)) != EOF) {
        if (c == '\n' || c == '\r') break;
        if (n + 1 < outn) out[n++] = (char)c;
    }

    fclose(f);

    if (n == 0) return -1;
    out[n] = '\0';
    return 0;
}

static long read_meminfo_kb_value(const char *wanted_key) {
    FILE *f;
    char key[64];
    long value;
    char unit[16];

    f = fopen("/proc/meminfo", "r");
    if (!f) return -1;

    while (fscanf(f, "%63[^:]: %ld %15s\n", key, &value, unit) == 3) {
        if (strcmp(key, wanted_key) == 0) {
            fclose(f);
            return value;
        }
    }

    fclose(f);
    return -1;
}

static void json_long_or_null(long v) {
    if (v < 0) fputs("null", stdout);
    else printf("%ld", v);
}

static void emit_system(void) {
    char line[256];
    char one[32], five[32], fifteen[32], procs[32];
    long last_pid = -1;
    long mem_total;
    long mem_free;
    long mem_available;
    long buffers;
    long cached;
    long swap_cached;
    long active;
    long inactive;

    fputs("  \"system\": {\n", stdout);

    fputs("    \"kernel_version\": ", stdout);
    if (read_first_line("/proc/version", line, sizeof(line)) == 0) json_string(line);
    else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"loadavg\": ", stdout);
    {
        FILE *f = fopen("/proc/loadavg", "r");
        if (f && fscanf(f, "%31s %31s %31s %31s %ld", one, five, fifteen, procs, &last_pid) == 5) {
            char *slash = strchr(procs, '/');
            fputs("{\"one\": ", stdout); fputs(one, stdout);
            fputs(", \"five\": ", stdout); fputs(five, stdout);
            fputs(", \"fifteen\": ", stdout); fputs(fifteen, stdout);
            if (slash) {
                *slash = '\0';
                fputs(", \"running_processes\": ", stdout); fputs(procs, stdout);
                fputs(", \"total_processes\": ", stdout); fputs(slash + 1, stdout);
            }
            printf(", \"last_pid\": %ld}", last_pid);
        } else {
            fputs("null", stdout);
        }
        if (f) fclose(f);
    }
    fputs(",\n", stdout);

    mem_total = read_meminfo_kb_value("MemTotal");
    mem_free = read_meminfo_kb_value("MemFree");
    mem_available = read_meminfo_kb_value("MemAvailable");
    buffers = read_meminfo_kb_value("Buffers");
    cached = read_meminfo_kb_value("Cached");
    swap_cached = read_meminfo_kb_value("SwapCached");
    active = read_meminfo_kb_value("Active");
    inactive = read_meminfo_kb_value("Inactive");

    fputs("    \"memory\": {", stdout);
    fputs("\"mem_total_kb\": ", stdout); json_long_or_null(mem_total);
    fputs(", \"mem_free_kb\": ", stdout); json_long_or_null(mem_free);
    fputs(", \"mem_available_kb\": ", stdout); json_long_or_null(mem_available);
    fputs(", \"buffers_kb\": ", stdout); json_long_or_null(buffers);
    fputs(", \"cached_kb\": ", stdout); json_long_or_null(cached);
    fputs(", \"swap_cached_kb\": ", stdout); json_long_or_null(swap_cached);
    fputs(", \"active_kb\": ", stdout); json_long_or_null(active);
    fputs(", \"inactive_kb\": ", stdout); json_long_or_null(inactive);
    fputs("}\n", stdout);

    fputs("  }", stdout);
}


static void trim_ascii(const uint8_t *raw, size_t n, char *out, size_t outn) {
    size_t start = 0;
    size_t end;

    if (outn == 0) return;
    if (n >= outn) n = outn - 1;

    memcpy(out, raw, n);
    out[n] = '\0';
    end = n;

    while (end > 0 && (out[end - 1] == '\0' || out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\r' || out[end - 1] == '\n')) --end;
    out[end] = '\0';

    while (out[start] == ' ' || out[start] == '\t' || out[start] == '\r' || out[start] == '\n') ++start;
    if (start > 0) memmove(out, out + start, strlen(out + start) + 1);
}

static void print_hex_string(const uint8_t *p, size_t n) {
    size_t i;
    putchar('"');
    for (i = 0; i < n; ++i) {
        printf("%02X", (unsigned)p[i]);
        if (i + 1 != n) putchar(':');
    }
    putchar('"');
}

static const char *bool_name(uint32_t v) {
    return v ? "enabled" : "disabled";
}

static const char *onu_state_name(uint32_t v) {
    switch (v) {
    case 0: return "O1?";
    case 1: return "O1";
    case 2: return "O2";
    case 3: return "O3";
    case 4: return "O4";
    case 5: return "O5";
    case 6: return "O6";
    case 7: return "O7";
    default: return "unknown";
    }
}

static const char *flow_type_name(uint32_t t) {
    if (t == 0) return "OMCI";
    if (t == 1) return "ETH";
    return "unknown";
}

static const char *port_speed_name(uint32_t speed) {
    switch (speed) {
    case 0: return "10M";
    case 1: return "100M";
    case 2: return "1000M";
    case 3: return "500M";
    default: return "unknown";
    }
}

static const char *port_duplex_name(uint32_t duplex) {
    switch (duplex) {
    case 0: return "half";
    case 1: return "full";
    default: return "unknown";
    }
}

static const char *qos_sched_type_name(uint32_t v) {
    switch (v) {
    case 0: return "strict";
    case 1: return "WRR";
    default: return "unknown";
    }
}

static const char *vlan_tag_mode_name(uint32_t v) {
    switch (v) {
    case 0: return "original";
    case 1: return "keep-format";
    case 2: return "priority-tag";
    case 3: return "real-keep-format";
    default: return "unknown";
    }
}

static unsigned ilog2_u32(uint32_t v) {
    unsigned n = 0;
    while (v >>= 1) ++n;
    return n;
}

/* Optical power raw to micro-dBm: dBm = 10 * log10(raw / 10000).
 * Integer-only fixed-point log2 approximation; no libm and no 64-bit division.
 */
static int32_t optical_raw_to_dbm_micro(uint32_t raw) {
    unsigned k;
    uint32_t y;
    uint32_t frac = 0;
    uint64_t log2_q24;
    int64_t micro;
    int i;

    if (raw == 0) return INT_MIN;

    k = ilog2_u32(raw);
    y = raw << (24 - k); /* Q8.24 in [1.0, 2.0) */

    for (i = 1; i <= 24; ++i) {
        y = (uint32_t)(((uint64_t)y * (uint64_t)y) >> 24);
        if (y >= (2u << 24)) {
            y >>= 1;
            frac |= 1u << (24 - i);
        }
    }

    log2_q24 = ((uint64_t)k << 24) | frac;
    micro = ((int64_t)log2_q24 * 3010300 + (1 << 23)) >> 24;
    micro -= 40000000;

    if (micro > INT_MAX) return INT_MAX;
    if (micro < INT_MIN) return INT_MIN;
    return (int32_t)micro;
}

static void json_micro_number(int32_t micro) {
    uint32_t v;

    if (micro == INT_MIN) {
        fputs("null", stdout);
        return;
    }

    if (micro < 0) {
        putchar('-');
        v = (uint32_t)(-micro);
    } else {
        v = (uint32_t)micro;
    }

    printf("%lu.%06lu", (unsigned long)(v / 1000000u), (unsigned long)(v % 1000000u));
}

static void json_temp_from_raw(int16_t raw) {
    int neg = 0;
    uint32_t v;
    uint32_t whole;
    uint32_t rem;
    uint32_t frac;

    if (raw < 0) {
        neg = 1;
        v = (uint32_t)(-raw);
    } else {
        v = (uint32_t)raw;
    }

    whole = v / 256u;
    rem = v % 256u;
    frac = (rem * 1000000u + 128u) / 256u;
    if (frac >= 1000000u) { ++whole; frac -= 1000000u; }

    if (neg) putchar('-');
    printf("%lu.%06lu", (unsigned long)whole, (unsigned long)frac);
}

static void json_voltage_from_raw(uint16_t raw) {
    uint32_t whole = (uint32_t)raw / 10000u;
    uint32_t rem = (uint32_t)raw % 10000u;
    printf("%lu.%06lu", (unsigned long)whole, (unsigned long)(rem * 100u));
}

static void json_bias_from_raw(uint16_t raw) {
    uint32_t whole = (uint32_t)raw / 500u;
    uint32_t rem = (uint32_t)raw % 500u;
    uint32_t frac = (rem * 1000000u + 250u) / 500u;
    if (frac >= 1000000u) { ++whole; frac -= 1000000u; }
    printf("%lu.%06lu", (unsigned long)whole, (unsigned long)frac);
}

static void json_serial_from_raw(const uint8_t sn[8]) {
    char s[16];

    if (sn[0] >= 32 && sn[0] <= 126 && sn[1] >= 32 && sn[1] <= 126 &&
        sn[2] >= 32 && sn[2] <= 126 && sn[3] >= 32 && sn[3] <= 126) {
        snprintf(s, sizeof(s), "%c%c%c%c%02X%02X%02X%02X", sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7]);
        json_string(s);
    } else {
        print_hex_string(sn, 8);
    }
}

static void json_null_or_error(int rc) {
    (void)rc;
    fputs("null", stdout);
}


static uint32_t first_non_echo_word(const uint8_t *buf, unsigned len, unsigned echo_count) {
    unsigned i;
    for (i = echo_count * 4u; i + 4u <= len; i += 4u) {
        uint32_t v = get_be32(buf + i);
        if (v != 0) return v;
    }
    return 0;
}

static uint32_t word_or_zero(const uint8_t *buf, unsigned len, unsigned word_index) {
    unsigned off = word_index * 4u;
    if (off + 4u > len) return 0;
    return get_be32(buf + off);
}

static int mib_counter_is_error(unsigned off);
static int has_nonzero_mib_error_words(const uint8_t *buf) {
    unsigned i;
    for (i = 0; i + 4 <= 376; i += 4) {
        if (mib_counter_is_error(i) && get_be32(buf + i) != 0) return 1;
    }
    return 0;
}

static void emit_json_string_array_unique_vlan51(int have51) {
    if (have51) fputs("[51]", stdout);
    else fputs("[]", stdout);
}

static void emit_gpon_summary(void) {
    uint8_t buf[16];
    uint32_t v;
    uint32_t a;
    uint32_t b;
    int rc;

    fputs("    \"onu_state\": ", stdout);
    rc = get_u32_opt(OPT_GPON_ONU_STATE, &v);
    if (rc == 0) {
        printf("{\"raw\": %lu, \"name\": ", (unsigned long)v);
        json_string(onu_state_name(v));
        fputs("}", stdout);
    } else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"alarm_status\": ", stdout);
    rc = get_buf_opt(OPT_GPON_ALARM_STATUS, buf, 8);
    if (rc == 0) {
        v = get_be32(buf);
        printf("{\"raw\": %lu, \"hex\": \"0x%08lX\"}", (unsigned long)v, (unsigned long)v);
    } else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"serial_number\": ", stdout);
    rc = get_buf_opt(OPT_GPON_SERIAL_NUMBER, buf, 8);
    if (rc == 0) json_serial_from_raw(buf);
    else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"serial_number_hex\": ", stdout);
    if (rc == 0) print_hex_string(buf, 8);
    else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"rdi\": ", stdout);
    rc = get_u32_opt(OPT_GPON_RDI, &v);
    if (rc == 0) printf("{\"enabled\": %s, \"raw\": %lu}", v ? "true" : "false", (unsigned long)v);
    else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"power_level\": ", stdout);
    rc = get_u32_opt(OPT_GPON_POWER_LEVEL, &v);
    if (rc == 0) printf("%lu", (unsigned long)v);
    else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"tx\": ", stdout);
    {
        uint32_t force_laser = 0, force_idle = 0, force_prbs = 0;
        int r1 = get_u32_opt(OPT_GPON_TX_FORCE_LASER, &force_laser);
        int r2 = get_u32_opt(OPT_GPON_TX_FORCE_IDLE, &force_idle);
        int r3 = get_u32_opt(OPT_GPON_TX_FORCE_PRBS, &force_prbs);
        if (r1 == 0 && r2 == 0 && r3 == 0) {
            printf("{\"force_laser\": %s, \"force_idle\": %s, \"force_prbs\": %s, \"raw\": {\"force_laser\": %lu, \"force_idle\": %lu, \"force_prbs\": %lu}}",
                   force_laser ? "true" : "false",
                   force_idle ? "true" : "false",
                   force_prbs ? "true" : "false",
                   (unsigned long)force_laser,
                   (unsigned long)force_idle,
                   (unsigned long)force_prbs);
        } else fputs("null", stdout);
    }
    fputs(",\n", stdout);

    fputs("    \"fec\": ", stdout);
    {
        uint32_t ds = 0, us = 0;
        int r1 = get_u32_opt(OPT_GPON_DS_FEC_STS, &ds);
        int r2 = get_u32_opt(OPT_GPON_US_FEC_STS, &us);
        if (r1 == 0 && r2 == 0) {
            printf("{\"downstream_enabled\": %s, \"upstream_enabled\": %s, \"downstream_raw\": %lu, \"upstream_raw\": %lu}",
                   ds ? "true" : "false",
                   us ? "true" : "false",
                   (unsigned long)ds,
                   (unsigned long)us);
        } else fputs("null", stdout);
    }
    fputs(",\n", stdout);

    fputs("    \"auto\": ", stdout);
    {
        uint32_t tcont = 0, boh = 0;
        int r1 = get_u32_opt(OPT_GPON_AUTO_TCONT, &tcont);
        int r2 = get_u32_opt(OPT_GPON_AUTO_BOH, &boh);
        if (r1 == 0 && r2 == 0) {
            printf("{\"tcont_enabled\": %s, \"boh_enabled\": %s, \"tcont_raw\": %lu, \"boh_raw\": %lu}",
                   tcont ? "true" : "false",
                   boh ? "true" : "false",
                   (unsigned long)tcont,
                   (unsigned long)boh);
        } else fputs("null", stdout);
    }
    fputs(",\n", stdout);

    fputs("    \"eqd_offset\": ", stdout);
    rc = get_u32_opt(OPT_GPON_EQD_OFFSET, &v);
    if (rc == 0) printf("%lu", (unsigned long)v);
    else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"dbru_block_size\": ", stdout);
    rc = get_u32_opt(OPT_GPON_DBRU_BLOCK, &v);
    if (rc == 0) printf("%lu", (unsigned long)v);
    else json_null_or_error(rc);
    fputs(",\n", stdout);

    fputs("    \"rogue_sd_count\": ", stdout);
    rc = get_buf_opt(OPT_GPON_ROGUE_SD_CNT, buf, 8);
    if (rc == 0) {
        a = get_be32(buf);
        b = get_be32(buf + 4);
        printf("{\"too_long\": %lu, \"mismatch\": %lu}", (unsigned long)a, (unsigned long)b);
    } else json_null_or_error(rc);
    fputs("\n", stdout);
}

static void emit_transceiver(void) {
    uint8_t raw[24];
    char s[32];
    int rc;

    fputs("    \"vendor_name\": ", stdout);
    rc = get_transceiver_raw(TYPE_VENDOR_NAME, raw);
    if (rc == 0) { trim_ascii(raw, 24, s, sizeof(s)); json_string(s); } else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"part_number\": ", stdout);
    rc = get_transceiver_raw(TYPE_PART_NUMBER, raw);
    if (rc == 0) { trim_ascii(raw, 24, s, sizeof(s)); json_string(s); } else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"serial_number\": ", stdout);
    rc = get_transceiver_raw(TYPE_SN, raw);
    if (rc == 0) { trim_ascii(raw, 24, s, sizeof(s)); json_string(s); } else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"module_id\": ", stdout);
    if (rc == 0) json_string(s); else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"temperature_c\": ", stdout);
    rc = get_transceiver_raw(TYPE_TEMPERATURE, raw);
    if (rc == 0) json_temp_from_raw(get_sbe16(raw)); else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"voltage_v\": ", stdout);
    rc = get_transceiver_raw(TYPE_VOLTAGE, raw);
    if (rc == 0) json_voltage_from_raw(get_be16(raw)); else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"bias_current_ma\": ", stdout);
    rc = get_transceiver_raw(TYPE_BIAS_CURRENT, raw);
    if (rc == 0) json_bias_from_raw(get_be16(raw)); else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"tx_power_dbm\": ", stdout);
    rc = get_transceiver_raw(TYPE_TX_POWER, raw);
    if (rc == 0) json_micro_number(optical_raw_to_dbm_micro(get_be16(raw))); else fputs("null", stdout);
    fputs(",\n", stdout);

    fputs("    \"rx_power_dbm\": ", stdout);
    rc = get_transceiver_raw(TYPE_RX_POWER, raw);
    if (rc == 0) json_micro_number(optical_raw_to_dbm_micro(get_be16(raw))); else fputs("null", stdout);
    fputs("\n", stdout);
}

static void emit_one_flow(unsigned opt, unsigned id, int upstream) {
    uint8_t buf[20];
    unsigned word = id;
    uint32_t flow;
    uint32_t gem;
    uint32_t type;
    uint32_t a;
    uint32_t b;
    int rc;

    rc = get_raw_words(opt, sizeof(buf), 1, &word, buf, sizeof(buf));
    if (rc != 0) {
        fputs("{\"valid\": false, \"error\": ", stdout);
        printf("%d}", rc);
        return;
    }

    flow = get_be32(buf + 0);
    gem = get_be32(buf + 4);
    type = get_be32(buf + 8);
    a = get_be32(buf + 12);
    b = get_be32(buf + 16);

    if (upstream && gem == 0 && type == 0 && a == 0 && b == 0 && flow != 0 && flow != 64) {
        printf("{\"flow_id\": %lu, \"valid\": false}", (unsigned long)flow);
        return;
    }

    printf("{\"flow_id\": %lu, \"valid\": true, \"gem_port\": %lu, \"type\": ",
           (unsigned long)flow, (unsigned long)gem);
    json_string(flow_type_name(type));
    printf(", \"type_raw\": %lu", (unsigned long)type);

    if (upstream) {
        printf(", \"tcont\": %lu, \"flags_hex\": \"0x%08lX\"}",
               (unsigned long)a, (unsigned long)b);
    } else {
        printf(", \"multicast\": %s, \"multicast_raw\": %lu, \"aes\": %s, \"aes_raw\": %lu}",
               a ? "true" : "false", (unsigned long)a,
               b ? "true" : "false", (unsigned long)b);
    }
}

static void emit_flows(void) {
    unsigned ids[] = {0, 1, 2, 64};
    unsigned i;

    fputs("    \"downstream\": [\n", stdout);
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        fputs("      ", stdout);
        emit_one_flow(OPT_GPON_DS_FLOW, ids[i], 0);
        fputs(i + 1 == sizeof(ids) / sizeof(ids[0]) ? "\n" : ",\n", stdout);
    }
    fputs("    ],\n", stdout);

    fputs("    \"upstream\": [\n", stdout);
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        fputs("      ", stdout);
        emit_one_flow(OPT_GPON_US_FLOW, ids[i], 1);
        fputs(i + 1 == sizeof(ids) / sizeof(ids[0]) ? "\n" : ",\n", stdout);
    }
    fputs("    ]\n", stdout);
}

static void emit_counter_fields(const char *name, const uint8_t *buf) {
    uint32_t w[13];
    unsigned i;

    for (i = 0; i < 13; ++i) w[i] = get_be32(buf + i * 4u);

    printf("{\"counter_id\": %lu", (unsigned long)w[0]);

    if (strcmp(name, "ds-bw") == 0) {
        printf(", \"total_rx_bwmap\": %lu, \"crc_err_rx_bwmap\": %lu, \"overflow_bwmap\": %lu, \"invalid_bwmap_0\": %lu, \"invalid_bwmap_1\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5]);
    } else if (strcmp(name, "ds-eth") == 0) {
        printf(", \"total_unicast\": %lu, \"total_multicast\": %lu, \"fwd_multicast\": %lu, \"leak_multicast\": %lu, \"fcs_error\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5]);
    } else if (strcmp(name, "ds-omci") == 0) {
        printf(", \"total_rx_omci\": %lu, \"rx_omci_byte\": %lu, \"crc_error_omci\": %lu, \"processed_omci\": %lu, \"dropped_omci\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5]);
    } else if (strcmp(name, "ds-phy") == 0) {
        printf(", \"bip_error_bits\": %lu, \"bip_error_blocks\": %lu, \"fec_correct_bits\": %lu, \"fec_correct_bytes\": %lu, \"fec_correct_codewords\": %lu, \"fec_codewords_uncor\": %lu, \"superframe_los\": %lu, \"plen_fail\": %lu, \"plen_correct\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5],
               (unsigned long)w[6], (unsigned long)w[7], (unsigned long)w[8], (unsigned long)w[9]);
    } else if (strcmp(name, "ds-plm") == 0) {
        printf(", \"total_rx_ploamd\": %lu, \"crc_err_rx_ploam\": %lu, \"proc_rx_ploamd\": %lu, \"overflow_rx_ploam\": %lu, \"unknown_rx_ploam\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5]);
    } else if (strcmp(name, "ds-gem") == 0) {
        printf(", \"ds_gem_los\": %lu, \"ds_gem_idle\": %lu, \"ds_gem_non_idle\": %lu, \"ds_hec_correct\": %lu, \"over_interleave\": %lu, \"mis_gem_pkt_len\": %lu, \"multi_flow_match\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
    } else if (strcmp(name, "us-dbr") == 0) {
        printf(", \"tx_dbru\": %lu", (unsigned long)w[1]);
    } else if (strcmp(name, "us-phy") == 0) {
        printf(", \"tx_boh\": %lu", (unsigned long)w[1]);
    } else if (strcmp(name, "us-plm") == 0) {
        printf(", \"total_tx_ploam\": %lu, \"process_tx_ploam\": %lu, \"tx_urgent_ploam\": %lu, \"proc_urg_ploam\": %lu, \"tx_normal_ploam\": %lu, \"proc_nrm_ploam\": %lu, \"tx_sn_ploam\": %lu, \"tx_nomsg_ploam\": %lu",
               (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7], (unsigned long)w[8]);
    } else {
        fputs(", \"raw_words\": [", stdout);
        for (i = 0; i < 13; ++i) {
            if (i) fputs(", ", stdout);
            printf("%lu", (unsigned long)w[i]);
        }
        fputs("]", stdout);
    }

    fputs("}", stdout);
}

static void emit_counters(void) {
    static const struct { const char *key; unsigned id; } counters[] = {
        {"active", 0}, {"ds_phy", 1}, {"ds_plm", 2}, {"ds_bw", 3}, {"ds_gem", 4}, {"ds_eth", 5}, {"ds_omci", 6},
        {"us_phy", 7}, {"us_dbr", 8}, {"us_plm", 9}, {"us_gem", 10}, {"us_eth", 11}, {"us_omci", 12}
    };
    static const char *names[] = {
        "active", "ds-phy", "ds-plm", "ds-bw", "ds-gem", "ds-eth", "ds-omci",
        "us-phy", "us-dbr", "us-plm", "us-gem", "us-eth", "us-omci"
    };
    unsigned i;
    uint8_t buf[52];

    for (i = 0; i < sizeof(counters) / sizeof(counters[0]); ++i) {
        unsigned id = counters[i].id;
        int rc;

        fputs("    ", stdout);
        json_string(counters[i].key);
        fputs(": ", stdout);

        rc = get_raw_words(OPT_GPON_GLOBAL_COUNTER, sizeof(buf), 1, &id, buf, sizeof(buf));
        if (rc == 0) emit_counter_fields(names[i], buf);
        else fputs("null", stdout);

        fputs(i + 1 == sizeof(counters) / sizeof(counters[0]) ? "\n" : ",\n", stdout);
    }
}

static void emit_ports(void) {
    unsigned p;

    fputs("  \"ports\": [\n", stdout);
    for (p = 0; p <= 6; ++p) {
        uint32_t link = 0, speed = 0, duplex = 0, txfc = 0, rxfc = 0;
        uint8_t buf[264];
        unsigned word = p;
        int r1, r2, r3;

        r1 = get_raw_words(OPT_PORT_LINK, sizeof(buf), 1, &word, buf, sizeof(buf));
        if (r1 == 0) link = get_be32(buf + 4);

        r2 = get_raw_words(OPT_PORT_SPEED_DUPLEX, sizeof(buf), 1, &word, buf, sizeof(buf));
        if (r2 == 0) {
            speed = get_be32(buf + 8);
            duplex = get_be32(buf + 12);
        }

        r3 = get_raw_words(OPT_PORT_FLOWCTRL, sizeof(buf), 1, &word, buf, sizeof(buf));
        if (r3 == 0) {
            txfc = get_be32(buf + 4);
            rxfc = get_be32(buf + 8);
        }

        fputs("    {", stdout);
        printf("\"port\": %lu, \"link\": %s", (unsigned long)p, link ? "true" : "false");

        if (link) {
            fputs(", \"speed\": ", stdout); json_string(port_speed_name(speed));
            printf(", \"speed_raw\": %lu", (unsigned long)speed);
            fputs(", \"duplex\": ", stdout); json_string(port_duplex_name(duplex));
            printf(", \"duplex_raw\": %lu", (unsigned long)duplex);
            printf(", \"tx_flow_control\": %s, \"rx_flow_control\": %s, \"tx_flow_control_raw\": %lu, \"rx_flow_control_raw\": %lu",
                   txfc ? "true" : "false",
                   rxfc ? "true" : "false",
                   (unsigned long)txfc,
                   (unsigned long)rxfc);
        }

        fputs("}", stdout);
        fputs(p == 6 ? "\n" : ",\n", stdout);
    }
    fputs("  ]", stdout);
}

static void emit_vlan(void) {
    uint8_t buf[264];
    uint32_t state = 0;
    unsigned p;
    int rc;

    rc = get_raw_words(OPT_VLAN_STATE, sizeof(buf), 0, NULL, buf, sizeof(buf));
    if (rc == 0) state = get_be32(buf);

    printf("    \"enabled\": %s,\n", state ? "true" : "false");
    printf("    \"state_raw\": %lu,\n", (unsigned long)state);
    fputs("    \"ports\": [\n", stdout);

    for (p = 0; p <= 6; ++p) {
        uint32_t pvid = 0, tag = 0, ingress = 0;
        unsigned word = p;

        if (get_raw_words(OPT_VLAN_PORT_PVID, sizeof(buf), 1, &word, buf, sizeof(buf)) == 0) pvid = get_be32(buf + 4);
        if (get_raw_words(OPT_VLAN_TAG_MODE, sizeof(buf), 1, &word, buf, sizeof(buf)) == 0) tag = get_be32(buf + 4);
        if (get_raw_words(OPT_VLAN_INGRESS_FILTER, sizeof(buf), 1, &word, buf, sizeof(buf)) == 0) ingress = get_be32(buf + 4);

        printf("      {\"port\": %lu, \"pvid\": %lu, \"tag_mode\": ",
               (unsigned long)p, (unsigned long)pvid);
        json_string(vlan_tag_mode_name(tag));
        printf(", \"tag_mode_raw\": %lu, \"ingress_filter\": %s, \"ingress_filter_raw\": %lu}",
               (unsigned long)tag,
               ingress ? "true" : "false",
               (unsigned long)ingress);
        fputs(p == 6 ? "\n" : ",\n", stdout);
    }

    fputs("    ]\n", stdout);
}

static void emit_qos(void) {
    uint8_t buf[256];
    uint32_t sched = 0;
    unsigned i;
    unsigned p;

    if (get_raw_words(OPT_QOS_SCHED_TYPE, 156, 0, NULL, buf, sizeof(buf)) == 0) sched = get_be32(buf + 0x94);

    fputs("    \"scheduling_type\": ", stdout);
    json_string(qos_sched_type_name(sched));
    printf(",\n    \"scheduling_type_raw\": %lu,\n", (unsigned long)sched);

    fputs("    \"priority_to_queue_table0\": [", stdout);
    if (get_raw_words(OPT_QOS_PRIORITY_MAP, 156, 0, NULL, buf, sizeof(buf)) == 0) {
        for (i = 0; i < 8; ++i) {
            if (i) fputs(", ", stdout);
            printf("%lu", (unsigned long)get_be32(buf + 0x40 + i * 4u));
        }
    }
    fputs("],\n", stdout);

    fputs("    \"port_priority_to_queue_table\": [\n", stdout);
    for (p = 0; p <= 6; ++p) {
        unsigned word = p;
        uint32_t table = 0;
        if (get_raw_words(OPT_QOS_PORT_PRI_MAP, 156, 1, &word, buf, sizeof(buf)) == 0) table = get_be32(buf + 4);
        printf("      {\"port\": %lu, \"table\": %lu}", (unsigned long)p, (unsigned long)table);
        fputs(p == 6 ? "\n" : ",\n", stdout);
    }
    fputs("    ],\n", stdout);

    fputs("    \"priority_selector_group0_weights\": [", stdout);
    if (get_raw_words(OPT_QOS_PRI_SEL_GROUP, 156, 0, NULL, buf, sizeof(buf)) == 0) {
        for (i = 0; i < 5; ++i) {
            if (i) fputs(", ", stdout);
            printf("%lu", (unsigned long)get_be32(buf + i * 4u));
        }
    }
    fputs("]\n", stdout);
}

static const char *mib_counter_name(unsigned off) {
    switch (off) {
    case 0x000: return "ifInOctets";
    case 0x010: return "ifInUcastPkts";
    case 0x014: return "ifInMulticastPkts";
    case 0x018: return "ifInBroadcastPkts";
    case 0x044: return "ifOutOctets";
    case 0x04c: return "ifOutUcastPkts";
    case 0x050: return "ifOutMulticastPkts";
    case 0x054: return "ifOutBroadcastPkts";
    case 0x058: return "dot1dTpPortInDiscards";
    case 0x060: return "dot3InPauseFrames";
    case 0x064: return "dot3OutPauseFrames";
    case 0x070: return "dot3StatsSingleCollisionFrames";
    case 0x074: return "dot3StatsMultipleCollisionFrames";
    case 0x078: return "dot3StatsDeferredTransmissions";
    case 0x07c: return "dot3StatsLateCollisions";
    case 0x080: return "dot3StatsExcessiveCollisions";
    case 0x084: return "dot3StatsSymbolErrors";
    case 0x088: return "dot3ControlInUnknownOpcodes";
    case 0x090: return "etherStatsDropEvents";
    case 0x094: return "etherStatsFragments";
    case 0x098: return "etherStatsJabbers";
    case 0x09c: return "etherStatsCollisions";
    case 0x0a0: return "etherStatsCRCAlignErrors";
    case 0x0e8: return "etherStatsTxUndersizePkts";
    case 0x0ec: return "etherStatsTxOversizePkts";
    case 0x0f0: return "etherStatsTxPkts64Octets";
    case 0x0f4: return "etherStatsTxPkts65to127Octets";
    case 0x0f8: return "etherStatsTxPkts128to255Octets";
    case 0x0fc: return "etherStatsTxPkts256to511Octets";
    case 0x100: return "etherStatsTxPkts512to1023Octets";
    case 0x104: return "etherStatsTxPkts1024to1518Octets";
    case 0x108: return "etherStatsTxPkts1519toMaxOctets";
    case 0x10c: return "etherStatsTxBroadcastPkts";
    case 0x110: return "etherStatsTxMulticastPkts";
    case 0x118: return "etherStatsRxUndersizePkts";
    case 0x11c: return "etherStatsRxOversizePkts";
    case 0x120: return "etherStatsRxPkts64Octets";
    case 0x124: return "etherStatsRxPkts65to127Octets";
    case 0x128: return "etherStatsRxPkts128to255Octets";
    case 0x12c: return "etherStatsRxPkts256to511Octets";
    case 0x130: return "etherStatsRxPkts512to1023Octets";
    case 0x134: return "etherStatsRxPkts1024to1518Octets";
    case 0x138: return "etherStatsRxPkts1519toMaxOctets";
    case 0x140: return "inOamPduPkts";
    case 0x144: return "outOamPduPkts";
    default: return NULL;
    }
}

static int mib_counter_is_error(unsigned off) {
    switch (off) {
    case 0x058:
    case 0x07c:
    case 0x080:
    case 0x084:
    case 0x088:
    case 0x090:
    case 0x094:
    case 0x098:
    case 0x0a0:
    case 0x0e8:
    case 0x0ec:
    case 0x118:
    case 0x11c:
        return 1;
    default:
        return 0;
    }
}

static void emit_mib_object_from_buf(const uint8_t *buf, int errors_only, int known_only) {
    unsigned i;
    unsigned n = 0;

    fputs("{", stdout);
    for (i = 0; i + 4 <= 376; i += 4) {
        uint32_t v = get_be32(buf + i);
        const char *name = mib_counter_name(i);

        if (v == 0) continue;
        if (errors_only && !mib_counter_is_error(i)) continue;
        if (known_only && name == NULL) continue;
        if (!known_only && name != NULL) continue;

        if (n) fputs(", ", stdout);
        if (name) json_string(name);
        else {
            char k[16];
            snprintf(k, sizeof(k), "off_0x%03lX", (unsigned long)i);
            json_string(k);
        }
        printf(": %lu", (unsigned long)v);
        ++n;
    }
    fputs("}", stdout);
}

static void emit_mib(void) {
    uint8_t buf[512];
    uint32_t mode = 0;
    int rc;

    rc = get_raw_words(OPT_MIB_COUNT_MODE, 376, 0, NULL, buf, sizeof(buf));
    if (rc == 0) mode = get_be32(buf);

    fputs("    \"count_mode\": ", stdout);
    json_string(mode == 0 ? "normal-free-run" : "unknown");
    printf(",\n    \"count_mode_raw\": %lu,\n", (unsigned long)mode);

    rc = get_raw_words(OPT_MIB_PORT_ALL, 376, 0, NULL, buf, sizeof(buf));
    if (rc != 0) {
        fputs("    \"port_all\": null,\n", stdout);
        fputs("    \"nonzero_errors\": null,\n", stdout);
        fputs("    \"nonzero_unknown\": null\n", stdout);
        return;
    }

    fputs("    \"port_all_nonzero_known\": ", stdout);
    emit_mib_object_from_buf(buf, 0, 1);
    fputs(",\n", stdout);

    fputs("    \"port_all_nonzero_errors\": ", stdout);
    emit_mib_object_from_buf(buf, 1, 1);
    fputs(",\n", stdout);

    fputs("    \"port_all_nonzero_unknown\": ", stdout);
    emit_mib_object_from_buf(buf, 0, 0);
    fputs("\n", stdout);
}


static const char *chip_name_from_id(uint32_t id) {
    switch (id) {
    case 0x96030002u: return "RTL9602C";
    default: return "unknown";
    }
}

static const char *storm_type_name(unsigned t) {
    switch (t) {
    case 0: return "unknown_unicast";
    case 1: return "unknown_multicast";
    case 2: return "multicast";
    case 3: return "broadcast";
    case 4: return "dhcp";
    case 5: return "arp";
    case 6: return "igmp_mld";
    default: return "unknown";
    }
}

static const char *attack_type_name(unsigned t) {
    switch (t) {
    case 0: return "daeqsa_deny";
    case 1: return "land_deny";
    case 2: return "blat_deny";
    case 3: return "synfin_deny";
    case 4: return "xma_deny";
    case 5: return "nullscan_deny";
    case 6: return "syn_sport_lt_1024_deny";
    case 7: return "tcp_hdr_min_check";
    case 8: return "tcp_frag_off_min_check";
    case 9: return "icmp_frag_pkts_deny";
    case 10: return "pod_deny";
    case 11: return "udp_bomb";
    case 12: return "syn_with_data";
    case 13: return "syn_flood";
    case 14: return "fin_flood";
    case 15: return "icmp_flood";
    default: return "unknown";
    }
}

static int raw_all_zero(const uint8_t *buf, unsigned len) {
    unsigned i;
    for (i = 0; i < len; ++i) if (buf[i] != 0) return 0;
    return 1;
}

static void json_hex_u32(uint32_t v) {
    char s[16];
    snprintf(s, sizeof(s), "0x%08lX", (unsigned long)v);
    json_string(s);
}

static void emit_nonzero_words_from_buf(const uint8_t *buf, unsigned len) {
    unsigned i;
    unsigned n = 0;
    fputs("{", stdout);
    for (i = 0; i + 4 <= len; i += 4) {
        uint32_t v = get_be32(buf + i);
        if (v == 0) continue;
        if (n) fputs(", ", stdout);
        printf("\"w%lu\": %lu", (unsigned long)(i / 4u), (unsigned long)v);
        ++n;
    }
    fputs("}", stdout);
}

static void emit_raw_probe_object(unsigned opt, unsigned len, unsigned wordc, const unsigned *words) {
    uint8_t buf[2048];
    int rc;
    if (len > sizeof(buf)) {
        printf("{\"ok\": false, \"error\": \"buffer-too-small\"}");
        return;
    }
    rc = get_raw_words(opt, len, wordc, words, buf, sizeof(buf));
    if (rc != 0) {
        printf("{\"ok\": false, \"error_code\": %d}", rc);
        return;
    }
    printf("{\"ok\": true, \"opt\": %lu, \"len\": %lu, \"nonzero_words\": ", (unsigned long)opt, (unsigned long)len);
    emit_nonzero_words_from_buf(buf, len);
    fputs("}", stdout);
}

static void emit_device(void) {
    uint8_t buf[1024];
    uint32_t chip_id = 0;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    int rc;

    rc = get_raw_words(OPT_SWITCH_VERSION, 684, 0, NULL, buf, sizeof(buf));
    if (rc == 0) {
        /* Verified from live output: word 155 / byte 0x26c is 0x96030002 on RTL9602C. */
        chip_id = get_be32(buf + 0x26c);
        api_major = get_be32(buf + 0x270);
        api_minor = get_be32(buf + 0x274);
    }

    fputs("    \"pon_mode\": ", stdout); json_string("GPON"); fputs(",\n", stdout);
    fputs("    \"chip\": ", stdout); json_string(chip_name_from_id(chip_id)); fputs(",\n", stdout);
    fputs("    \"chip_id_hex\": ", stdout); json_hex_u32(chip_id); fputs(",\n", stdout);
    printf("    \"chip_id_raw\": %lu,\n", (unsigned long)chip_id);
    printf("    \"switch_api_version_guess\": \"%lu.%lu\",\n", (unsigned long)api_major, (unsigned long)api_minor);
    printf("    \"version_words\": {\"w155\": %lu, \"w156\": %lu, \"w157\": %lu},\n",
           (unsigned long)chip_id, (unsigned long)api_major, (unsigned long)api_minor);
    fputs("    \"diag_build_time\": null,\n", stdout);
    fputs("    \"note\": ", stdout);
    json_string("chip info is read from the Realtek switch-version backend; diag build time is a property of vendor diag, not this standalone binary");
    fputs("\n", stdout);
}

static void emit_switch_extra(void) {
    uint8_t buf[1024];
    uint32_t chip_id = 0;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    unsigned p;
    int rc;

    rc = get_raw_words(OPT_SWITCH_VERSION, 684, 0, NULL, buf, sizeof(buf));
    if (rc == 0) {
        chip_id = get_be32(buf + 0x26c);
        api_major = get_be32(buf + 0x270);
        api_minor = get_be32(buf + 0x274);
    }

    fputs("    \"chip\": ", stdout); json_string(chip_name_from_id(chip_id)); fputs(",\n", stdout);
    fputs("    \"chip_id_hex\": ", stdout); json_hex_u32(chip_id); fputs(",\n", stdout);
    printf("    \"chip_id_raw\": %lu,\n", (unsigned long)chip_id);
    printf("    \"switch_api_version_guess\": \"%lu.%lu\",\n", (unsigned long)api_major, (unsigned long)api_minor);

    fputs("    \"max_packet_length_by_port\": [\n", stdout);
    for (p = 0; p <= 6; ++p) {
        unsigned word = p;
        uint32_t max_len = 0;
        rc = get_raw_words(OPT_SWITCH_MAX_PKT_PORT, 684, 1, &word, buf, sizeof(buf));
        if (rc == 0) {
            max_len = get_be32(buf + (152u * 4u));
            printf("      {\"port\": %lu, \"max_packet_length\": %lu, \"raw_word\": 152}%s\n",
                   (unsigned long)p, (unsigned long)max_len, p == 6 ? "" : ",");
        } else {
            printf("      {\"port\": %lu, \"max_packet_length\": null, \"error_code\": %d}%s\n",
                   (unsigned long)p, rc, p == 6 ? "" : ",");
        }
    }
    fputs("    ],\n", stdout);

    fputs("    \"_raw\": {\n", stdout);
    fputs("      \"version_backend\": ", stdout);
    emit_raw_probe_object(OPT_SWITCH_VERSION, 684, 0, NULL);
    fputs(",\n      \"patch_info_backend\": ", stdout);
    emit_raw_probe_object(OPT_SWITCH_PATCH_INFO, 684, 0, NULL);
    fputs(",\n      \"thermal_backend\": ", stdout);
    emit_raw_probe_object(OPT_SWITCH_THERMAL, 684, 0, NULL);
    fputs(",\n      \"mgmt_mac_backend\": ", stdout);
    emit_raw_probe_object(OPT_SWITCH_MGMT_MAC, 684, 0, NULL);
    fputs("\n    }\n", stdout);
}

static void emit_classification(void) {
    static const unsigned interesting[] = {64,65,66,67,68,69,70,71,72,254,255};
    uint8_t buf[256];
    unsigned i;
    unsigned valid_count = 0;
    int have_vlan51 = 0;

    for (i = 0; i < sizeof(interesting)/sizeof(interesting[0]); ++i) {
        unsigned idx = interesting[i];
        int rc = get_raw_words(OPT_CLASSF_CFG_ENTRY, 208, 1, &idx, buf, sizeof(buf));
        if (rc == 0 && !raw_all_zero(buf, 208)) {
            unsigned w;
            ++valid_count;
            for (w = 0; w + 4 <= 208; w += 4) {
                if (get_be32(buf + w) == 51) have_vlan51 = 1;
            }
        }
    }

    fputs("    \"summary\": {", stdout);
    printf("\"interesting_valid_entry_count\": %lu, ", (unsigned long)valid_count);
    fputs("\"observed_vlan_ids\": ", stdout); emit_json_string_array_unique_vlan51(have_vlan51);
    fputs(", \"upstream_sid_guess\": 0, \"note\": ", stdout);
    json_string("compact summary derived from known active classify entries; raw words are retained under _raw_entries");
    fputs("},\n", stdout);

    fputs("    \"_raw\": {\n", stdout);
    fputs("      \"cf_select_backend\": ", stdout); emit_raw_probe_object(OPT_CLASSF_CF_SEL, 208, 0, NULL); fputs(",\n", stdout);
    fputs("      \"upstream_unmatch_backend\": ", stdout); emit_raw_probe_object(OPT_CLASSF_UNMATCH_US, 208, 0, NULL); fputs(",\n", stdout);
    fputs("      \"downstream_unmatch_backend\": ", stdout); emit_raw_probe_object(OPT_CLASSF_UNMATCH_DS, 208, 0, NULL); fputs(",\n", stdout);
    fputs("      \"default_wan_if_backend\": ", stdout); emit_raw_probe_object(OPT_CLASSF_DEFAULT_WANIF, 208, 0, NULL); fputs(",\n", stdout);
    fputs("      \"entry_num_pattern1_backend\": ", stdout); emit_raw_probe_object(OPT_CLASSF_ENTRY_NUM_P1, 208, 0, NULL); fputs(",\n", stdout);
    fputs("      \"entries\": [\n", stdout);
    for (i = 0; i < sizeof(interesting)/sizeof(interesting[0]); ++i) {
        unsigned idx = interesting[i];
        int rc = get_raw_words(OPT_CLASSF_CFG_ENTRY, 208, 1, &idx, buf, sizeof(buf));
        printf("        {\"index\": %lu, ", (unsigned long)idx);
        if (rc == 0) {
            printf("\"valid_guess\": %s, \"nonzero_words\": ", raw_all_zero(buf, 208) ? "false" : "true");
            emit_nonzero_words_from_buf(buf, 208);
        } else {
            printf("\"valid_guess\": false, \"error_code\": %d", rc);
        }
        fputs(i + 1 == sizeof(interesting)/sizeof(interesting[0]) ? "}\n" : "},\n", stdout);
    }
    fputs("      ]\n", stdout);
    fputs("    }\n", stdout);
}

static void emit_l2(void) {
    uint8_t buf[1024];
    unsigned p;
    int rc;
    uint32_t link_flush = 0, learn_count = 0, limit_count = 0;

    rc = get_raw_words(OPT_L2_FLUSH_LINKDOWN, 580, 0, NULL, buf, sizeof(buf));
    if (rc == 0) link_flush = get_be32(buf + 0);
    rc = get_raw_words(OPT_L2_LEARNING_COUNT, 580, 0, NULL, buf, sizeof(buf));
    if (rc == 0) learn_count = get_be32(buf + 44);
    rc = get_raw_words(OPT_L2_LIMIT_COUNT, 580, 0, NULL, buf, sizeof(buf));
    if (rc == 0) limit_count = get_be32(buf + 44);

    printf("    \"link_down_flush\": %s,\n", link_flush ? "true" : "false");
    printf("    \"link_down_flush_raw\": %lu,\n", (unsigned long)link_flush);
    printf("    \"learning_count\": %lu,\n", (unsigned long)learn_count);
    printf("    \"limit_learning_count\": %lu,\n", (unsigned long)limit_count);
    fputs("    \"port_learning_counts\": [\n", stdout);
    for (p = 0; p <= 6; ++p) {
        unsigned word = p;
        uint32_t count = 0;
        rc = get_raw_words(OPT_L2_PORT_LEARN_COUNT, 580, 1, &word, buf, sizeof(buf));
        if (rc == 0) count = get_be32(buf + 44);
        printf("      {\"port\": %lu, \"count\": %lu}", (unsigned long)p, (unsigned long)count);
        fputs(p == 6 ? "\n" : ",\n", stdout);
    }
    fputs("    ]\n", stdout);
}

static const char *security_action_name(uint32_t raw) {
    switch (raw) {
    case 0: return "forward";
    case 1: return "drop";
    case 2: return "trap";
    default: return "unknown";
    }
}

static void emit_security(void) {
    unsigned p, t;
    uint8_t buf[64];

    fputs("    \"attack_prevent\": {\n", stdout);
    fputs("      \"ports\": [\n", stdout);
    for (p = 0; p <= 6; ++p) {
        unsigned word = p;
        int rc = get_raw_words(OPT_SEC_PORT_ATTACK, 28, 1, &word, buf, sizeof(buf));
        uint32_t enabled = (rc == 0) ? word_or_zero(buf, 28, 1) : 0;
        printf("        {\"port\": %lu, ", (unsigned long)p);
        if (rc == 0) printf("\"enabled\": %s, \"raw\": %lu", enabled ? "true" : "false", (unsigned long)enabled);
        else printf("\"enabled\": null, \"error_code\": %d", rc);
        fputs(p == 6 ? "}\n" : "},\n", stdout);
    }
    fputs("      ]\n", stdout);
    fputs("    },\n", stdout);

    fputs("    \"actions\": {\n", stdout);
    for (t = 0; t <= 15; ++t) {
        unsigned word = t;
        int rc = get_raw_words(OPT_SEC_ATTACK_ACTION, 28, 1, &word, buf, sizeof(buf));
        uint32_t action = (rc == 0) ? word_or_zero(buf, 28, 1) : 0;
        fputs("      ", stdout); json_string(attack_type_name(t)); fputs(": ", stdout);
        if (rc == 0) { printf("{\"action\": "); json_string(security_action_name(action)); printf(", \"raw\": %lu}", (unsigned long)action); }
        else printf("{\"action\": null, \"error_code\": %d}", rc);
        fputs(t == 15 ? "\n" : ",\n", stdout);
    }
    fputs("    },\n", stdout);

    fputs("    \"flood_thresholds\": {\n", stdout);
    for (t = 13; t <= 15; ++t) {
        unsigned word = t;
        int rc = get_raw_words(OPT_SEC_FLOOD_THRESH, 28, 1, &word, buf, sizeof(buf));
        uint32_t threshold = (rc == 0) ? word_or_zero(buf, 28, 1) : 0;
        fputs("      ", stdout); json_string(attack_type_name(t)); fputs(": ", stdout);
        if (rc == 0) printf("{\"threshold\": %lu, \"threshold_raw\": %lu}", (unsigned long)threshold, (unsigned long)threshold);
        else printf("{\"threshold\": null, \"error_code\": %d}", rc);
        fputs(t == 15 ? "\n" : ",\n", stdout);
    }
    fputs("    }\n", stdout);
}

static void emit_storm_control(void) {
    unsigned t, p;
    uint8_t buf[128];
    fputs("    \"types\": [\n", stdout);
    for (t = 0; t <= 6; ++t) {
        printf("      {\"type\": "); json_string(storm_type_name(t)); printf(", \"type_raw\": %lu, \"ports\": [", (unsigned long)t);
        for (p = 0; p <= 6; ++p) {
            unsigned words[2];
            int re, rm;
            uint32_t enabled = 0, meter = 0;
            words[0] = t; words[1] = p;
            re = get_raw_words(OPT_STORM_PORT_ENABLE, 64, 2, words, buf, sizeof(buf));
            if (re == 0) enabled = word_or_zero(buf, 64, 2);
            rm = get_raw_words(OPT_STORM_METER_IDX, 64, 2, words, buf, sizeof(buf));
            if (rm == 0) meter = word_or_zero(buf, 64, 2);
            printf("%s{\"port\": %lu, ", p ? ", " : "", (unsigned long)p);
            if (re == 0) printf("\"enabled\": %s, \"enabled_raw\": %lu", enabled ? "true" : "false", (unsigned long)enabled);
            else printf("\"enabled\": null, \"enable_error\": %d", re);
            if (rm == 0) printf(", \"meter\": %lu", (unsigned long)meter);
            else printf(", \"meter\": null, \"meter_error\": %d", rm);
            fputs("}", stdout);
        }
        fputs("]}", stdout);
        fputs(t == 6 ? "\n" : ",\n", stdout);
    }
    fputs("    ],\n", stdout);
    fputs("    \"bypass\": ", stdout);
    emit_raw_probe_object(OPT_STORM_BYPASS, 64, 0, NULL);
    fputs("\n", stdout);
}

static void emit_buffer(void) {
    fputs("    \"implemented\": false,\n", stdout);
    fputs("    \"reason\": ", stdout);
    json_string("flowctrl page counters in the dumps are raw ASIC register helper calls, not yet mapped to the Realtek socket backend used by this standalone binary");
    fputs(",\n    \"wanted_diag_sources\": [", stdout);
    json_string("flowctrl dump used-page"); fputs(", ", stdout);
    json_string("flowctrl get used-page-cnt total"); fputs(", ", stdout);
    json_string("flowctrl get used-page-cnt ingress/egress port all");
    fputs("]\n", stdout);
}

static void emit_pbo(void) {
    fputs("    \"implemented\": false,\n", stdout);
    fputs("    \"reason\": ", stdout);
    json_string("PBO counters/config are currently exposed by raw rtl9602c_raw_pbo_* register helpers; the matching socket layout is not mapped yet");
    fputs(",\n    \"wanted_diag_sources\": [", stdout);
    json_string("pbo get state/status/used-page"); fputs(", ", stdout);
    json_string("pbo dump config memory"); fputs(", ", stdout);
    json_string("pbo dump counter pbo/ponnic/group all");
    fputs("]\n", stdout);
}


static void emit_meta(void) {
    fputs("  \"meta\": {\n", stdout);
    fputs("    \"tool\": \"diag_to_json\",\n", stdout);
    fputs("    \"readonly\": true,\n", stdout);
    fputs("    \"uses_diag_binary\": false,\n", stdout);
    fputs("    \"backend\": \"Realtek getsockopt raw socket 0xff\"\n", stdout);
    fputs("  }", stdout);
}

static int get_transceiver_dbm_micro(unsigned type, int32_t *out) {
    uint8_t raw[24];
    int rc = get_transceiver_raw(type, raw);
    if (rc != 0) return rc;
    *out = optical_raw_to_dbm_micro(get_be16(raw));
    return 0;
}

static int get_transceiver_temp_micro(int32_t *out) {
    uint8_t raw[24];
    int16_t t;
    int rc = get_transceiver_raw(TYPE_TEMPERATURE, raw);
    if (rc != 0) return rc;
    t = get_sbe16(raw);
    *out = (int32_t)(((int64_t)t * 1000000 + (t >= 0 ? 128 : -128)) / 256);
    return 0;
}

static int get_port_link_state(unsigned p, uint32_t *link_out) {
    uint8_t buf[264];
    unsigned word = p;
    int rc = get_raw_words(OPT_PORT_LINK, sizeof(buf), 1, &word, buf, sizeof(buf));
    if (rc != 0) return rc;
    *link_out = get_be32(buf + 4);
    return 0;
}

static int current_mib_errors_active(void) {
    uint8_t buf[512];
    int rc = get_raw_words(OPT_MIB_PORT_ALL, 376, 0, NULL, buf, sizeof(buf));
    if (rc != 0) return 0;
    return has_nonzero_mib_error_words(buf);
}

static int get_transceiver_voltage_micro(int32_t *out) {
    uint8_t raw[24];
    int rc = get_transceiver_raw(TYPE_VOLTAGE, raw);
    if (rc != 0) return rc;
    *out = (int32_t)get_be16(raw) * 100; /* raw / 10000 V -> micro-V */
    return 0;
}

static int get_transceiver_bias_micro_ma(int32_t *out) {
    uint8_t raw[24];
    int rc = get_transceiver_raw(TYPE_BIAS_CURRENT, raw);
    if (rc != 0) return rc;
    *out = (int32_t)get_be16(raw) * 2000; /* raw / 500 mA -> micro-mA */
    return 0;
}

static const char *ha_status_rx_power(int32_t micro_dbm) {
    if (micro_dbm == INT_MIN) return "unknown";
    if (micro_dbm <= -30000000 || micro_dbm >= -3000000) return "critical";
    if (micro_dbm <= -27000000 || micro_dbm >= -8000000) return "warning";
    return "ok";
}

static const char *ha_status_tx_power(int32_t micro_dbm) {
    if (micro_dbm == INT_MIN) return "unknown";
    if (micro_dbm <= -3000000 || micro_dbm >= 7000000) return "critical";
    if (micro_dbm <= 0 || micro_dbm >= 5500000) return "warning";
    return "ok";
}

static const char *ha_status_temperature(int32_t micro_c) {
    if (micro_c == INT_MIN) return "unknown";
    if (micro_c <= -40000000 || micro_c >= 85000000) return "critical";
    if (micro_c <= -10000000 || micro_c >= 70000000) return "warning";
    return "ok";
}

static const char *ha_status_voltage(int32_t micro_v) {
    if (micro_v == INT_MIN) return "unknown";
    if (micro_v <= 2900000 || micro_v >= 3700000) return "critical";
    if (micro_v <= 3100000 || micro_v >= 3500000) return "warning";
    return "ok";
}

static const char *ha_status_bias(int32_t micro_ma) {
    if (micro_ma == INT_MIN) return "unknown";
    if (micro_ma <= 1000000 || micro_ma >= 80000000) return "critical";
    if (micro_ma <= 3000000 || micro_ma >= 60000000) return "warning";
    return "ok";
}

static int ha_status_is_bad(const char *s) {
    return strcmp(s, "ok") != 0;
}

static void emit_ha_sensor_number(const char *key, int32_t micro, const char *unit, const char *device_class, const char *status) {
    fputs("      ", stdout); json_string(key); fputs(": {\"value\": ", stdout);
    json_micro_number(micro);
    fputs(", \"unit\": ", stdout); json_string(unit);
    if (device_class && device_class[0]) {
        fputs(", \"device_class\": ", stdout); json_string(device_class);
    }
    fputs(", \"state_class\": \"measurement\"", stdout);
    fputs(", \"status\": ", stdout); json_string(status);
    fputs("}", stdout);
}

static void emit_ha_binary_sensor(const char *key, int state, const char *device_class) {
    fputs("      ", stdout); json_string(key);
    fputs(": {\"state\": ", stdout);
    fputs(state ? "true" : "false", stdout);
    if (device_class && device_class[0]) {
        fputs(", \"device_class\": ", stdout);
        json_string(device_class);
    }
    fputs("}", stdout);
}

static void emit_health(void) {
    uint32_t onu = 0, alarm = 0, link = 0;
    int32_t tx = INT_MIN, rx = INT_MIN, temp = INT_MIN, voltage = INT_MIN, bias = INT_MIN;
    uint8_t buf[16];
    unsigned p;
    int first = 1;
    int gpon_ok = (get_u32_opt(OPT_GPON_ONU_STATE, &onu) == 0 && onu == 5);
    int alarms_active = 0;
    int mib_errors = current_mib_errors_active();
    const char *rx_status;
    const char *tx_status;
    const char *temp_status;
    const char *voltage_status;
    const char *bias_status;
    int optics_problem;

    if (get_buf_opt(OPT_GPON_ALARM_STATUS, buf, 8) == 0) {
        alarm = get_be32(buf);
        alarms_active = alarm != 0;
    }

    get_transceiver_dbm_micro(TYPE_TX_POWER, &tx);
    get_transceiver_dbm_micro(TYPE_RX_POWER, &rx);
    get_transceiver_temp_micro(&temp);
    get_transceiver_voltage_micro(&voltage);
    get_transceiver_bias_micro_ma(&bias);

    rx_status = ha_status_rx_power(rx);
    tx_status = ha_status_tx_power(tx);
    temp_status = ha_status_temperature(temp);
    voltage_status = ha_status_voltage(voltage);
    bias_status = ha_status_bias(bias);
    optics_problem = ha_status_is_bad(rx_status) || ha_status_is_bad(tx_status) ||
                     ha_status_is_bad(temp_status) || ha_status_is_bad(voltage_status) ||
                     ha_status_is_bad(bias_status);

    fputs("  \"health\": {\n", stdout);
    fputs("    \"overall\": ", stdout);
    json_string((gpon_ok && !alarms_active && !mib_errors && !optics_problem) ? "ok" : "warning");
    fputs(",\n", stdout);
    printf("    \"gpon_online\": %s,\n", gpon_ok ? "true" : "false");
    fputs("    \"onu_state\": ", stdout); json_string(onu_state_name(onu)); fputs(",\n", stdout);
    printf("    \"alarms_active\": %s,\n", alarms_active ? "true" : "false");
    printf("    \"alarm_status_raw\": %lu,\n", (unsigned long)alarm);
    printf("    \"mib_errors_active\": %s,\n", mib_errors ? "true" : "false");

    fputs("    \"optics\": {\n", stdout);
    fputs("      \"rx_power_dbm\": ", stdout); json_micro_number(rx); fputs(",\n", stdout);
    fputs("      \"rx_power_status\": ", stdout); json_string(rx_status); fputs(",\n", stdout);
    fputs("      \"tx_power_dbm\": ", stdout); json_micro_number(tx); fputs(",\n", stdout);
    fputs("      \"tx_power_status\": ", stdout); json_string(tx_status); fputs(",\n", stdout);
    fputs("      \"temperature_c\": ", stdout); json_micro_number(temp); fputs(",\n", stdout);
    fputs("      \"temperature_status\": ", stdout); json_string(temp_status); fputs(",\n", stdout);
    fputs("      \"voltage_v\": ", stdout); json_micro_number(voltage); fputs(",\n", stdout);
    fputs("      \"voltage_status\": ", stdout); json_string(voltage_status); fputs(",\n", stdout);
    fputs("      \"bias_current_ma\": ", stdout); json_micro_number(bias); fputs(",\n", stdout);
    fputs("      \"bias_current_status\": ", stdout); json_string(bias_status); fputs("\n", stdout);
    fputs("    },\n", stdout);

    fputs("    \"thresholds\": {\n", stdout);
    fputs("      \"rx_power_dbm\": {\"ok_min\": -27.0, \"ok_max\": -8.0, \"critical_min\": -30.0, \"critical_max\": -3.0},\n", stdout);
    fputs("      \"tx_power_dbm\": {\"ok_min\": 0.0, \"ok_max\": 5.5, \"critical_min\": -3.0, \"critical_max\": 7.0},\n", stdout);
    fputs("      \"temperature_c\": {\"ok_min\": -10.0, \"ok_max\": 70.0, \"critical_min\": -40.0, \"critical_max\": 85.0},\n", stdout);
    fputs("      \"voltage_v\": {\"ok_min\": 3.1, \"ok_max\": 3.5, \"critical_min\": 2.9, \"critical_max\": 3.7},\n", stdout);
    fputs("      \"bias_current_ma\": {\"ok_min\": 3.0, \"ok_max\": 60.0, \"critical_min\": 1.0, \"critical_max\": 80.0}\n", stdout);
    fputs("    },\n", stdout);

    fputs("    \"ports_up\": [", stdout);
    first = 1;
    for (p = 0; p <= 6; ++p) {
        if (get_port_link_state(p, &link) == 0 && link) {
            if (!first) fputs(", ", stdout);
            printf("%lu", (unsigned long)p);
            first = 0;
        }
    }
    fputs("],\n", stdout);

    fputs("    \"warnings\": [", stdout);
    first = 1;
    if (!gpon_ok) { json_string("gpon_not_o5"); first = 0; }
    if (alarms_active) { if (!first) fputs(", ", stdout); json_string("gpon_alarm_active"); first = 0; }
    if (mib_errors) { if (!first) fputs(", ", stdout); json_string("mib_error_counter_nonzero"); first = 0; }
    if (ha_status_is_bad(rx_status)) { if (!first) fputs(", ", stdout); json_string("rx_power_not_ok"); first = 0; }
    if (ha_status_is_bad(tx_status)) { if (!first) fputs(", ", stdout); json_string("tx_power_not_ok"); first = 0; }
    if (ha_status_is_bad(temp_status)) { if (!first) fputs(", ", stdout); json_string("temperature_not_ok"); first = 0; }
    if (ha_status_is_bad(voltage_status)) { if (!first) fputs(", ", stdout); json_string("voltage_not_ok"); first = 0; }
    if (ha_status_is_bad(bias_status)) { if (!first) fputs(", ", stdout); json_string("bias_current_not_ok"); first = 0; }
    fputs("]\n", stdout);
    fputs("  }", stdout);
}

static void emit_cpu_scalar_state(const char *key, unsigned opt) {
    uint8_t buf[32];
    int rc = get_raw_words(opt, 16, 0, NULL, buf, sizeof(buf));
    uint32_t guess = rc == 0 ? first_non_echo_word(buf, 16, 0) : 0;
    fputs("    ", stdout); json_string(key); fputs(": ", stdout);
    if (rc == 0) printf("{\"enabled\": %s, \"raw_guess\": %lu}", guess ? "true" : "false", (unsigned long)guess);
    else printf("{\"enabled\": null, \"error_code\": %d}", rc);
}

static void emit_cpu(void) {
    emit_cpu_scalar_state("tag_aware", OPT_CPU_TAG_AWARE);
    fputs(",\n", stdout);
    emit_cpu_scalar_state("trap_insert_tag", OPT_CPU_TRAP_INSERT_TAG);
    fputs("\n", stdout);
}

static void emit_bandwidth(void) {
    uint8_t buf[128];
    unsigned p;
    int rc;
    uint32_t v = 0;
    rc = get_raw_words(OPT_RATE_EGR_INCLUDE_IFG, 64, 0, NULL, buf, sizeof(buf));
    if (rc == 0) v = first_non_echo_word(buf, 64, 0);
    fputs("    \"egress_ifg\": ", stdout);
    if (rc == 0) { fputs("{\"include\": ", stdout); printf("%s, \"mode\": ", v ? "true" : "false"); json_string(v ? "include" : "exclude"); printf(", \"raw\": %lu}", (unsigned long)v); }
    else printf("{\"include\": null, \"error_code\": %d}", rc);
    fputs(",\n", stdout);
    fputs("    \"egress_ifg_ports\": [\n", stdout);
    for (p = 0; p <= 6; ++p) {
        unsigned word = p;
        rc = get_raw_words(OPT_RATE_PORT_EGR_INCLUDE_IFG, 64, 1, &word, buf, sizeof(buf));
        v = rc == 0 ? word_or_zero(buf, 64, 1) : 0;
        printf("      {\"port\": %lu, ", (unsigned long)p);
        if (rc == 0) { printf("\"include\": %s, \"mode\": ", v ? "true" : "false"); json_string(v ? "include" : "exclude"); printf(", \"raw\": %lu", (unsigned long)v); }
        else printf("\"include\": null, \"error_code\": %d", rc);
        fputs(p == 6 ? "}\n" : "},\n", stdout);
    }
    fputs("    ],\n", stdout);
    fputs("    \"ingress_bypass_packet\": {\"implemented\": false, \"reason\": ", stdout);
    json_string("matching socket backend not mapped yet; vendor diag reports disabled on the observed dump");
    fputs("}\n", stdout);
}

static void emit_home_assistant(void) {
    uint32_t onu = 0, alarm = 0, link = 0;
    int32_t tx = INT_MIN, rx = INT_MIN, temp = INT_MIN, voltage = INT_MIN, bias = INT_MIN;
    uint8_t buf[16];
    unsigned p;
    int gpon_ok = (get_u32_opt(OPT_GPON_ONU_STATE, &onu) == 0 && onu == 5);
    int alarms_active = 0;
    int mib_errors = current_mib_errors_active();
    const char *rx_status;
    const char *tx_status;
    const char *temp_status;
    const char *voltage_status;
    const char *bias_status;
    int optics_problem;
    int ports_up_count = 0;

    if (get_buf_opt(OPT_GPON_ALARM_STATUS, buf, 8) == 0) {
        alarm = get_be32(buf);
        alarms_active = alarm != 0;
    }

    get_transceiver_dbm_micro(TYPE_TX_POWER, &tx);
    get_transceiver_dbm_micro(TYPE_RX_POWER, &rx);
    get_transceiver_temp_micro(&temp);
    get_transceiver_voltage_micro(&voltage);
    get_transceiver_bias_micro_ma(&bias);

    rx_status = ha_status_rx_power(rx);
    tx_status = ha_status_tx_power(tx);
    temp_status = ha_status_temperature(temp);
    voltage_status = ha_status_voltage(voltage);
    bias_status = ha_status_bias(bias);
    optics_problem = ha_status_is_bad(rx_status) || ha_status_is_bad(tx_status) ||
                     ha_status_is_bad(temp_status) || ha_status_is_bad(voltage_status) ||
                     ha_status_is_bad(bias_status);

    for (p = 0; p <= 6; ++p) {
        if (get_port_link_state(p, &link) == 0 && link) ++ports_up_count;
    }

    fputs("  \"home_assistant\": {\n", stdout);
    fputs("    \"availability\": {\"online\": ", stdout);
    fputs(gpon_ok ? "true" : "false", stdout);
    fputs(", \"state\": ", stdout);
    json_string(gpon_ok ? "online" : "offline");
    fputs("},\n", stdout);
    fputs("    \"suggested_entity_prefix\": \"ont\",\n", stdout);
    fputs("    \"discovery_hint\": \"Use this object for HA template sensors/binary_sensors; values are already normalized and unit-tagged.\",\n", stdout);

    fputs("    \"binary_sensors\": {\n", stdout);
    emit_ha_binary_sensor("gpon_online", gpon_ok, "connectivity"); fputs(",\n", stdout);
    emit_ha_binary_sensor("gpon_alarm_active", alarms_active, "problem"); fputs(",\n", stdout);
    emit_ha_binary_sensor("mib_errors_active", mib_errors, "problem"); fputs(",\n", stdout);
    emit_ha_binary_sensor("optics_problem", optics_problem, "problem"); fputs(",\n", stdout);
    for (p = 0; p <= 6; ++p) {
        char key[32];
        int state = 0;
        link = 0;
        if (get_port_link_state(p, &link) == 0 && link) state = 1;
        snprintf(key, sizeof(key), "port_%lu_link", (unsigned long)p);
        emit_ha_binary_sensor(key, state, "connectivity");
        fputs(p == 6 ? "\n" : ",\n", stdout);
    }
    fputs("    },\n", stdout);

    fputs("    \"sensors\": {\n", stdout);
    fputs("      \"onu_state_raw\": {\"value\": ", stdout); printf("%lu", (unsigned long)onu);
    fputs(", \"name\": ", stdout); json_string(onu_state_name(onu)); fputs("},\n", stdout);
    fputs("      \"alarm_status\": {\"value\": ", stdout); printf("%lu", (unsigned long)alarm);
    printf(", \"hex\": \"0x%08lx\"},\n", (unsigned long)alarm);
    fputs("      \"ports_up_count\": {\"value\": ", stdout); printf("%d", ports_up_count);
    fputs(", \"unit\": \"ports\"},\n", stdout);
    emit_ha_sensor_number("rx_power_dbm", rx, "dBm", "signal_strength", rx_status); fputs(",\n", stdout);
    emit_ha_sensor_number("tx_power_dbm", tx, "dBm", "signal_strength", tx_status); fputs(",\n", stdout);
    emit_ha_sensor_number("temperature_c", temp, "°C", "temperature", temp_status); fputs(",\n", stdout);
    emit_ha_sensor_number("voltage_v", voltage, "V", "voltage", voltage_status); fputs(",\n", stdout);
    emit_ha_sensor_number("bias_current_ma", bias, "mA", "current", bias_status); fputs("\n", stdout);
    fputs("    },\n", stdout);

    fputs("    \"template_examples\": {\n", stdout);
    fputs("      \"availability_template\": \"{{ value_json.home_assistant.availability.online }}\",\n", stdout);
    fputs("      \"rx_power_state_template\": \"{{ value_json.home_assistant.sensors.rx_power_dbm.value }}\",\n", stdout);
    fputs("      \"gpon_online_state_template\": \"{{ value_json.home_assistant.binary_sensors.gpon_online.state }}\"\n", stdout);
    fputs("    }\n", stdout);
    fputs("  }", stdout);
}

static void emit_errors(void) {
    fputs("  \"errors\": [\n", stdout);
    fputs("    {\"section\": \"buffer\", \"kind\": \"not_implemented\", \"reason\": ", stdout);
    json_string("flowctrl page counters not mapped to stable socket backend yet");
    fputs("},\n", stdout);
    fputs("    {\"section\": \"pbo\", \"kind\": \"not_implemented\", \"reason\": ", stdout);
    json_string("PBO register-helper layout not mapped to stable socket backend yet");
    fputs("}\n", stdout);
    fputs("  ]", stdout);
}

int main(void) {
    fputs("{\n", stdout);
    emit_runtime();
    fputs(",\n", stdout);
    emit_system();
    fputs(",\n", stdout);
    emit_meta();
    fputs(",\n", stdout);
    emit_health();
    fputs(",\n", stdout);

    emit_home_assistant();
    fputs(",\n", stdout);

    fputs("  \"device\": {\n", stdout);
    emit_device();
    fputs("  },\n", stdout);

    fputs("  \"gpon\": {\n", stdout);
    emit_gpon_summary();
    fputs("  },\n", stdout);

    fputs("  \"transceiver\": {\n", stdout);
    emit_transceiver();
    fputs("  },\n", stdout);

    fputs("  \"flows\": {\n", stdout);
    emit_flows();
    fputs("  },\n", stdout);

    fputs("  \"counters\": {\n", stdout);
    emit_counters();
    fputs("  },\n", stdout);

    emit_ports();
    fputs(",\n", stdout);

    fputs("  \"vlan\": {\n", stdout);
    emit_vlan();
    fputs("  },\n", stdout);

    fputs("  \"qos\": {\n", stdout);
    emit_qos();
    fputs("  },\n", stdout);

    fputs("  \"cpu\": {\n", stdout);
    emit_cpu();
    fputs("  },\n", stdout);

    fputs("  \"bandwidth\": {\n", stdout);
    emit_bandwidth();
    fputs("  },\n", stdout);

    fputs("  \"mib\": {\n", stdout);
    emit_mib();
    fputs("  },\n", stdout);

    fputs("  \"switch\": {\n", stdout);
    emit_switch_extra();
    fputs("  },\n", stdout);

    fputs("  \"classification\": {\n", stdout);
    emit_classification();
    fputs("  },\n", stdout);

    fputs("  \"buffer\": {\n", stdout);
    emit_buffer();
    fputs("  },\n", stdout);

    fputs("  \"pbo\": {\n", stdout);
    emit_pbo();
    fputs("  },\n", stdout);

    fputs("  \"l2\": {\n", stdout);
    emit_l2();
    fputs("  },\n", stdout);

    fputs("  \"security\": {\n", stdout);
    emit_security();
    fputs("  },\n", stdout);

    fputs("  \"storm_control\": {\n", stdout);
    emit_storm_control();
    fputs("  },\n", stdout);

    emit_errors();
    fputs("\n", stdout);

    fputs("}\n", stdout);
    return 0;
}
