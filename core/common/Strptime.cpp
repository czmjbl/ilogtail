/*-
 * Copyright (c) 1997, 1998, 2005, 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code was contributed to The NetBSD Foundation by Klaus Klein.
 * Heavily optimised by David Laight
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "Strptime.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "common/StringTools.h"

namespace logtail {
/*
 * We do not implement alternate representations. However, we always
 * check whether a given modifier is allowed for a certain conversion.
 */
#define ALT_E 0x01
#define ALT_O 0x02
#define LEGAL_ALT(x) \
    { \
        if (alt_format & ~(x)) \
            return NULL; \
    }

static char gmt[] = {"GMT"};
static char utc[] = {"UTC"};

/* RFC-822/RFC-2822 */
static const char* const nast[5] = {"EST", "CST", "MST", "PST", "\0\0\0"};
static const char* const nadt[5] = {"EDT", "CDT", "MDT", "PDT", "\0\0\0"};

#define TM_YEAR_BASE 1900

static const char* day[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* abday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* mon[12] = {"January",
                              "February",
                              "March",
                              "April",
                              "May",
                              "June",
                              "July",
                              "August",
                              "September",
                              "October",
                              "November",
                              "December"};
static const char* abmon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char* am_pm[2] = {"AM", "PM"};

static const unsigned char* conv_num(const unsigned char*, int*, unsigned int, unsigned int);
static const unsigned char* conv_nanosecond(const unsigned char*, long*, int*);
static const unsigned char* find_string(const unsigned char*, int*, const char* const*, const char* const*, int);

// Parse time string into two part: second and nanosecond
// Modify from https://github.com/tnodir/luasys/blob/master/src/win32/strptime.c
const char* strptime_ns(const char* buf, const char* fmt, struct tm* tm, long* nanosecond, int* nanosecondLength) {
    // Replenish %s support.
    if (0 == strcmp("%s", fmt)) {
        char* cp;
        long long n;
        n = strtoll(buf, &cp, 10);
        // Assert the length of second timestamp to be less than 10.
        // Input buf may contains other contents, so get bufLength from n.
        size_t bufLength = std::to_string(n).length();
        size_t secondTimestampLength = bufLength >= 10 ? 10 : bufLength;
        for (size_t i = 0; i < bufLength - secondTimestampLength; ++i) {
            n /= 10;
        }
        time_t t;
        if (n == 0 || (long long)(t = n) != n) {
            return NULL;
        }
#ifdef _MSC_VER
        if (localtime_s(tm, &t) != 0)
            return NULL;
#else
        if (NULL == localtime_r(&t, tm))
            return NULL;
#endif

        *nanosecond = 0;
        *nanosecondLength = 0;
        conv_nanosecond((const unsigned char*)(buf + secondTimestampLength), nanosecond, nanosecondLength);
        return ((const char*)cp);
    }

    unsigned char c;
    const unsigned char *bp, *ep;
    int alt_format, i, split_year = 0, neg = 0, offs;
    const char* new_fmt;
    *nanosecond = 0;

    bp = (const unsigned char*)buf;

    while (bp != NULL && (c = *fmt++) != '\0') {
        /* Clear 'alternate' modifier prior to new conversion. */
        alt_format = 0;
        i = 0;

        /* Eat up white-space. */
        if (isspace(c)) {
            while (isspace(*bp))
                bp++;
            continue;
        }

        if (c != '%')
            goto literal;


    again:
        switch (c = *fmt++) {
            case '%': /* "%%" is converted to "%". */
            literal:
                if (c != *bp++)
                    return NULL;
                LEGAL_ALT(0);
                continue;

                /*
                 * "Alternative" modifiers. Just set the appropriate flag
                 * and start over again.
                 */
            case 'E': /* "%E?" alternative conversion modifier. */
                LEGAL_ALT(0);
                alt_format |= ALT_E;
                goto again;

            case 'O': /* "%O?" alternative conversion modifier. */
                LEGAL_ALT(0);
                alt_format |= ALT_O;
                goto again;

                /*
                 * "Complex" conversion rules, implemented through recursion.
                 */
            case 'c': /* Date and time, using the locale's format. */
                // Tue Nov 20 14:12:58 2020
                new_fmt = "%a %b %d %H:%M:%S %Y";
                goto recurse;

            case 'D': /* The date as "%m/%d/%y". */
                new_fmt = "%m/%d/%y";
                LEGAL_ALT(0);
                goto recurse;

            case 'F': /* The date as "%Y-%m-%d". */
                new_fmt = "%Y-%m-%d";
                LEGAL_ALT(0);
                goto recurse;

            case 'R': /* The time as "%H:%M". */
                new_fmt = "%H:%M";
                LEGAL_ALT(0);
                goto recurse;

            case 'r': /* The time in 12-hour clock representation. */
                new_fmt = "%I:%M:%S %p";
                LEGAL_ALT(0);
                goto recurse;

            case 'T': /* The time as "%H:%M:%S". */
                new_fmt = "%H:%M:%S";
                LEGAL_ALT(0);
                goto recurse;

            case 'X': /* The time, using the locale's format. */
                new_fmt = "%H:%M:%S";
                goto recurse;

            case 'x': /* The date, using the locale's format. */
                new_fmt = "%m/%d/%y";
            recurse:
                bp = (const unsigned char*)strptime_ns((const char*)bp, new_fmt, tm, nanosecond, nanosecondLength);
                LEGAL_ALT(ALT_E);
                continue;

                /*
                 * "Elementary" conversion rules.
                 */
            case 'A': /* The day of week, using the locale's form. */
            case 'a':
                bp = find_string(bp, &tm->tm_wday, day, abday, 7);
                LEGAL_ALT(0);
                continue;

            case 'B': /* The month, using the locale's form. */
            case 'b':
            case 'h':
                bp = find_string(bp, &tm->tm_mon, mon, abmon, 12);
                LEGAL_ALT(0);
                continue;

            case 'C': /* The century number. */
                i = 20;
                bp = conv_num(bp, &i, 0, 99);

                i = i * 100 - TM_YEAR_BASE;
                if (split_year)
                    i += tm->tm_year % 100;
                split_year = 1;
                tm->tm_year = i;
                LEGAL_ALT(ALT_E);
                continue;

            case 'd': /* The day of month. */
            case 'e':
                bp = conv_num(bp, &tm->tm_mday, 1, 31);
                LEGAL_ALT(ALT_O);
                continue;

            case 'f': /* Nanosecond */
                bp = conv_nanosecond(bp, nanosecond, nanosecondLength);
                LEGAL_ALT(ALT_O);
                continue;

            case 'k': /* The hour (24-hour clock representation). */
                LEGAL_ALT(0);
                /* FALLTHROUGH */
            case 'H':
                bp = conv_num(bp, &tm->tm_hour, 0, 23);
                LEGAL_ALT(ALT_O);
                continue;

            case 'l': /* The hour (12-hour clock representation). */
                LEGAL_ALT(0);
                /* FALLTHROUGH */
            case 'I':
                bp = conv_num(bp, &tm->tm_hour, 1, 12);
                if (tm->tm_hour == 12)
                    tm->tm_hour = 0;
                LEGAL_ALT(ALT_O);
                continue;

            case 'j': /* The day of year. */
                i = 1;
                bp = conv_num(bp, &i, 1, 366);
                tm->tm_yday = i - 1;
                LEGAL_ALT(0);
                continue;

            case 'M': /* The minute. */
                bp = conv_num(bp, &tm->tm_min, 0, 59);
                LEGAL_ALT(ALT_O);
                continue;

            case 'm': /* The month. */
                i = 1;
                bp = conv_num(bp, &i, 1, 12);
                tm->tm_mon = i - 1;
                LEGAL_ALT(ALT_O);
                continue;

            case 'p': /* The locale's equivalent of AM/PM. */
                bp = find_string(bp, &i, am_pm, NULL, 2);
                if (tm->tm_hour > 11)
                    return NULL;
                tm->tm_hour += i * 12;
                LEGAL_ALT(0);
                continue;

            case 'S': /* The seconds. */
                bp = conv_num(bp, &tm->tm_sec, 0, 61);
                LEGAL_ALT(ALT_O);
                continue;

            case 'U': /* The week of year, beginning on sunday. */
            case 'W': /* The week of year, beginning on monday. */
                /*
                 * XXX This is bogus, as we can not assume any valid
                 * information present in the tm structure at this
                 * point to calculate a real value, so just check the
                 * range for now.
                 */
                bp = conv_num(bp, &i, 0, 53);
                LEGAL_ALT(ALT_O);
                continue;

            case 'w': /* The day of week, beginning on sunday. */
                bp = conv_num(bp, &tm->tm_wday, 0, 6);
                LEGAL_ALT(ALT_O);
                continue;

            case 'u': /* The day of week, monday = 1. */
                bp = conv_num(bp, &i, 1, 7);
                tm->tm_wday = i % 7;
                LEGAL_ALT(ALT_O);
                continue;

            case 'g':
                /* The year corresponding to the ISO week
                 * number but without the century.
                 */
                bp = conv_num(bp, &i, 0, 99);
                continue;

            case 'G':
                /* The year corresponding to the ISO week
                 * number with century.
                 */
                do
                    bp++;
                while (isdigit(*bp));
                continue;

            case 'V': /* The ISO 8601:1988 week number as decimal */
                bp = conv_num(bp, &i, 0, 53);
                continue;

            case 'Y': /* The year. */
                i = TM_YEAR_BASE; /* just for data sanity... */
                bp = conv_num(bp, &i, 0, 9999);
                tm->tm_year = i - TM_YEAR_BASE;
                LEGAL_ALT(ALT_E);
                continue;

            case 'y': /* The year within 100 years of the epoch. */
                /* LEGAL_ALT(ALT_E | ALT_O); */
                bp = conv_num(bp, &i, 0, 99);

                if (split_year)
                    /* preserve century */
                    i += (tm->tm_year / 100) * 100;
                else {
                    split_year = 1;
                    if (i <= 68)
                        i = i + 2000 - TM_YEAR_BASE;
                    else
                        i = i + 1900 - TM_YEAR_BASE;
                }
                tm->tm_year = i;
                continue;

            case 'Z':
                if (CStringNCaseInsensitiveCmp((const char*)bp, gmt, 3) == 0
                    || CStringNCaseInsensitiveCmp((const char*)bp, utc, 3) == 0) {
                    tm->tm_isdst = 0;
#ifdef TM_GMTOFF
                    tm->TM_GMTOFF = 0;
#endif
#ifdef TM_ZONE
                    tm->TM_ZONE = gmt;
#endif
                    bp += 3;
                } else {
#if defined(_TM_DEFINED) && !defined(_WIN32_WCE)
                    _tzset();
                    ep = find_string(bp, &i, (const char* const*)tzname, NULL, 2);
                    if (ep != NULL) {
                        tm->tm_isdst = i;
#ifdef TM_GMTOFF
                        tm->TM_GMTOFF = -(timezone);
#endif
#ifdef TM_ZONE
                        tm->TM_ZONE = tzname[i];
#endif
                    }
                    bp = ep;
#endif
                }
                continue;

            case 'z':
                /*
                 * We recognize all ISO 8601 formats:
                 * Z = Zulu time/UTC
                 * [+-]hhmm
                 * [+-]hh:mm
                 * [+-]hh
                 * We recognize all RFC-822/RFC-2822 formats:
                 * UT|GMT
                 *    North American : UTC offsets
                 * E[DS]T = Eastern : -4 | -5
                 * C[DS]T = Central : -5 | -6
                 * M[DS]T = Mountain: -6 | -7
                 * P[DS]T = Pacific : -7 | -8
                 *    Military
                 * [A-IL-M] = -1 ... -9 (J not used)
                 * [N-Y]  = +1 ... +12
                 */
                while (isspace(*bp))
                    bp++;

                switch (*bp++) {
                    case 'G':
                        if (*bp++ != 'M')
                            return NULL;
                        /*FALLTHROUGH*/
                    case 'U':
                        if (*bp++ != 'T')
                            return NULL;
                        /*FALLTHROUGH*/
                    case 'Z':
                        tm->tm_isdst = 0;
#ifdef TM_GMTOFF
                        tm->TM_GMTOFF = 0;
#endif
#ifdef TM_ZONE
                        tm->TM_ZONE = utc;
#endif
                        continue;
                    case '+':
                        neg = 0;
                        break;
                    case '-':
                        neg = 1;
                        break;
                    default:
                        --bp;
                        ep = find_string(bp, &i, nast, NULL, 4);
                        if (ep != NULL) {
#ifdef TM_GMTOFF
                            tm->TM_GMTOFF = -5 - i;
#endif
#ifdef TM_ZONE
                            tm->TM_ZONE = (char*)(nast[i]);
#endif
                            bp = ep;
                            continue;
                        }
                        ep = find_string(bp, &i, nadt, NULL, 4);
                        if (ep != NULL) {
                            tm->tm_isdst = 1;
#ifdef TM_GMTOFF
                            tm->TM_GMTOFF = -4 - i;
#endif
#ifdef TM_ZONE
                            tm->TM_ZONE = (char*)(nadt[i]);
#endif
                            bp = ep;
                            continue;
                        }

                        if ((*bp >= 'A' && *bp <= 'I') || (*bp >= 'L' && *bp <= 'Y')) {
#ifdef TM_GMTOFF
                            /* Argh! No 'J'! */
                            if (*bp >= 'A' && *bp <= 'I')
                                tm->TM_GMTOFF = ('A' - 1) - (int)*bp;
                            else if (*bp >= 'L' && *bp <= 'M')
                                tm->TM_GMTOFF = 'A' - (int)*bp;
                            else if (*bp >= 'N' && *bp <= 'Y')
                                tm->TM_GMTOFF = (int)*bp - 'M';
#endif
#ifdef TM_ZONE
                            tm->TM_ZONE = NULL; /* XXX */
#endif
                            bp++;
                            continue;
                        }
                        return NULL;
                }
                offs = 0;
                for (i = 0; i < 4;) {
                    if (isdigit(*bp)) {
                        offs = offs * 10 + (*bp++ - '0');
                        i++;
                        continue;
                    }
                    if (i == 2 && *bp == ':') {
                        bp++;
                        continue;
                    }
                    break;
                }
                switch (i) {
                    case 2:
                        offs *= 100;
                        break;
                    case 4:
                        i = offs % 100;
                        if (i >= 60)
                            return NULL;
                        /* Convert minutes into decimal */
                        offs = (offs / 100) * 100 + (i * 50) / 30;
                        break;
                    default:
                        return NULL;
                }
                if (neg)
                    offs = -offs;
                tm->tm_isdst = 0; /* XXX */
#ifdef TM_GMTOFF
                tm->TM_GMTOFF = offs;
#endif
#ifdef TM_ZONE
                tm->TM_ZONE = NULL; /* XXX */
#endif
                continue;

                /*
                 * Miscellaneous conversions.
                 */
            case 'n': /* Any kind of white-space. */
            case 't':
                while (isspace(*bp))
                    bp++;
                LEGAL_ALT(0);
                continue;


            default: /* Unknown/unsupported conversion. */
                return NULL;
        }
    }
    return ((const char*)bp);
}


static const unsigned char* conv_num(const unsigned char* buf, int* dest, unsigned int llim, unsigned int ulim) {
    unsigned int result = 0;
    unsigned char ch;

    /* The limit also determines the number of valid digits. */
    unsigned int rulim = ulim;

    ch = *buf;
    if (ch < '0' || ch > '9')
        return NULL;

    do {
        result *= 10;
        result += ch - '0';
        rulim /= 10;
        ch = *++buf;
    } while ((result * 10 <= ulim) && rulim && ch >= '0' && ch <= '9');

    if (result < llim || result > ulim)
        return NULL;

    *dest = result;
    return buf;
}

static const unsigned char* conv_nanosecond(const unsigned char* buf, long* dest, int* nanosecondLength) {
    unsigned int result = 0;
    unsigned char ch;
    int digitNum = 0;
    const unsigned char* start = buf;

    ch = *buf;
    if (ch < '0' || ch > '9')
        return NULL;

    do {
        result *= 10;
        result += ch - '0';
        ++digitNum;
        ch = *++buf;
    } while (ch >= '0' && ch <= '9');
    for (int i = 0; i < 9 - digitNum; i++) {
        result *= 10;
    }
    *dest = result;
    *nanosecondLength = buf - start;
    return buf;
}

static const unsigned char*
find_string(const unsigned char* bp, int* tgt, const char* const* n1, const char* const* n2, int c) {
    int i;
    unsigned int len;

    /* check full name - then abbreviated ones */
    for (; n1 != NULL; n1 = n2, n2 = NULL) {
        for (i = 0; i < c; i++, n1++) {
            len = strlen(*n1);
            if (CStringNCaseInsensitiveCmp(*n1, (const char*)bp, len) == 0) {
                *tgt = i;
                return bp + len;
            }
        }
    }

    /* Nothing matched */
    return NULL;
}

} // namespace logtail
