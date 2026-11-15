/* SPDX-License-Identifier: GPL-2.0-only
 * Read-only frontend discovery and a bounded scan of an already tuned DVB TS.
 * A private TSDEMUX_TAP filter avoids consuming another program's DVR stream.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <iconv.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_SERVICES 512
#define MAX_CAIDS 32
#define SECTION_SIZE 4096
#define TEXT_SIZE 1024
#define TS_SIZE 188

struct service {
    uint16_t sid, pmt_pid, sdt_tsid;
    unsigned in_pat, sdt_seen, sdt_ca, pmt_seen;
    int pmt_version;
    char name[TEXT_SIZE], provider[TEXT_SIZE];
    uint16_t caids[MAX_CAIDS];
    size_t caid_count;
};

struct section_buffer {
    uint8_t data[SECTION_SIZE];
    size_t used, expected;
    int last_cc, have_cc;
};

struct scanner {
    struct service services[MAX_SERVICES];
    size_t service_count, buffer_count;
    struct section_buffer *pid[8192];
    int pat_version, pat_tsid;
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t mpeg_crc(const uint8_t *p, size_t n) {
    uint32_t crc = UINT32_MAX;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
    }
    return crc;
}

/* Emit ASCII JSON, validating UTF-8 even for an unexpected driver name. */
static void json_string(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    putchar('"');
    while (*p) {
        uint32_t cp = *p++;
        unsigned extra = 0;
        uint32_t minimum = 0;
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; extra = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; extra = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 0x07; extra = 3; minimum = 0x10000; }
        else if (cp >= 0x80) cp = 0xfffd;
        if (extra) {
            const uint8_t *q = p;
            unsigned j;
            for (j = 0; j < extra && *q && (*q & 0xc0) == 0x80; ++j)
                cp = (cp << 6) | (*q++ & 0x3f);
            if (j != extra || cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                cp = 0xfffd;
            else p = q;
        }
        if (cp == '"' || cp == '\\') printf("\\%c", (int)cp);
        else if (cp >= 0x20 && cp < 0x7f) putchar((int)cp);
        else if (cp <= 0xffff) printf("\\u%04" PRIx32, cp);
        else {
            cp -= 0x10000;
            printf("\\u%04" PRIx32 "\\u%04" PRIx32, 0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
        }
    }
    putchar('"');
}

/* EN 300 468 character selectors. iconv is part of glibc on the target image.
 * Unknown selectors/bad bytes become U+FFFD, never invalid JSON or shell text.
 */
static void dvb_text(char out[TEXT_SIZE], const uint8_t *data, size_t size) {
    const char *encoding = "ISO_6937";
    char iso[24];
    if (size && data[0] < 0x20) {
        uint8_t selector = *data++;
        --size;
        if (selector >= 1 && selector <= 11 && selector != 8) {
            snprintf(iso, sizeof(iso), "ISO-8859-%u", selector + 4);
            encoding = iso;
        } else if (selector == 0x10 && size >= 2 && data[0] == 0 && data[1] >= 1 && data[1] <= 15 && data[1] != 12) {
            snprintf(iso, sizeof(iso), "ISO-8859-%u", data[1]);
            encoding = iso;
            data += 2; size -= 2;
        } else if (selector == 0x11) encoding = "UTF-16BE";
        else if (selector == 0x12) encoding = "EUC-KR";
        else if (selector == 0x13) encoding = "GB2312";
        else if (selector == 0x14) encoding = "BIG5";
        else if (selector == 0x15) encoding = "UTF-8";
        else encoding = "ASCII";
    }
    iconv_t cd = iconv_open("UTF-8", encoding);
    char *dst = out;
    size_t room = TEXT_SIZE - 1;
    char *src = (char *)(uintptr_t)data;
    if (cd == (iconv_t)-1) {
        while (size && room >= 3) {
            unsigned char c = (unsigned char)*src++;
            --size;
            if (c >= 0x20 && c < 0x7f) { *dst++ = (char)c; --room; }
            else { memcpy(dst, "\xef\xbf\xbd", 3); dst += 3; room -= 3; }
        }
    } else {
        while (size && room >= 3) {
            if (iconv(cd, &src, &size, &dst, &room) != (size_t)-1) break;
            if (errno == E2BIG) break;
            memcpy(dst, "\xef\xbf\xbd", 3); dst += 3; room -= 3;
            ++src; --size;
            (void)iconv(cd, NULL, NULL, NULL, NULL);
        }
        iconv_close(cd);
    }
    *dst = 0;
    /* Drop DVB emphasis/control characters; render DVB line breaks as spaces. */
    char *r = out, *w = out;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x20 || c == 0x7f) { ++r; continue; }
        if (c == 0xc2 && (unsigned char)r[1] >= 0x80 && (unsigned char)r[1] <= 0x9f) {
            if ((unsigned char)r[1] == 0x8a) *w++ = ' ';
            r += 2; continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

static struct service *service_get(struct scanner *s, uint16_t sid, int create) {
    for (size_t i = 0; i < s->service_count; ++i)
        if (s->services[i].sid == sid) return &s->services[i];
    if (!create || !sid || s->service_count == MAX_SERVICES) return NULL;
    struct service *v = &s->services[s->service_count++];
    memset(v, 0, sizeof(*v));
    v->sid = sid;
    v->pmt_version = -1;
    return v;
}

static void add_caid(struct service *v, uint16_t caid) {
    for (size_t i = 0; i < v->caid_count; ++i)
        if (v->caids[i] == caid) return;
    if (v->caid_count < MAX_CAIDS) v->caids[v->caid_count++] = caid;
}

static struct section_buffer *buffer_get(struct scanner *s, unsigned pid) {
    if (!s->pid[pid] && s->buffer_count < MAX_SERVICES + 2) {
        s->pid[pid] = calloc(1, sizeof(*s->pid[pid]));
        if (!s->pid[pid]) { perror("allocate section buffer"); exit(1); }
        ++s->buffer_count;
    }
    return s->pid[pid];
}

static int descriptors_valid(const uint8_t *data, size_t size) {
    while (size) {
        if (size < 2 || (size_t)data[1] + 2 > size) return 0;
        size_t n = (size_t)data[1] + 2;
        data += n; size -= n;
    }
    return 1;
}

static void ca_descriptors(struct service *v, const uint8_t *p, size_t size) {
    while (size >= 2) {
        size_t n = (size_t)p[1] + 2;
        if (n > size) return;
        if (p[0] == 0x09 && p[1] >= 4) add_caid(v, be16(p + 2));
        p += n; size -= n;
    }
}

static void parse_pat(struct scanner *s, const uint8_t *p, size_t n) {
    if (n < 12 || (n - 12) % 4) return;
    int tsid = be16(p + 3), version = (p[5] >> 1) & 31;
    if (s->pat_tsid != tsid || s->pat_version != version) {
        for (size_t i = 0; i < s->service_count; ++i) s->services[i].in_pat = 0;
        s->pat_tsid = tsid; s->pat_version = version;
    }
    for (size_t i = 8; i + 4 <= n - 4; i += 4) {
        uint16_t sid = be16(p + i), pid = be16(p + i + 2) & 0x1fff;
        if (!sid || pid < 0x20 || pid == 0x1fff) continue;
        struct service *v = service_get(s, sid, 1);
        if (!v) continue;
        if (v->pmt_pid != pid) { v->pmt_seen = 0; v->caid_count = 0; v->pmt_version = -1; }
        v->pmt_pid = pid; v->in_pat = 1;
        (void)buffer_get(s, pid);
    }
}

static void parse_sdt(struct scanner *s, const uint8_t *p, size_t n) {
    if (n < 15) return;
    uint16_t tsid = be16(p + 3);
    if (s->pat_tsid >= 0 && s->pat_tsid != tsid) return;
    /* Validate the complete section before modifying results. */
    for (size_t pos = 11; pos < n - 4;) {
        if (n - 4 - pos < 5) return;
        size_t len = be16(p + pos + 3) & 0x0fff;
        if (len > n - 4 - pos - 5 || !descriptors_valid(p + pos + 5, len)) return;
        pos += 5 + len;
    }
    for (size_t pos = 11; pos < n - 4;) {
        size_t len = be16(p + pos + 3) & 0x0fff;
        struct service *v = service_get(s, be16(p + pos), 1);
        if (v) {
            v->sdt_tsid = tsid; v->sdt_seen = 1; v->sdt_ca = (p[pos + 3] & 0x10) != 0;
            size_t end = pos + 5 + len;
            for (size_t d = pos + 5; d < end; d += 2 + p[d + 1]) {
                size_t dl = p[d + 1];
                if (p[d] != 0x48 || dl < 3) continue;
                size_t pl = p[d + 3];
                if (pl + 3 > dl) continue;
                size_t nl = p[d + 4 + pl];
                if (pl + nl + 3 > dl) continue;
                dvb_text(v->provider, p + d + 4, pl);
                dvb_text(v->name, p + d + 5 + pl, nl);
            }
        }
        pos += 5 + len;
    }
}

static void parse_pmt(struct scanner *s, unsigned pid, const uint8_t *p, size_t n) {
    if (n < 16) return;
    struct service *v = service_get(s, be16(p + 3), 0);
    if (!v || !v->in_pat || v->pmt_pid != pid) return;
    size_t pi = be16(p + 10) & 0x0fff;
    if (pi > n - 16 || !descriptors_valid(p + 12, pi)) return;
    for (size_t pos = 12 + pi; pos < n - 4;) {
        if (n - 4 - pos < 5) return;
        size_t len = be16(p + pos + 3) & 0x0fff;
        if (len > n - 4 - pos - 5 || !descriptors_valid(p + pos + 5, len)) return;
        pos += 5 + len;
    }
    int version = (p[5] >> 1) & 31;
    if (v->pmt_version != version) { v->caid_count = 0; v->pmt_version = version; }
    v->pmt_seen = 1;
    ca_descriptors(v, p + 12, pi);
    for (size_t pos = 12 + pi; pos < n - 4;) {
        size_t len = be16(p + pos + 3) & 0x0fff;
        ca_descriptors(v, p + pos + 5, len);
        pos += 5 + len;
    }
}

static void parse_section(struct scanner *s, unsigned pid, const uint8_t *p, size_t n) {
    if (n < 12 || !(p[1] & 0x80) || !(p[5] & 1) || p[6] > p[7] || mpeg_crc(p, n)) return;
    if (pid == 0 && p[0] == 0x00) parse_pat(s, p, n);
    else if (pid == 0x11 && p[0] == 0x42) parse_sdt(s, p, n);
    else if (p[0] == 0x02) parse_pmt(s, pid, p, n);
}

static void reset_section(struct section_buffer *b) { b->used = b->expected = 0; }

static void section_feed(struct scanner *s, unsigned pid, struct section_buffer *b,
                         const uint8_t *p, size_t n, int can_start) {
    while (n) {
        if (!b->used && (!can_start || *p == 0xff)) return;
        if (b->used < 3) {
            size_t take = 3 - b->used;
            if (take > n) take = n;
            memcpy(b->data + b->used, p, take); b->used += take; p += take; n -= take;
            if (b->used < 3) return;
            b->expected = 3 + (be16(b->data + 1) & 0x0fff);
            if (b->expected < 12 || b->expected > SECTION_SIZE) { reset_section(b); return; }
        }
        size_t take = b->expected - b->used;
        if (take > n) take = n;
        memcpy(b->data + b->used, p, take); b->used += take; p += take; n -= take;
        if (b->used == b->expected) {
            parse_section(s, pid, b->data, b->used);
            reset_section(b);
        }
    }
}

static void ts_packet(struct scanner *s, const uint8_t *p) {
    unsigned pid = be16(p + 1) & 0x1fff;
    struct section_buffer *b = s->pid[pid];
    if (!b) return;
    unsigned afc = (p[3] >> 4) & 3, cc = p[3] & 15;
    if ((p[1] & 0x80) || (p[3] & 0xc0) || !afc) { reset_section(b); b->have_cc = 0; return; }
    size_t pos = 4;
    if (afc & 2) {
        size_t len = p[4];
        if (len > 183) { reset_section(b); b->have_cc = 0; return; }
        if (len && (p[5] & 0x80)) { reset_section(b); b->have_cc = 0; }
        pos += 1 + len;
    }
    if (!(afc & 1) || pos >= TS_SIZE) return;
    if (b->have_cc) {
        if (cc == (unsigned)b->last_cc) return; /* Duplicate TS packet. */
        if (cc != ((unsigned)b->last_cc + 1) % 16) reset_section(b);
    }
    b->have_cc = 1; b->last_cc = (int)cc;
    if (p[1] & 0x40) {
        size_t pointer = p[pos++];
        if (pointer > TS_SIZE - pos) { reset_section(b); return; }
        if (b->used) section_feed(s, pid, b, p + pos, pointer, 0);
        reset_section(b);
        pos += pointer;
        section_feed(s, pid, b, p + pos, TS_SIZE - pos, 1);
    } else if (b->used) section_feed(s, pid, b, p + pos, TS_SIZE - pos, 0);
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int scan_fd(struct scanner *s, int fd, unsigned seconds, int live) {
    uint8_t data[TS_SIZE * 256 + TS_SIZE];
    size_t used = 0;
    int64_t end = monotonic_ms() + (int64_t)seconds * 1000;
    while (monotonic_ms() < end) {
        int64_t remain = end - monotonic_ms();
        if (remain <= 0) break;
        struct pollfd pollfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pollfd, 1, remain > 1000 ? 1000 : (int)remain);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll scan"); return 1; }
        if (!ready) continue;
        ssize_t got = read(fd, data + used, sizeof(data) - used);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (live && errno == EOVERFLOW) {
                for (size_t i = 0; i < 8192; ++i)
                    if (s->pid[i]) { reset_section(s->pid[i]); s->pid[i]->have_cc = 0; }
                used = 0; continue;
            }
            perror("read scan"); return 1;
        }
        if (!got) break;
        used += (size_t)got;
        size_t pos = 0;
        while (used - pos >= TS_SIZE) {
            if (data[pos] != 0x47 || (used - pos >= TS_SIZE * 2 && data[pos + TS_SIZE] != 0x47)) { ++pos; continue; }
            ts_packet(s, data + pos);
            pos += TS_SIZE;
        }
        memmove(data, data + pos, used - pos); used -= pos;
    }
    return 0;
}

static int compare_service(const void *a, const void *b) {
    const struct service *sa = a, *sb = b;
    return (int)sa->sid - (int)sb->sid;
}

static void print_services(struct scanner *s) {
    qsort(s->services, s->service_count, sizeof(s->services[0]), compare_service);
    putchar('[');
    int comma = 0;
    for (size_t i = 0; i < s->service_count; ++i) {
        struct service *v = &s->services[i];
        if (!v->in_pat) continue;
        int have_sdt = v->sdt_seen && v->sdt_tsid == s->pat_tsid;
        if (comma++) putchar(',');
        printf("{\"sid\":%u,\"name\":", v->sid);
        json_string(have_sdt ? v->name : "");
        printf(",\"provider\":"); json_string(have_sdt ? v->provider : "");
        printf(",\"scrambled\":%s,\"caids\":[", (v->caid_count || (have_sdt && v->sdt_ca)) ? "true" : "false");
        for (size_t j = 0; j < v->caid_count; ++j) printf("%s\"%04X\"", j ? "," : "", v->caids[j]);
        printf("],\"pmt_pid\":%u,\"pmt_seen\":%s,\"sdt_seen\":%s}", v->pmt_pid,
               v->pmt_seen ? "true" : "false", have_sdt ? "true" : "false");
    }
    puts("]");
}

static const char *delivery_name(unsigned d) {
    switch (d) {
    case SYS_DVBS: return "DVB-S";
    case SYS_DVBS2: return "DVB-S2";
    case SYS_DSS: return "DSS";
    case SYS_TURBO: return "TURBO";
    case SYS_DVBT: return "DVB-T";
    case SYS_DVBT2: return "DVB-T2";
    case SYS_DVBC_ANNEX_A: return "DVB-C/A";
    case SYS_DVBC_ANNEX_B: return "DVB-C/B";
    case SYS_DVBC_ANNEX_C: return "DVB-C/C";
    case SYS_ATSC: return "ATSC";
    case SYS_ISDBT: return "ISDB-T";
    case SYS_ISDBS: return "ISDB-S";
    default: return "UNKNOWN";
    }
}

static int list_frontends(void) {
    glob_t paths;
    memset(&paths, 0, sizeof(paths));
    int gr = glob("/dev/dvb/adapter*/frontend0", 0, NULL, &paths);
    if (gr && gr != GLOB_NOMATCH) { fputs("Cannot enumerate DVB frontends\n", stderr); globfree(&paths); return 1; }
    putchar('[');
    int comma = 0;
    for (size_t i = 0; i < paths.gl_pathc; ++i) {
        unsigned adapter;
        int consumed = 0;
        if (sscanf(paths.gl_pathv[i], "/dev/dvb/adapter%u/frontend0%n", &adapter, &consumed) != 1 || paths.gl_pathv[i][consumed]) continue;
        int fd = open(paths.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        struct dvb_frontend_info info;
        memset(&info, 0, sizeof(info));
        int ok = fd >= 0 && ioctl(fd, FE_GET_INFO, &info) == 0;
        int saved_errno = errno;
        if (comma++) putchar(',');
        printf("{\"adapter\":%u,\"frontend\":0,\"path\":", adapter); json_string(paths.gl_pathv[i]);
        printf(",\"available\":%s,\"name\":", ok ? "true" : "false");
        info.name[sizeof(info.name) - 1] = 0;
        json_string(ok ? info.name : "");
        if (!ok) { printf(",\"error\":"); json_string(strerror(saved_errno)); }
        printf(",\"capabilities\":%" PRIu32 ",\"delivery_systems\":[", (uint32_t)info.caps);
        if (ok) {
            struct dtv_property prop;
            memset(&prop, 0, sizeof(prop));
            prop.cmd = DTV_ENUM_DELSYS;
            struct dtv_properties props = { .num = 1, .props = &prop };
            if (ioctl(fd, FE_GET_PROPERTY, &props) == 0 && prop.result == 0) {
                unsigned count = prop.u.buffer.len;
                if (count > sizeof(prop.u.buffer.data)) count = sizeof(prop.u.buffer.data);
                for (unsigned j = 0; j < count; ++j) { if (j) putchar(','); json_string(delivery_name(prop.u.buffer.data[j])); }
            } else if (info.type == FE_QPSK) {
                printf("\"DVB-S\"%s", (info.caps & FE_CAN_2G_MODULATION) ? ",\"DVB-S2\"" : "");
            }
        }
        printf("],\"frequency_min\":%u,\"frequency_max\":%u,\"frequency_unit\":\"%s\",\"symbol_rate_min\":%u,\"symbol_rate_max\":%u}",
               info.frequency_min, info.frequency_max, info.type == FE_QPSK ? "kHz" : "Hz", info.symbol_rate_min, info.symbol_rate_max);
        if (fd >= 0) close(fd);
    }
    puts("]");
    globfree(&paths);
    return 0;
}

static int number(const char *s, unsigned limit, unsigned *out) {
    if (!*s) return 0;
    for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    unsigned long n = strtoul(s, NULL, 10);
    if (errno || n > limit) return 0;
    *out = (unsigned)n;
    return 1;
}

static int usage(void) {
    fputs("Usage: dvb-scan --list\n"
          "       dvb-scan --scan --adapter N [--scan-seconds 1..60]\n"
          "       dvb-scan --input FILE [--scan-seconds 1..60]\n"
          "Scans an already tuned transport stream; never tunes or changes LNB voltage.\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    unsigned adapter = 0, seconds = 12;
    int scan = 0, listed = 0, adapter_set = 0;
    const char *input = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--list")) listed = 1;
        else if (!strcmp(argv[i], "--scan")) scan = 1;
        else if (!strcmp(argv[i], "--adapter") && i + 1 < argc) {
            if (!number(argv[++i], 4095, &adapter)) return usage();
            adapter_set = 1;
        } else if (!strcmp(argv[i], "--scan-seconds") && i + 1 < argc) {
            if (!number(argv[++i], 60, &seconds) || !seconds) return usage();
        } else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else return usage();
    }
    if (listed) return argc == 2 ? list_frontends() : usage();
    if ((!scan && !input) || (input && (scan || adapter_set)) || (scan && !adapter_set)) return usage();
    int fd;
    if (input) fd = open(input, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    else {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dvb/adapter%u/demux0", adapter);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            /* Each demux open owns its filter; closing it does not stop DVBlast. */
            (void)ioctl(fd, DMX_SET_BUFFER_SIZE, 4 * 1024 * 1024);
            struct dmx_pes_filter_params filter = {
                .pid = 0x2000, .input = DMX_IN_FRONTEND, .output = DMX_OUT_TSDEMUX_TAP,
                .pes_type = DMX_PES_OTHER, .flags = DMX_IMMEDIATE_START
            };
            if (ioctl(fd, DMX_SET_PES_FILTER, &filter)) { perror("DMX_SET_PES_FILTER (private TS tap)"); close(fd); return 1; }
        }
    }
    if (fd < 0) { perror(input ? "open input" : "open demux0"); return 1; }
    struct scanner *s = calloc(1, sizeof(*s));
    if (!s) { perror("allocate scanner"); close(fd); return 1; }
    s->pat_version = s->pat_tsid = -1;
    (void)buffer_get(s, 0); (void)buffer_get(s, 0x11);
    int result = scan_fd(s, fd, seconds, !input);
    close(fd);
    if (!result) print_services(s);
    for (size_t i = 0; i < 8192; ++i) free(s->pid[i]);
    free(s);
    return result;
}
