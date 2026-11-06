/* avm1_date.c - the AVM1 Date class (ECMA-262 3rd edition time
 * arithmetic with Flash Player's quirks). */
#include "avm1_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
/* platform: milliseconds since 1970 (UTC) and the local time zone offset in minutes (local - UTC) */
extern double plat_now_ms(void);
extern int plat_tz_minutes(void);

#define MS_PER_SECOND 1000
#define MS_PER_MINUTE 60000
#define MS_PER_HOUR 3600000
#define MS_PER_DAY 86400000

static double dnan(void) { double z = 0; return z / z; }
static s32 clamp_i32(double d) { if (d >= -2147483648.0 && d <= 2147483647.0) return (s32)d; return (s32)0x80000000u; }
static s32 rem_euclid_i32(double lhs, s32 rhs) { s32 r = clamp_i32(fmod(lhs, (double)rhs)); return r < 0 ? r + rhs : r; }
static int finite_d(double d) { return d - d == 0; }

static const unsigned short month_offsets[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 } };

static double day_of(double t) { return floor(t / MS_PER_DAY); }
static double day_from_year(double y) { return 365.0 * (y - 1970.0) + floor((y - 1969.0) / 4.0) - floor((y - 1901.0) / 100.0) + floor((y - 1601.0) / 400.0); }
static double from_year(s32 y) { return (double)MS_PER_DAY * day_from_year(y); }
static s32 year_of(double t) {
    double day = day_of(t);
    s32 low = clamp_i32(floor(day / (t < 0 ? 365.0 : 366.0))) + 1970;
    s32 high = clamp_i32(ceil(day / (t < 0 ? 366.0 : 365.0))) + 1970;
    while (low < high) {
        s32 pivot = clamp_i32(((double)low + (double)high) / 2.0);
        if (from_year(pivot) <= t) {
            if (from_year(pivot + 1) > t) return pivot;
            low = pivot + 1;
        } else high = pivot - 1;
    }
    return low;
}
static int is_leap(s32 y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static s32 day_within_year(double t) { return clamp_i32(day_of(t) - day_from_year(year_of(t))); }
static s32 month_of(double t) {
    s32 d = day_within_year(t); int leap = is_leap(year_of(t)), i;
    for (i = 0; i < 11; i++) if (d < month_offsets[leap][i + 1]) return i;
    return 11;
}
static s32 date_of(double t) { s32 m = month_of(t); return day_within_year(t) - month_offsets[is_leap(year_of(t))][m] + 1; }
static s32 week_day(double t) { return rem_euclid_i32(day_of(t) + 4.0, 7); }
static s32 hours_of(double t) { return rem_euclid_i32(floor((t + 0.5) / MS_PER_HOUR), 24); }
static s32 minutes_of(double t) { return rem_euclid_i32(floor(t / MS_PER_MINUTE), 60); }
static s32 seconds_of(double t) { return rem_euclid_i32(floor(t / MS_PER_SECOND), 60); }
static s32 ms_of(double t) { return rem_euclid_i32(t, MS_PER_SECOND); }
static double time_within_day(double t, int swf_version) {
    if (swf_version > 7) { double r = fmod(t, (double)MS_PER_DAY); return r < 0 ? r + MS_PER_DAY : r; }
    return fmod(t, (double)MS_PER_DAY);
}
static double make_time(double h, double m, double s, double ms) { return floor(h) * MS_PER_HOUR + floor(m) * MS_PER_MINUTE + floor(s) * MS_PER_SECOND + floor(ms); }
static double day_from_month(double year, double month) {
    s32 y = clamp_i32(year), mo = clamp_i32(floor(month));
    if (mo < 0 || mo >= 12) return dnan();
    return day_from_year(y) + month_offsets[is_leap(y)][mo];
}
static double make_day(double year, double month, double date) {
    double m;
    year = floor(year); month = floor(month); date = floor(date);
    year += floor(month / 12.0);
    m = fmod(month, 12.0); if (m < 0) m += 12.0;
    return day_from_month(year, m) + date - 1.0;
}
static double make_date(double day, double time) { return day * MS_PER_DAY + time; }
static double clip_t(double t) { if (!finite_d(t) || fabs(t) > 100000000.0 * MS_PER_DAY) return dnan(); return floor(t); }
static double new_date(double year, double month, double date, double h, double mi, double s, double ms) {
    if (year < 100.0) year += 1900.0;
    return make_date(make_day(year, month, date), make_time(h, mi, s, ms));
}
static double tza(void) { return (double)plat_tz_minutes() * 60000.0; }
static double to_local(double t) { return t + tza(); }
static double to_utc(double t) { return t - tza(); }
static double tz_offset(double t) { return (t - to_local(t)) / MS_PER_MINUTE; }

static AStr *date_string(VM *vm, double t) {
    static const char *dw[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char b[96]; double lt; s32 tz, atz;
    /* Flash 9 prints an infinite time as a zeroed date */
    if (!finite_d(t)) return str_intern(vm, t == t && avm1_player_major() <= 9 ? "Sun Jan 1 00:00:00 GMT+0000 1970" : "Invalid Date");
    lt = to_local(t);
    tz = clamp_i32(-tz_offset(t)); atz = tz < 0 ? -tz : tz;
    sprintf(b, "%s %s %ld %02ld:%02ld:%02ld GMT%c%02ld%02ld %ld", dw[week_day(lt)], mn[month_of(lt)], (long)date_of(lt),
            (long)hours_of(lt), (long)minutes_of(lt), (long)seconds_of(lt), tz < 0 ? '-' : '+', (long)(atz / 60), (long)(atz % 60), (long)year_of(lt));
    return str_cstr(vm, b);
}

enum { GET_FULL_YEAR = 0, GET_YEAR, GET_MONTH, GET_DATE, GET_DAY, GET_HOURS, GET_MINUTES, GET_SECONDS, GET_MILLISECONDS,
       SET_FULL_YEAR, SET_MONTH, SET_DATE, SET_HOURS, SET_MINUTES, SET_SECONDS, SET_MILLISECONDS, GET_TIME, SET_TIME,
       GET_TIMEZONE_OFFSET, TO_STRING, SET_YEAR, DM_CONSTRUCTOR = 256, DM_UTC = 257 };

static double *date_ref(AObj *o) { o = o && o->nk == NK_SUPER ? o->nat.sup.self : o; if (o && o->relay) o = o->relay; return o && o->nk == NK_DATE ? &o->nat.n : NULL; }

static double now_ms(void) { return plat_now_ms(); }

Value date_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    double args[7]; int n = 0, i, is_utc, is_get, is_set_year; double *ref, t, d;
    for (i = 0; i < argc && i < 7; i++) {
        if (argv[i].t == V_UNDEF) break;
        args[n++] = v_num_of(a, argv[i]);
        if (a->vm->err) return v_undef();
    }
    if (index == DM_CONSTRUCTOR) {
        if (n == 0) { t = now_ms(); if (a->swf_version > 7) t = floor(t + 0.5); }
        else if (n == 1) t = args[0];
        else {
            /* Flash 9: a non-finite year or month gives local 0000-01-01 00:00 */
            int bad = !finite_d(args[0]) || !finite_d(args[1]);
            if (bad && avm1_player_major() <= 9) t = to_utc(from_year(0));
            else t = to_utc(new_date(args[0], args[1], n > 2 ? args[2] : 1, n > 3 ? args[3] : 0, n > 4 ? args[4] : 0, n > 5 ? args[5] : 0, n > 6 ? args[6] : 0));
        }
        { AObj *o = o_native_relay(a, self, NK_DATE); if (o) { o->nk = NK_DATE; o->nat.n = t; } }
        return v_obj(self);
    }
    if (index == DM_UTC) {
        if (n < 2) return v_undef();
        return v_num(new_date(args[0], args[1], n > 2 ? args[2] : 1, n > 3 ? args[3] : 0, n > 4 ? args[4] : 0, n > 5 ? args[5] : 0, n > 6 ? args[6] : 0));
    }
    ref = date_ref(self);
    if (!ref) return v_undef();
    t = *ref;
    if (index == GET_TIME) return v_num(t);
    if (index == SET_TIME) { double nt = clip_t(n > 0 ? args[0] : dnan()); *ref = nt; return v_num(nt); }
    if (index == GET_TIMEZONE_OFFSET) return v_num(tz_offset(t));
    is_utc = index >= 128;
    if (is_utc) index -= 128;
    is_get = (index >= GET_FULL_YEAR && index <= GET_MILLISECONDS);
    if (is_get && t != t) return v_num(dnan());
    is_set_year = index == SET_YEAR;
    if (is_set_year) index = SET_FULL_YEAR;
    d = is_utc ? t : to_local(t);
#define HAS(k) ((k) >= index && (k) - index < n)
#define ARG(k) (HAS(k) ? args[(k) - index] : ((k) == index ? dnan() : 0))
#define PRESENT(k) (HAS(k) || (k) == index)
    switch (index) {
    case GET_FULL_YEAR: return v_num(year_of(d));
    case GET_YEAR: return v_num(year_of(d) - 1900);
    case GET_MONTH: return v_num(month_of(d));
    case GET_DATE: return v_num(date_of(d));
    case GET_DAY: return v_num(week_day(d));
    case GET_HOURS: return v_num(hours_of(d));
    case GET_MINUTES: return v_num(minutes_of(d));
    case GET_SECONDS: return v_num(seconds_of(d));
    case GET_MILLISECONDS: return v_num(ms_of(d));
    case SET_FULL_YEAR: case SET_MONTH: case SET_DATE: {
        double year, month, date, nt;
        if (PRESENT(SET_FULL_YEAR)) { year = ARG(SET_FULL_YEAR); if (is_set_year && year >= 0.0 && year <= 99.0) year += 1900.0; }
        else year = year_of(d);
        month = PRESENT(SET_MONTH) ? ARG(SET_MONTH) : month_of(d);
        if (index == SET_MONTH && month != month) month = 0.0;
        date = PRESENT(SET_DATE) ? ARG(SET_DATE) : date_of(d);
        nt = make_date(make_day(year, month, date), time_within_day(d, a->swf_version));
        if (!is_utc) nt = to_utc(nt);
        nt = clip_t(nt);
        *ref = nt;
        return v_num(nt); }
    case SET_HOURS: case SET_MINUTES: case SET_SECONDS: case SET_MILLISECONDS: {
        double h = PRESENT(SET_HOURS) ? ARG(SET_HOURS) : hours_of(d);
        double mi = PRESENT(SET_MINUTES) ? ARG(SET_MINUTES) : minutes_of(d);
        double s = PRESENT(SET_SECONDS) ? ARG(SET_SECONDS) : seconds_of(d);
        double ms = PRESENT(SET_MILLISECONDS) ? ARG(SET_MILLISECONDS) : ms_of(d);
        double nt;
        if (index == SET_MINUTES) { mi = clamp_i32(mi); s = clamp_i32(s); ms = clamp_i32(ms); }
        nt = make_date(day_of(d), make_time(h, mi, s, ms));
        if (!is_utc) nt = to_utc(nt);
        nt = clip_t(nt);
        *ref = nt;
        return v_num(nt); }
    case TO_STRING: return v_str(date_string(a->vm, t));
    default: return v_undef();
    }
#undef HAS
#undef ARG
#undef PRESENT
}

static Value date_ctor(Act *a, AObj *self, int argc, Value *argv) { return date_method(a, self, argc, argv, DM_CONSTRUCTOR); }
static Value date_function(Act *a, AObj *self, int argc, Value *argv) { (void)self; (void)argc; (void)argv; return v_str(date_string(a->vm, now_ms())); }

/* ASnative(103, n): ASnative(103, 256) called as a function gives the date string */
Value date_native_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    if (index == DM_CONSTRUCTOR && !a->vm->table_ctor) return date_function(a, self, argc, argv);
    return date_method(a, self, argc, argv, index);
}

AObj *lib_date(VM *vm, Env *env) {
    static const struct { const char *n; int i; } m[] = {
        { "getFullYear", GET_FULL_YEAR }, { "getYear", GET_YEAR }, { "getMonth", GET_MONTH }, { "getDate", GET_DATE },
        { "getDay", GET_DAY }, { "getHours", GET_HOURS }, { "getMinutes", GET_MINUTES }, { "getSeconds", GET_SECONDS },
        { "getMilliseconds", GET_MILLISECONDS }, { "setFullYear", SET_FULL_YEAR }, { "setMonth", SET_MONTH },
        { "setDate", SET_DATE }, { "setHours", SET_HOURS }, { "setMinutes", SET_MINUTES }, { "setSeconds", SET_SECONDS },
        { "setMilliseconds", SET_MILLISECONDS }, { "getTime", GET_TIME }, { "setTime", SET_TIME },
        { "getTimezoneOffset", GET_TIMEZONE_OFFSET }, { "toString", TO_STRING }, { "setYear", SET_YEAR },
        { "getUTCFullYear", 128 + GET_FULL_YEAR }, { "getUTCYear", 128 + GET_YEAR }, { "getUTCMonth", 128 + GET_MONTH },
        { "getUTCDate", 128 + GET_DATE }, { "getUTCDay", 128 + GET_DAY }, { "getUTCHours", 128 + GET_HOURS },
        { "getUTCMinutes", 128 + GET_MINUTES }, { "getUTCSeconds", 128 + GET_SECONDS }, { "getUTCMilliseconds", 128 + GET_MILLISECONDS },
        { "setUTCFullYear", 128 + SET_FULL_YEAR }, { "setUTCMonth", 128 + SET_MONTH }, { "setUTCDate", 128 + SET_DATE },
        { "setUTCHours", 128 + SET_HOURS }, { "setUTCMinutes", 128 + SET_MINUTES }, { "setUTCSeconds", 128 + SET_SECONDS },
        { "setUTCMilliseconds", 128 + SET_MILLISECONDS }, { "valueOf", GET_TIME }, { NULL, 0 } };
    AObj *proto, *c = lib_class(vm, env, date_ctor, date_function, env->p.object, 1, &proto); int i;
    env->p.date = proto; env->p.date_ctor = c;
    for (i = 0; m[i].n; i++) o_define(vm, proto, str_intern(vm, m[i].n), v_obj(fn_table(vm, env, date_method, m[i].i)), DE | DD);
    o_define(vm, c, str_intern(vm, "UTC"), v_obj(fn_table(vm, env, date_method, DM_UTC)), DE | DD | RO);
    return c;
}
