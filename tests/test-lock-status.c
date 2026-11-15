/* SPDX-License-Identifier: GPL-2.0-only
 * Exercise metric scales without any real frontend. */
#define main helper_main
#define ioctl fake_ioctl
#include "../src/unicable-tune.c"
#undef ioctl
#undef main
#include <assert.h>
#include <stdarg.h>

static int mode;
int fake_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == -1 && request == FE_GET_PROPERTY);
    va_list args;
    va_start(args, request);
    struct dtv_properties *ps = va_arg(args, struct dtv_properties *);
    va_end(args);
    struct dtv_property *p = ps->props;
    if (mode == 0) { errno = EOPNOTSUPP; return -1; }
    if (mode == 1) {
        p->u.st.len = 2;
        p->u.st.stat[0].scale = FE_SCALE_DECIBEL;
        p->u.st.stat[0].svalue = p->cmd == DTV_STAT_CNR ? 10875 : -43250;
        p->u.st.stat[1].scale = FE_SCALE_RELATIVE;
        p->u.st.stat[1].uvalue = 65535;
    } else {
        p->u.st.len = MAX_DTV_STATS;
        p->u.st.stat[0].scale = FE_SCALE_RELATIVE;
        p->u.st.stat[0].uvalue = 65536; /* Invalid values must not become >100%. */
        p->u.st.stat[1].scale = FE_SCALE_COUNTER;
        p->u.st.stat[1].uvalue = 123456; /* A counter is not CNR/strength. */
    }
    return 0;
}
int main(void)
{
    struct signal_stats s = read_stats(-1);
    assert(!s.cnr_db.present && !s.strength_dbm.present);
    assert(!s.strength_percent.present && !s.quality_percent.present);
    mode = 1;
    s = read_stats(-1);
    assert(s.cnr_db.present && s.cnr_db.value == 10.875);
    assert(s.strength_dbm.present && s.strength_dbm.value == -43.25);
    assert(s.strength_percent.present && s.strength_percent.value == 100.0);
    assert(s.quality_percent.present && s.quality_percent.value == 100.0);
    mode = 2;
    s = read_stats(-1);
    assert(!s.cnr_db.present && !s.strength_dbm.present);
    assert(!s.strength_percent.present && !s.quality_percent.present);
    puts("PASS metric scaling, native units and unavailable/invalid values");
    return 0;
}
