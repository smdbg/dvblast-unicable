/* SPDX-License-Identifier: GPL-2.0-only */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dvb/frontend.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* EN50494 only, universal Ku LNB: 9750/10600 MHz, switch 11700 MHz.
 * Calculation follows VDR diseqc.c, cDiseqc::SetScrFrequency(). Only the
 * selected user band is programmed, once. Keep the frontend open for
 * DVBlast external tuning (-f -). No arguments preserve the tested profile.
 */
struct options {
    unsigned adapter, duration, frequency_khz, symbol_rate, slot, slot_khz;
    char polarization, position;
    enum fe_delivery_system system;
    enum fe_modulation modulation;
    enum fe_code_rate fec;
    enum fe_rolloff rolloff;
    int dry, json, parameterized, modulation_set, fec_set, rolloff_set;
};
struct measure { int present; double value; };
struct signal_stats {
    struct measure cnr_db, strength_dbm, strength_percent, quality_percent;
};
static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }
static void fail(const char *what)
{
    if (stop) exit(0);
    perror(what);
    exit(1);
}
static int timespec_compare(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}
static uint64_t elapsed_seconds(const struct timespec *start, const struct timespec *now)
{
    uint64_t seconds = (uint64_t)(now->tv_sec - start->tv_sec);
    if (now->tv_nsec < start->tv_nsec && seconds) --seconds;
    return seconds;
}
static int parse_uint(const char *text, unsigned min, unsigned max, unsigned *out)
{
    if (!*text) return -1;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value < min || value > max) return -1;
    *out = (unsigned)value;
    return 0;
}
/* Exact MHz -> kHz conversion; at most three decimal places. */
static int parse_mhz(const char *text, unsigned min, unsigned max, unsigned *out)
{
    const char *p = text;
    unsigned whole = 0, fraction = 0, places = 0;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        if (whole > max / 1000) return -1;
        whole = whole * 10 + (unsigned)(*p++ - '0');
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (++places > 3) return -1;
            fraction = fraction * 10 + (unsigned)(*p++ - '0');
        }
        if (!places) return -1;
    }
    if (*p || whole > max / 1000) return -1;
    while (places++ < 3) fraction *= 10;
    unsigned value = whole * 1000 + fraction;
    if (value < min || value > max) return -1;
    *out = value;
    return 0;
}
struct named_fec { const char *name; enum fe_code_rate value; };
static const struct named_fec fecs[] = {
    {"auto", FEC_AUTO}, {"1/2", FEC_1_2}, {"2/3", FEC_2_3},
    {"3/4", FEC_3_4}, {"4/5", FEC_4_5}, {"5/6", FEC_5_6},
    {"6/7", FEC_6_7}, {"7/8", FEC_7_8}, {"8/9", FEC_8_9},
    {"3/5", FEC_3_5}, {"9/10", FEC_9_10}
};
static const char *fec_name(enum fe_code_rate fec)
{
    for (unsigned i = 0; i < sizeof(fecs)/sizeof(fecs[0]); ++i)
        if (fecs[i].value == fec) return fecs[i].name;
    return "unknown";
}
static const char *modulation_name(enum fe_modulation modulation)
{
    return modulation == QPSK ? "QPSK" : modulation == PSK_8 ? "8PSK" : "auto";
}
static const char *rolloff_name(enum fe_rolloff rolloff)
{
    return rolloff == ROLLOFF_20 ? "0.20" : rolloff == ROLLOFF_25 ? "0.25" :
           rolloff == ROLLOFF_35 ? "0.35" : "auto";
}
static void usage(FILE *out, const char *program)
{
    fprintf(out,
        "Usage: %s [options]\n"
        "  --adapter 0..255                 (default 7; frontend0)\n"
        "  --frequency MHz                 (10700..12750, default 12654)\n"
        "  --symbol-rate symbols/second    (100000..65000000, default 30000000)\n"
        "  --polarization H|V              (default H)\n"
        "  --delivery-system DVB-S|DVB-S2   (default DVB-S2)\n"
        "  --slot 1..8                     (default 3; EN50494 user band)\n"
        "  --slot-frequency MHz            (950..2150, default 1680)\n"
        "  --position A|B                  (default A)\n"
        "  --seconds 0..86400              (default 60; 0 holds until signal)\n"
        "  --modulation auto|QPSK|8PSK\n"
        "  --fec auto|1/2|2/3|3/4|4/5|5/6|6/7|7/8|8/9|3/5|9/10\n"
        "  --rolloff auto|0.20|0.25|0.35\n"
        "  --json                          JSON lines, status every second\n"
        "  --dry-run                       calculate without opening devices\n"
        "Without tuning options: legacy 8PSK, FEC5/6, rolloff0.20. Explicit tuning\n"
        "options select auto FEC/rolloff and capability-dependent modulation.\n", program);
}
static int parse_options(int argc, char **argv, struct options *o)
{
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) { usage(stdout, argv[0]); return 1; }
        if (!strcmp(arg, "--dry-run")) { o->dry = 1; continue; }
        if (!strcmp(arg, "--json")) { o->json = 1; continue; }
        if (++i == argc) { fprintf(stderr, "Missing value for %s\n", arg); return -1; }
        const char *value = argv[i];
        int valid = 0;
        if (!strcmp(arg, "--adapter")) valid = parse_uint(value, 0, 255, &o->adapter) == 0;
        else if (!strcmp(arg, "--seconds")) valid = parse_uint(value, 0, 86400, &o->duration) == 0;
        else {
            o->parameterized = 1;
            if (!strcmp(arg, "--frequency")) valid = parse_mhz(value, 10700000, 12750000, &o->frequency_khz) == 0;
            else if (!strcmp(arg, "--symbol-rate")) valid = parse_uint(value, 100000, 65000000, &o->symbol_rate) == 0;
            else if (!strcmp(arg, "--slot")) valid = parse_uint(value, 1, 8, &o->slot) == 0;
            else if (!strcmp(arg, "--slot-frequency")) valid = parse_mhz(value, 950000, 2150000, &o->slot_khz) == 0;
            else if (!strcmp(arg, "--polarization")) {
                valid = !strcmp(value, "H") || !strcmp(value, "V");
                if (valid) o->polarization = value[0];
            } else if (!strcmp(arg, "--position")) {
                valid = !strcmp(value, "A") || !strcmp(value, "B");
                if (valid) o->position = value[0];
            } else if (!strcmp(arg, "--delivery-system")) {
                valid = !strcmp(value, "DVB-S") || !strcmp(value, "DVB-S2");
                if (valid) o->system = !strcmp(value, "DVB-S") ? SYS_DVBS : SYS_DVBS2;
            } else if (!strcmp(arg, "--modulation")) {
                valid = !strcmp(value, "auto") || !strcmp(value, "QPSK") || !strcmp(value, "8PSK");
                if (valid) {
                    o->modulation_set = 1;
                    o->modulation = !strcmp(value, "QPSK") ? QPSK : !strcmp(value, "8PSK") ? PSK_8 : QAM_AUTO;
                }
            } else if (!strcmp(arg, "--fec")) {
                for (unsigned n = 0; n < sizeof(fecs)/sizeof(fecs[0]); ++n)
                    if (!strcmp(value, fecs[n].name)) {
                        o->fec = fecs[n].value; o->fec_set = 1; valid = 1; break;
                    }
            } else if (!strcmp(arg, "--rolloff")) {
                valid = !strcmp(value, "auto") || !strcmp(value, "0.20") || !strcmp(value, "0.25") || !strcmp(value, "0.35");
                if (valid) {
                    o->rolloff_set = 1;
                    o->rolloff = !strcmp(value, "0.20") ? ROLLOFF_20 : !strcmp(value, "0.25") ? ROLLOFF_25 :
                                 !strcmp(value, "0.35") ? ROLLOFF_35 : ROLLOFF_AUTO;
                }
            }
        }
        if (!valid) { fprintf(stderr, "Invalid option or value: %s %s\n", arg, value); return -1; }
    }
    if (o->parameterized) {
        if (!o->fec_set) o->fec = FEC_AUTO;
        if (!o->rolloff_set) o->rolloff = ROLLOFF_AUTO;
        if (!o->modulation_set) o->modulation = QAM_AUTO;
    }
    if (o->system == SYS_DVBS) {
        if (o->modulation_set && o->modulation == PSK_8) {
            fprintf(stderr, "DVB-S requires QPSK modulation.\n"); return -1;
        }
        o->modulation = QPSK;
        o->rolloff = ROLLOFF_35;
    }
    return 0;
}
static struct signal_stats read_stats(int fd)
{
    struct signal_stats result = {0};
    const uint32_t commands[] = {DTV_STAT_SIGNAL_STRENGTH, DTV_STAT_CNR};
    for (unsigned i = 0; i < 2; ++i) {
        struct dtv_property p = {.cmd = commands[i]};
        struct dtv_properties ps = {.num = 1, .props = &p};
        if (ioctl(fd, FE_GET_PROPERTY, &ps) < 0 || p.result != 0) continue;
        for (unsigned n = 0; n < p.u.st.len && n < MAX_DTV_STATS; ++n) {
            struct dtv_stats s = p.u.st.stat[n];
            struct measure *m = NULL;
            double value = 0;
            if (s.scale == FE_SCALE_DECIBEL) {
                m = i ? &result.cnr_db : &result.strength_dbm;
                value = (double)s.svalue / 1000.0;
            } else if (s.scale == FE_SCALE_RELATIVE && s.uvalue <= 65535) {
                m = i ? &result.quality_percent : &result.strength_percent;
                value = (double)s.uvalue * 100.0 / 65535.0;
            }
            if (m && !m->present) { m->present = 1; m->value = value; }
        }
    }
    return result;
}
static void json_measure(const char *name, struct measure m)
{
    printf(",\"%s\":", name);
    if (m.present) printf("%.3f", m.value); else printf("null");
}
static void print_status(const struct options *o, int fd, fe_status_t status, uint64_t second)
{
    struct signal_stats s = read_stats(fd);
    if (o->json) {
        printf("{\"event\":\"status\",\"adapter\":%u,\"elapsed_seconds\":%" PRIu64
               ",\"status\":%u,\"lock\":%s", o->adapter, second, (unsigned)status,
               status & FE_HAS_LOCK ? "true" : "false");
        json_measure("cnr_db", s.cnr_db);
        json_measure("strength_dbm", s.strength_dbm);
        json_measure("strength_percent", s.strength_percent);
        json_measure("quality_percent", s.quality_percent);
        puts("}");
    } else {
        printf("t=%" PRIu64 "s status=0x%02x lock=%s", second, (unsigned)status,
               status & FE_HAS_LOCK ? "YES" : "NO");
        if (s.strength_dbm.present) printf(" power=%.3fdBm", s.strength_dbm.value);
        if (s.strength_percent.present) printf(" power=%.1f%%", s.strength_percent.value);
        if (s.cnr_db.present) printf(" CNR=%.3fdB", s.cnr_db.value);
        if (s.quality_percent.present) printf(" CNR=%.1f%%", s.quality_percent.value);
        puts("");
    }
}
int main(int argc, char **argv)
{
    struct options o = {
        .adapter = 7, .duration = 60, .frequency_khz = 12654000,
        .symbol_rate = 30000000, .slot = 3, .slot_khz = 1680000,
        .polarization = 'H', .position = 'A', .system = SYS_DVBS2,
        .modulation = PSK_8, .fec = FEC_5_6, .rolloff = ROLLOFF_20
    };
    int parsed = parse_options(argc, argv, &o);
    if (parsed) return parsed < 0 ? 2 : 0;
    unsigned high_band = o.frequency_khz >= 11700000;
    unsigned lo_khz = high_band ? 10600000 : 9750000;
    unsigned intermediate_khz = o.frequency_khz - lo_khz;
    unsigned word = (intermediate_khz + o.slot_khz + 2000) / 4000 - 350;
    unsigned tune_khz = (word + 350) * 4000 - intermediate_khz;
    if (word > 1023 || tune_khz < 950000 || tune_khz > 2150000) {
        fprintf(stderr, "Frequency combination is outside the EN50494/frontend range.\n");
        return 2;
    }
    struct dvb_diseqc_master_cmd diseqc = {
        .msg = {0xe0, 0x10, 0x5a,
                ((o.slot - 1) << 5) | ((o.position == 'B') << 4) |
                ((o.polarization == 'H') << 3) | (high_band << 2) | (word >> 8), word & 255},
        .msg_len = 5
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (o.json) {
        printf("{\"event\":\"tuning\",\"adapter\":%u,\"frequency_mhz\":%.3f,"
               "\"symbol_rate\":%u,\"polarization\":\"%c\",\"delivery_system\":\"%s\","
               "\"slot\":%u,\"slot_frequency_mhz\":%.3f,\"position\":\"%c\","
               "\"lo_mhz\":%u,\"high_band\":%s,\"tuner_khz\":%u,"
               "\"modulation\":\"%s\",\"fec\":\"%s\",\"rolloff\":\"%s\",\"diseqc\":\"",
               o.adapter, o.frequency_khz / 1000.0, o.symbol_rate, o.polarization,
               o.system == SYS_DVBS ? "DVB-S" : "DVB-S2", o.slot, o.slot_khz / 1000.0,
               o.position, lo_khz / 1000, high_band ? "true" : "false", tune_khz,
               modulation_name(o.modulation), fec_name(o.fec), rolloff_name(o.rolloff));
        for (unsigned i = 0; i < diseqc.msg_len; ++i) printf("%s%02x", i ? " " : "", diseqc.msg[i]);
        puts("\"}");
    } else {
        printf("adapter%u/frontend0: EN50494 UB%u/index%u %gMHz position %c; RF%g%c SR%u %s %s FEC%s\n",
               o.adapter, o.slot, o.slot - 1, o.slot_khz / 1000.0, o.position,
               o.frequency_khz / 1000.0, o.polarization, o.symbol_rate,
               o.system == SYS_DVBS ? "DVB-S" : "DVB-S2", modulation_name(o.modulation), fec_name(o.fec));
        printf("DiSEqC:");
        for (unsigned i = 0; i < diseqc.msg_len; ++i) printf(" %02x", diseqc.msg[i]);
        printf("; tuner=%ukHz (Unicable rounding compensation)\n", tune_khz);
    }
    if (o.dry) return 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    char path[64];
    snprintf(path, sizeof(path), "/dev/dvb/adapter%u/frontend0", o.adapter);
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fail("open frontend");
    struct dvb_frontend_info info = {0};
    if (ioctl(fd, FE_GET_INFO, &info) < 0) fail("FE_GET_INFO");
    if (!o.json) printf("Frontend: %.128s\n", info.name);
    /* Some satellite frontends reject QAM_AUTO. STiD135 searches modulation
     * automatically, so PSK_8 is a compatible DVB-S2 hint. Other hardware can
     * use explicit --modulation QPSK or 8PSK. */
    if (o.modulation == QAM_AUTO && !(info.caps & FE_CAN_QAM_AUTO)) {
        o.modulation = o.system == SYS_DVBS ? QPSK : PSK_8;
        fprintf(stderr, "Frontend does not advertise auto modulation; using %s hint.\n", modulation_name(o.modulation));
    }
    struct dtv_property init[] = {
        {.cmd = DTV_CLEAR}, {.cmd = DTV_DELIVERY_SYSTEM, .u.data = o.system}
    };
    struct dtv_properties ps = {.num = sizeof(init)/sizeof(init[0]), .props = init};
    if (ioctl(fd, FE_SET_PROPERTY, &ps) < 0) fail("set delivery system");
    if (ioctl(fd, FE_SET_TONE, SEC_TONE_OFF) < 0) fail("tone off");
    if (ioctl(fd, FE_SET_VOLTAGE, SEC_VOLTAGE_13) < 0) fail("voltage 13V");
    usleep(250000);
    if (stop) { close(fd); return 0; }
    if (ioctl(fd, FE_SET_VOLTAGE, SEC_VOLTAGE_18) < 0) fail("voltage 18V");
    usleep(15000);
    int sent = stop ? 0 : ioctl(fd, FE_DISEQC_SEND_MASTER_CMD, &diseqc);
    int send_errno = errno;
    usleep(15000);
    if (ioctl(fd, FE_SET_VOLTAGE, SEC_VOLTAGE_13) < 0) fail("return voltage 13V");
    if (stop) { close(fd); return 0; }
    if (sent < 0) { errno = send_errno; fail("send EN50494"); }
    usleep(100000);
    if (stop) { close(fd); return 0; }
    struct dtv_property tune[10];
    unsigned count = 0;
#define ADD_PROPERTY(command, value) do { \
    tune[count++] = (struct dtv_property){.cmd = (command), .u.data = (value)}; \
} while (0)
    ADD_PROPERTY(DTV_DELIVERY_SYSTEM, o.system);
    ADD_PROPERTY(DTV_FREQUENCY, tune_khz);
    ADD_PROPERTY(DTV_SYMBOL_RATE, o.symbol_rate);
    ADD_PROPERTY(DTV_MODULATION, o.modulation);
    ADD_PROPERTY(DTV_INNER_FEC, o.fec);
    ADD_PROPERTY(DTV_INVERSION, INVERSION_AUTO);
    if (o.system == SYS_DVBS2) {
        ADD_PROPERTY(DTV_ROLLOFF, o.rolloff);
        ADD_PROPERTY(DTV_PILOT, PILOT_AUTO);
        ADD_PROPERTY(DTV_STREAM_ID, NO_STREAM_ID_FILTER);
    }
    ADD_PROPERTY(DTV_TUNE, 0);
#undef ADD_PROPERTY
    ps.num = count; ps.props = tune;
    if (ioctl(fd, FE_SET_PROPERTY, &ps) < 0) fail("tune frontend");
    unsigned consecutive = 0;
    uint64_t locked_samples = 0, lost = 0, last_logged = 0;
    fe_status_t last_status = 0;
    int have_last_status = 0, stable = 0;
    struct timespec start, deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) fail("read monotonic clock");
    deadline = start;
    deadline.tv_sec += o.duration;
    while (!stop) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) fail("read monotonic clock");
        if (o.duration && timespec_compare(&now, &deadline) >= 0) break;
        uint64_t second = elapsed_seconds(&start, &now);
        fe_status_t status = 0;
        if (ioctl(fd, FE_READ_STATUS, &status) < 0) fail("read status");
        if (status & FE_HAS_LOCK) {
            if (consecutive < 3) ++consecutive;
            if (locked_samples < UINT64_MAX) ++locked_samples;
        } else {
            if (consecutive && lost < UINT64_MAX) ++lost;
            consecutive = 0;
        }
        if (o.json || second < 10 || !have_last_status || status != last_status || second - last_logged >= 10) {
            print_status(&o, fd, status, second);
            last_logged = second;
        }
        have_last_status = 1;
        last_status = status;
        if (consecutive >= 3 && !stable) {
            stable = 1;
            if (o.json) printf("{\"event\":\"stable_lock\",\"adapter\":%u,\"lock\":true}\n", o.adapter);
            else puts("STABLE_LOCK: three consecutive FE_HAS_LOCK samples");
        }
        struct timespec wake = now;
        ++wake.tv_sec;
        if (o.duration && timespec_compare(&wake, &deadline) > 0) wake = deadline;
        int sleep_result;
        do {
            sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
        } while (sleep_result == EINTR && !stop);
        if (sleep_result && sleep_result != EINTR) { errno = sleep_result; fail("wait for next sample"); }
    }
    if (o.json)
        printf("{\"event\":\"result\",\"adapter\":%u,\"stable_lock\":%s,\"locked_samples\":%" PRIu64
               ",\"lock_losses\":%" PRIu64 "}\n", o.adapter, stable ? "true" : "false", locked_samples, lost);
    else
        printf("RESULT: stable_lock=%s locked_samples=%" PRIu64 " lock_losses=%" PRIu64
               "; closing frontend\n", stable ? "YES" : "NO", locked_samples, lost);
    close(fd);
    return stop || stable ? 0 : 3;
}
