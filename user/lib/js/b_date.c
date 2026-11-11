/*
 * Date (27o): time values in milliseconds since 1970 UTC, the calendar by
 * the specification's day arithmetic (any year a time value reaches), the
 * local zone from the C library's localtime (on RitterOS the zone of
 * /system/config/time). Date.parse takes the ISO format exactly and, as
 * browsers do, the usual other forms (toString's and toUTCString's,
 * "Oct 26, 2026 10:00", "10/26/2026").
 */
#include <math.h>
#include <time.h>
#include "jsint.h"

#define MS_DAY 86400000.0

static double day_of(double t) { return floor(t / MS_DAY); }

static double time_within_day(double t)
{
    double r = fmod(t, MS_DAY);
    return r < 0 ? r + MS_DAY : r;
}

static double day_from_year(double y)
{
    return 365 * (y - 1970) + floor((y - 1969) / 4) - floor((y - 1901) / 100) + floor((y - 1601) / 400);
}

static double time_from_year(double y) { return MS_DAY * day_from_year(y); }

static bool leap(double y)
{
    return fmod(y, 4) == 0 && (fmod(y, 100) != 0 || fmod(y, 400) == 0);
}

static double year_from_time(double t)
{
    double y = floor(t / (MS_DAY * 365.2425)) + 1970;
    while (time_from_year(y) > t)
        y--;
    while (time_from_year(y + 1) <= t)
        y++;
    return y;
}

static const int month_start[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

static void split_date(double t, double *year, int *month, int *date)
{
    double y = year_from_time(t);
    int dy = (int)(day_of(t) - day_from_year(y));
    int l = leap(y);
    int m = 0;
    while (m < 11 && dy >= month_start[l][m + 1])
        m++;
    *year = y;
    *month = m;
    *date = dy - month_start[l][m] + 1;
}

static double make_day(double year, double month, double date)
{
    if (!isfinite(year) || !isfinite(month) || !isfinite(date))
        return NAN;
    double y = trunc(year), m = trunc(month), dt = trunc(date);
    double ym = y + floor(m / 12);
    if (fabs(ym) > 400000)
        return NAN;
    int mn = (int)fmod(m, 12);
    if (mn < 0)
        mn += 12;
    double day = day_from_year(ym) + month_start[leap(ym)][mn];
    return day + dt - 1;
}

static double make_time(double h, double m, double s, double ms)
{
    if (!isfinite(h) || !isfinite(m) || !isfinite(s) || !isfinite(ms))
        return NAN;
    return trunc(h) * 3600000.0 + trunc(m) * 60000.0 + trunc(s) * 1000.0 + trunc(ms);
}

static double make_date(double day, double time)
{
    if (!isfinite(day) || !isfinite(time))
        return NAN;
    double tv = day * MS_DAY + time;
    return isfinite(tv) ? tv : NAN;
}

static double time_clip(double t)
{
    if (!isfinite(t) || fabs(t) > 8.64e15)
        return NAN;
    return trunc(t) + 0.0;
}

/* the local zone's offset (ms) at UTC time t */
static double offset_at(double t)
{
    if (t != t)
        return 0;
    double secs = floor(t / 1000);
    if (secs < -67768040609740800.0 || secs > 67768036191676799.0)
        return 0;
    time_t tt = (time_t)secs;
    struct tm *lt = localtime(&tt);
    if (!lt)
        return 0;
    double day = make_day(lt->tm_year + 1900.0, lt->tm_mon, lt->tm_mday);
    double local = make_date(day, make_time(lt->tm_hour, lt->tm_min, lt->tm_sec, 0));
    return local - secs * 1000;
}

static double local_time(double t)
{
    return t + offset_at(t);
}

static double utc_of_local(double t)
{
    if (t != t)
        return NAN;
    double off = offset_at(t);
    off = offset_at(t - off);
    return t - off;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return floor(ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
}

static double this_time(struct js *J, jsval v, bool *threw)
{
    if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_DATE)
        return js_number_value(to_obj(v)->u.prim);
    throw_type(J, "this is not a Date object.");
    *threw = true;
    return NAN;
}

/* --- parsing ------------------------------------------------------------ */

static int digits(const uint16_t *s, uint32_t n, uint32_t *i, int count, int *v)
{
    int x = 0;
    for (int k = 0; k < count; k++) {
        if (*i >= n || s[*i] < '0' || s[*i] > '9')
            return -1;
        x = x * 10 + (s[(*i)++] - '0');
    }
    *v = x;
    return 0;
}

static double parse_iso(const uint16_t *s, uint32_t n, bool *ok)
{
    uint32_t i = 0;
    int sign = 1, y, mo = 1, d = 1, h = 0, mi = 0, se = 0, ms = 0;
    *ok = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        sign = s[i] == '-' ? -1 : 1;
        i++;
        if (digits(s, n, &i, 6, &y) < 0)
            return NAN;
        if (sign < 0 && y == 0)
            return NAN;
    } else if (digits(s, n, &i, 4, &y) < 0)
        return NAN;
    bool date_only = true, has_offset = false;
    double offset = 0;
    if (i < n && s[i] == '-') {
        i++;
        if (digits(s, n, &i, 2, &mo) < 0)
            return NAN;
        if (i < n && s[i] == '-') {
            i++;
            if (digits(s, n, &i, 2, &d) < 0)
                return NAN;
        }
    }
    if (i < n && (s[i] == 'T' || s[i] == 't')) {
        date_only = false;
        i++;
        if (digits(s, n, &i, 2, &h) < 0 || i >= n || s[i] != ':')
            return NAN;
        i++;
        if (digits(s, n, &i, 2, &mi) < 0)
            return NAN;
        if (i < n && s[i] == ':') {
            i++;
            if (digits(s, n, &i, 2, &se) < 0)
                return NAN;
            if (i < n && (s[i] == '.' || s[i] == ',')) {
                i++;
                int count = 0;
                ms = 0;
                while (i < n && s[i] >= '0' && s[i] <= '9') {
                    if (count < 3)
                        ms = ms * 10 + (s[i] - '0');
                    count++;
                    i++;
                }
                if (!count)
                    return NAN;
                while (count++ < 3)
                    ms *= 10;
            }
        }
        if (i < n && (s[i] == 'Z' || s[i] == 'z')) {
            has_offset = true;
            i++;
        } else if (i < n && (s[i] == '+' || s[i] == '-')) {
            int osign = s[i] == '-' ? -1 : 1, oh, om;
            i++;
            if (digits(s, n, &i, 2, &oh) < 0 || i >= n || s[i] != ':')
                return NAN;
            i++;
            if (digits(s, n, &i, 2, &om) < 0)
                return NAN;
            has_offset = true;
            offset = osign * (oh * 60 + om) * 60000.0;
        }
    }
    if (i != n)
        return NAN;
    *ok = true;                                 /* the ISO form: its verdict stands */
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || se > 59 || (h == 24 && (mi || se || ms)))
        return NAN;
    int dim = month_start[leap(sign * (double)y)][mo] - month_start[leap(sign * (double)y)][mo - 1];
    if (d > dim)
        return NAN;
    double t = make_date(make_day(sign * (double)y, mo - 1, d), make_time(h, mi, se, ms));
    if (date_only || has_offset)
        return t - offset;
    return utc_of_local(t);
}

static int month_by_name(const uint16_t *w, uint32_t n)
{
    static const char *const months[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                          "jul", "aug", "sep", "oct", "nov", "dec" };
    if (n < 3)
        return -1;
    for (int m = 0; m < 12; m++) {
        bool same = true;
        for (int k = 0; k < 3; k++)
            if ((w[k] | 0x20) != (uint16_t)months[m][k])
                same = false;
        if (same)
            return m;
    }
    return -1;
}

/* the forms other browsers take: words, numbers, h:m:s, GMT+hhmm, (comments) */
static double parse_legacy(const uint16_t *s, uint32_t n)
{
    int month = -1, h = -1, mi = 0, se = 0, ms = 0;
    double nums[3];
    int nn = 0, slash_form = 0;
    bool pm = false, am = false, utc = false;
    double offset = 0;
    uint32_t i = 0;
    while (i < n) {
        uint32_t c = s[i];
        if (c == '(') {
            int depth = 1;
            i++;
            while (i < n && depth) {
                if (s[i] == '(')
                    depth++;
                else if (s[i] == ')')
                    depth--;
                i++;
            }
            continue;
        }
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            uint32_t a = i;
            while (i < n && (s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z')
                i++;
            uint32_t len = i - a;
            if (len == 2 && (s[a] | 0x20) == 'a' && (s[a + 1] | 0x20) == 'm')
                am = true;
            else if (len == 2 && (s[a] | 0x20) == 'p' && (s[a + 1] | 0x20) == 'm')
                pm = true;
            else if ((len == 3 && (s[a] | 0x20) == 'g' && (s[a + 1] | 0x20) == 'm' && (s[a + 2] | 0x20) == 't') ||
                     (len == 3 && (s[a] | 0x20) == 'u' && (s[a + 1] | 0x20) == 't' && (s[a + 2] | 0x20) == 'c') ||
                     (len == 1 && (s[a] | 0x20) == 'z'))
                utc = true;
            else if (month < 0 && month_by_name(s + a, len) >= 0)
                month = month_by_name(s + a, len);
            else if (len == 1 && (s[a] | 0x20) == 't')
                ;
            continue;
        }
        if ((c == '+' || c == '-') && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9' && (utc || h >= 0) && nn > 0) {
            int sign = c == '-' ? -1 : 1;
            i++;
            int v = 0, k = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9' && k < 4)
                v = v * 10 + (s[i++] - '0'), k++;
            if (i < n && s[i] == ':') {
                i++;
                int m2 = 0;
                while (i < n && s[i] >= '0' && s[i] <= '9')
                    m2 = m2 * 10 + (s[i++] - '0');
                v = v * 100 + m2;
            } else if (k <= 2)
                v *= 100;
            offset = sign * ((v / 100) * 60 + v % 100) * 60000.0;
            utc = true;
            continue;
        }
        if (c >= '0' && c <= '9') {
            double v = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9')
                v = v * 10 + (s[i++] - '0');
            if (i < n && s[i] == ':') {
                h = (int)v;
                i++;
                mi = 0;
                while (i < n && s[i] >= '0' && s[i] <= '9')
                    mi = mi * 10 + (s[i++] - '0');
                if (i < n && s[i] == ':') {
                    i++;
                    se = 0;
                    while (i < n && s[i] >= '0' && s[i] <= '9')
                        se = se * 10 + (s[i++] - '0');
                    if (i < n && s[i] == '.') {
                        i++;
                        int k = 0;
                        ms = 0;
                        while (i < n && s[i] >= '0' && s[i] <= '9') {
                            if (k < 3)
                                ms = ms * 10 + (s[i] - '0');
                            k++;
                            i++;
                        }
                        while (k++ < 3)
                            ms *= 10;
                    }
                }
                continue;
            }
            if (i < n && (s[i] == '/' || s[i] == '-') && nn == 0)
                slash_form = 1;
            if (nn < 3)
                nums[nn++] = v;
            else
                return NAN;
            continue;
        }
        i++;
    }
    double year, day;
    if (month >= 0) {
        if (nn < 2)
            return NAN;
        if (nums[0] > 31) {
            year = nums[0];
            day = nums[1];
        } else {
            day = nums[0];
            year = nums[1];
        }
    } else if (nn == 3) {
        if (nums[0] > 31) {                     /* y/m/d */
            year = nums[0];
            month = (int)nums[1] - 1;
            day = nums[2];
        } else {                                /* m/d/y */
            month = (int)nums[0] - 1;
            day = nums[1];
            year = nums[2];
        }
        (void)slash_form;
    } else
        return NAN;
    if (year < 50)
        year += 2000;
    else if (year < 100)
        year += 1900;
    if (month < 0 || month > 11 || day < 1 || day > 31)
        return NAN;
    if (h < 0)
        h = 0;
    if (pm && h < 12)
        h += 12;
    if (am && h == 12)
        h = 0;
    if (h > 24 || mi > 59 || se > 59)
        return NAN;
    double t = make_date(make_day(year, month, day), make_time(h, mi, se, ms));
    if (utc)
        return t - offset;
    return utc_of_local(t);
}

static double parse_date(struct js *J, struct jsstring *str)
{
    struct jsstring *f = str_flat(J, str);
    uint32_t n = str_len(f), a = 0;
    while (a < n && js_is_space(f->chars[a]))
        a++;
    while (n > a && js_is_space(f->chars[n - 1]))
        n--;
    bool ok;
    double t = parse_iso(f->chars + a, n - a, &ok);
    if (ok)
        return time_clip(t);
    return time_clip(parse_legacy(f->chars + a, n - a));
}

/* --- formatting ----------------------------------------------------------- */

static const char *const day_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const month_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int week_day(double t)
{
    int w = (int)fmod(day_of(t) + 4, 7);
    return w < 0 ? w + 7 : w;
}

static void fmt_year(char *buf, size_t n, double y)
{
    if (y < 0)
        snprintf(buf, n, "-%06.0f", -y);
    else
        snprintf(buf, n, "%04.0f", y);
}

static void fmt_date(char *out, size_t n, double lt)
{
    double y;
    int m, d;
    split_date(lt, &y, &m, &d);
    char ys[16];
    fmt_year(ys, sizeof ys, y);
    snprintf(out, n, "%s %s %02d %s", day_names[week_day(lt)], month_names[m], d, ys);
}

static void fmt_time(char *out, size_t n, double t, bool zone)
{
    double lt = local_time(t), w = time_within_day(lt);
    int h = (int)(w / 3600000), mi = (int)fmod(w / 60000, 60), s = (int)fmod(w / 1000, 60);
    if (!zone) {
        snprintf(out, n, "%02d:%02d:%02d", h, mi, s);
        return;
    }
    int off = (int)(offset_at(t) / 60000);
    char sign = off < 0 ? '-' : '+';
    if (off < 0)
        off = -off;
    char zname[64] = "";
    double secs = floor(t / 1000);
    if (secs > -67768040609740800.0 && secs < 67768036191676799.0) {
        time_t tt = (time_t)secs;
        struct tm *ltm = localtime(&tt);
        if (ltm)
            strftime(zname, sizeof zname, "%Z", ltm);
    }
    if (zname[0])
        snprintf(out, n, "%02d:%02d:%02d GMT%c%02d%02d (%s)", h, mi, s, sign, off / 60, off % 60, zname);
    else
        snprintf(out, n, "%02d:%02d:%02d GMT%c%02d%02d", h, mi, s, sign, off / 60, off % 60);
}

struct jsstring *date_to_string(struct js *J, double t)
{
    if (t != t)
        return str_from_ascii(J, "Invalid Date");
    char d[64], tm[128], out[200];
    fmt_date(d, sizeof d, local_time(t));
    fmt_time(tm, sizeof tm, t, true);
    snprintf(out, sizeof out, "%s %s", d, tm);
    return str_from_ascii(J, out);
}

static jsval d_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val;
    if (nt == JS_UNDEFINED)
        return str_val(date_to_string(J, now()));
    double tv;
    if (argc == 0)
        tv = now();
    else if (argc == 1) {
        jsval v = argv[0];
        if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_DATE)
            tv = js_number_value(to_obj(v)->u.prim);
        else {
            v = js_to_primitive(J, v, 0);
            if (v == JS_EXCEPTION)
                return v;
            if (js_is_string(v))
                tv = parse_date(J, to_str(v));
            else {
                bool threw = false;
                tv = time_clip(js_to_number(J, v, &threw));
                if (threw)
                    return JS_EXCEPTION;
            }
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < argc && i < 7; i++) {
            bool threw = false;
            f[i] = js_to_number(J, argv[i], &threw);
            if (threw)
                return JS_EXCEPTION;
        }
        if (f[0] == f[0]) {
            double yi = trunc(f[0]);
            if (yi >= 0 && yi <= 99)
                f[0] = 1900 + yi;
        }
        tv = time_clip(utc_of_local(make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
    }
    struct jsobject *proto = js_proto_from_ctor(J, nt, J->proto[P_DATE]);
    if (!proto)
        return JS_EXCEPTION;
    struct jsobject *o = obj_new(J, proto, CLASS_DATE);
    o->u.prim = js_number(tv);
    return obj_val(o);
}

static jsval d_now(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)J, (void)t, (void)c, (void)a, (void)n;
    return js_number(now());
}

static jsval d_parse(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)t, (void)c, (void)n;
    struct jsstring *s = js_to_str(J, a[0]);
    if (!s)
        return JS_EXCEPTION;
    return js_number(parse_date(J, s));
}

static jsval d_utc(struct js *J, jsval t, int argc, jsval *argv, jsval n)
{
    (void)t, (void)n;
    double f[7] = { NAN, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < argc && i < 7; i++) {
        bool threw = false;
        f[i] = js_to_number(J, argv[i], &threw);
        if (threw)
            return JS_EXCEPTION;
    }
    if (f[0] == f[0]) {
        double yi = trunc(f[0]);
        if (yi >= 0 && yi <= 99)
            f[0] = 1900 + yi;
    }
    return js_number(time_clip(make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
}

/* getters: magic picks the field; 0x100 = UTC */
enum { G_YEAR, G_MONTH, G_DATE, G_DAY, G_HOURS, G_MINUTES, G_SECONDS, G_MS, G_TIME, G_TZ, G_YEAR2 };

static jsval get_field(struct js *J, jsval this_val, int field, bool utc)
{
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    if (t != t)
        return js_number(NAN);
    if (field == G_TIME)
        return js_number(t);
    if (field == G_TZ)
        return js_number((t - local_time(t)) / 60000);
    double lt = utc ? t : local_time(t);
    double y;
    int m, d;
    split_date(lt, &y, &m, &d);
    double w = time_within_day(lt);
    switch (field) {
    case G_YEAR: return js_number(y);
    case G_YEAR2: return js_number(y - 1900);
    case G_MONTH: return js_number(m);
    case G_DATE: return js_number(d);
    case G_DAY: return js_number(week_day(lt));
    case G_HOURS: return js_number(floor(w / 3600000));
    case G_MINUTES: return js_number(fmod(floor(w / 60000), 60));
    case G_SECONDS: return js_number(fmod(floor(w / 1000), 60));
    default: return js_number(fmod(w, 1000));
    }
}

#define GETTER(name, field, utc) \
    static jsval name(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return get_field(J, t, field, utc); }
GETTER(dp_get_full_year, G_YEAR, false) GETTER(dp_get_utc_full_year, G_YEAR, true)
GETTER(dp_get_month, G_MONTH, false) GETTER(dp_get_utc_month, G_MONTH, true)
GETTER(dp_get_date, G_DATE, false) GETTER(dp_get_utc_date, G_DATE, true)
GETTER(dp_get_day, G_DAY, false) GETTER(dp_get_utc_day, G_DAY, true)
GETTER(dp_get_hours, G_HOURS, false) GETTER(dp_get_utc_hours, G_HOURS, true)
GETTER(dp_get_minutes, G_MINUTES, false) GETTER(dp_get_utc_minutes, G_MINUTES, true)
GETTER(dp_get_seconds, G_SECONDS, false) GETTER(dp_get_utc_seconds, G_SECONDS, true)
GETTER(dp_get_milliseconds, G_MS, false) GETTER(dp_get_utc_milliseconds, G_MS, true)
GETTER(dp_get_time, G_TIME, true) GETTER(dp_get_timezone_offset, G_TZ, false)
GETTER(dp_get_year, G_YEAR2, false)

/* setters: which fields they take (first, count) */
static jsval set_fields(struct js *J, jsval this_val, int argc, jsval *argv, int first, int count, bool utc)
{
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    double vals[4];
    int given = argc < count ? (argc ? argc : 1) : count;
    for (int i = 0; i < given; i++) {
        vals[i] = js_to_number(J, i < argc ? argv[i] : JS_UNDEFINED, &threw);
        if (threw)
            return JS_EXCEPTION;
    }
    if (t != t && first != 0)
        return js_number(NAN);
    double lt = t != t ? 0 : (utc ? t : local_time(t));
    double y;
    int m, d;
    split_date(lt, &y, &m, &d);
    double w = time_within_day(lt);
    double f[7] = { y, m, d, floor(w / 3600000), fmod(floor(w / 60000), 60), fmod(floor(w / 1000), 60), fmod(w, 1000) };
    if (t != t && first == 0)
        f[1] = 0, f[2] = 1, f[3] = f[4] = f[5] = f[6] = 0;
    for (int i = 0; i < given; i++)
        f[first + i] = vals[i];
    double nt = make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]));
    nt = time_clip(utc ? nt : utc_of_local(nt));
    to_obj(this_val)->u.prim = js_number(nt);
    return js_number(nt);
}

#define SETTER(name, first, count, utc) \
    static jsval name(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return set_fields(J, t, c, a, first, count, utc); }
SETTER(dp_set_full_year, 0, 3, false) SETTER(dp_set_utc_full_year, 0, 3, true)
SETTER(dp_set_month, 1, 2, false) SETTER(dp_set_utc_month, 1, 2, true)
SETTER(dp_set_date, 2, 1, false) SETTER(dp_set_utc_date, 2, 1, true)
SETTER(dp_set_hours, 3, 4, false) SETTER(dp_set_utc_hours, 3, 4, true)
SETTER(dp_set_minutes, 4, 3, false) SETTER(dp_set_utc_minutes, 4, 3, true)
SETTER(dp_set_seconds, 5, 2, false) SETTER(dp_set_utc_seconds, 5, 2, true)
SETTER(dp_set_milliseconds, 6, 1, false) SETTER(dp_set_utc_milliseconds, 6, 1, true)

static jsval dp_set_time(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    bool threw = false;
    this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    double t = time_clip(js_to_number(J, argv[0], &threw));
    if (threw)
        return JS_EXCEPTION;
    to_obj(this_val)->u.prim = js_number(t);
    return js_number(t);
}

static jsval dp_set_year(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    bool threw = false;
    double y = js_to_number(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (y == y && trunc(y) >= 0 && trunc(y) <= 99)
        y = 1900 + trunc(y);
    jsval v = js_number(y);
    return set_fields(J, this_val, 1, &v, 0, 1, false);
}

static jsval dp_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    return str_val(date_to_string(J, t));
}

static jsval dp_to_date_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    if (t != t)
        return str_val(str_from_ascii(J, "Invalid Date"));
    char d[64];
    fmt_date(d, sizeof d, local_time(t));
    return str_val(str_from_ascii(J, d));
}

static jsval dp_to_time_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    if (t != t)
        return str_val(str_from_ascii(J, "Invalid Date"));
    char tm[128];
    fmt_time(tm, sizeof tm, t, true);
    return str_val(str_from_ascii(J, tm));
}

static jsval dp_to_utc_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    if (t != t)
        return str_val(str_from_ascii(J, "Invalid Date"));
    double y;
    int m, d;
    split_date(t, &y, &m, &d);
    double w = time_within_day(t);
    char ys[16], out[100];
    fmt_year(ys, sizeof ys, y);
    snprintf(out, sizeof out, "%s, %02d %s %s %02d:%02d:%02d GMT", day_names[week_day(t)], d, month_names[m], ys,
             (int)(w / 3600000), (int)fmod(w / 60000, 60), (int)fmod(w / 1000, 60));
    return str_val(str_from_ascii(J, out));
}

static jsval dp_to_iso_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    if (t != t)
        return throw_range(J, "Invalid Date");
    double y;
    int m, d;
    split_date(t, &y, &m, &d);
    double w = time_within_day(t);
    char ys[16], out[64];
    if (y < 0 || y > 9999)
        snprintf(ys, sizeof ys, "%c%06.0f", y < 0 ? '-' : '+', fabs(y));
    else
        snprintf(ys, sizeof ys, "%04.0f", y);
    snprintf(out, sizeof out, "%s-%02d-%02dT%02d:%02d:%02d.%03dZ", ys, m + 1, d, (int)(w / 3600000),
             (int)fmod(w / 60000, 60), (int)fmod(w / 1000, 60), (int)fmod(w, 1000));
    return str_val(str_from_ascii(J, out));
}

static jsval dp_to_json(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = js_to_object(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    jsval tv = js_to_primitive(J, obj_val(o), 1);
    if (tv == JS_EXCEPTION)
        return tv;
    if (js_is_number(tv) && !isfinite(js_number_value(tv)))
        return JS_NULL;
    return js_call_method(J, obj_val(o), "toISOString", 0, 0);
}

/* (27s) Intl.DateTimeFormat's: 10/26/2026, 7:47:00 AM */
static jsval locale_format(struct js *J, jsval this_val, jsval *argv, const char *required, const char *defaults)
{
    bool threw = false;
    double t = this_time(J, this_val, &threw);
    if (threw)
        return JS_EXCEPTION;
    return intl_date_to_locale(J, t, argv[0], argv[1], required, defaults);
}

static jsval dp_to_locale_string(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return locale_format(J, t, a, "any", "all"); }
static jsval dp_to_locale_date_string(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return locale_format(J, t, a, "date", "date"); }
static jsval dp_to_locale_time_string(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return locale_format(J, t, a, "time", "time"); }

static jsval dp_to_primitive(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "Date.prototype[Symbol.toPrimitive] called on a non-object");
    struct jsstring *hint = js_is_string(argv[0]) ? to_str(argv[0]) : 0;
    int order;
    if (hint && (str_is_ascii_cstr(J, hint, "string") || str_is_ascii_cstr(J, hint, "default")))
        order = 2;
    else if (hint && str_is_ascii_cstr(J, hint, "number"))
        order = 1;
    else
        return throw_type(J, "Invalid hint");
    const char *names[2] = { order == 2 ? "toString" : "valueOf", order == 2 ? "valueOf" : "toString" };
    for (int i = 0; i < 2; i++) {
        jsval m = js_get_cstr(J, this_val, names[i]);
        if (m == JS_EXCEPTION)
            return m;
        if (js_is_callable(m)) {
            jsval r = vm_call(J, m, this_val, 0, 0, JS_UNDEFINED);
            if (r == JS_EXCEPTION || !js_is_object(r))
                return r;
        }
    }
    return throw_type(J, "No default value");
}

void init_date(struct js *J)
{
    struct jsobject *p = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_DATE] = p;
    struct jsobject *c = make_ctor(J, "Date", d_ctor, 7, p);
    obj_define_native(J, c, "now", d_now, 0, 0);
    obj_define_native(J, c, "parse", d_parse, 1, 0);
    obj_define_native(J, c, "UTC", d_utc, 7, 0);
    static const struct { const char *name; js_native fn; int len; } methods[] = {
        { "getDate", dp_get_date, 0 }, { "getDay", dp_get_day, 0 }, { "getFullYear", dp_get_full_year, 0 },
        { "getHours", dp_get_hours, 0 }, { "getMilliseconds", dp_get_milliseconds, 0 },
        { "getMinutes", dp_get_minutes, 0 }, { "getMonth", dp_get_month, 0 }, { "getSeconds", dp_get_seconds, 0 },
        { "getTime", dp_get_time, 0 }, { "getTimezoneOffset", dp_get_timezone_offset, 0 },
        { "getUTCDate", dp_get_utc_date, 0 }, { "getUTCDay", dp_get_utc_day, 0 },
        { "getUTCFullYear", dp_get_utc_full_year, 0 }, { "getUTCHours", dp_get_utc_hours, 0 },
        { "getUTCMilliseconds", dp_get_utc_milliseconds, 0 }, { "getUTCMinutes", dp_get_utc_minutes, 0 },
        { "getUTCMonth", dp_get_utc_month, 0 }, { "getUTCSeconds", dp_get_utc_seconds, 0 },
        { "getYear", dp_get_year, 0 }, { "setDate", dp_set_date, 1 }, { "setFullYear", dp_set_full_year, 3 },
        { "setHours", dp_set_hours, 4 }, { "setMilliseconds", dp_set_milliseconds, 1 },
        { "setMinutes", dp_set_minutes, 3 }, { "setMonth", dp_set_month, 2 }, { "setSeconds", dp_set_seconds, 2 },
        { "setTime", dp_set_time, 1 }, { "setUTCDate", dp_set_utc_date, 1 },
        { "setUTCFullYear", dp_set_utc_full_year, 3 }, { "setUTCHours", dp_set_utc_hours, 4 },
        { "setUTCMilliseconds", dp_set_utc_milliseconds, 1 }, { "setUTCMinutes", dp_set_utc_minutes, 3 },
        { "setUTCMonth", dp_set_utc_month, 2 }, { "setUTCSeconds", dp_set_utc_seconds, 2 },
        { "setYear", dp_set_year, 1 }, { "toDateString", dp_to_date_string, 0 },
        { "toISOString", dp_to_iso_string, 0 }, { "toJSON", dp_to_json, 1 },
        { "toLocaleDateString", dp_to_locale_date_string, 0 }, { "toLocaleString", dp_to_locale_string, 0 },
        { "toLocaleTimeString", dp_to_locale_time_string, 0 }, { "toString", dp_to_string, 0 },
        { "toTimeString", dp_to_time_string, 0 }, { "toUTCString", dp_to_utc_string, 0 },
        { "valueOf", dp_get_time, 0 },
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        obj_define_native(J, p, methods[i].name, methods[i].fn, methods[i].len, 0);
    obj_define_value(J, p, key_cstr(J, "toGMTString"), obj_get(J, p, key_cstr(J, "toUTCString"), obj_val(p)),
                     PROP_WRITABLE | PROP_CONFIGURABLE);
    struct jsobject *tp = make_native(J, "[Symbol.toPrimitive]", dp_to_primitive, 1, 0);
    obj_define_value(J, p, J->symbol[SYM_TO_PRIMITIVE], obj_val(tp), PROP_CONFIGURABLE);
}
