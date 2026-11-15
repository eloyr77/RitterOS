/*
 * (43a) getopt and getopt_long as GNU's (the order of their checks,
 * their messages): words that are not options are skipped over, and once
 * the options after them have been read the skipped words are moved
 * after those options, so argv ends with the operands (unless options
 * starts with '+', or POSIXLY_CORRECT is set). (46b) optarg, optind,
 * opterr and optopt are the library's own variables (programs reach them
 * through the GOT); the calls that hand out their addresses stay for
 * programs built before, which reached them that way.
 */
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt;
static int place;                       /* the next letter in argv[optind] (0: a new word) */

char **__ritteros_optarg(void) { return &optarg; }
int *__ritteros_optind(void) { return &optind; }
int *__ritteros_opterr(void) { return &opterr; }
int *__ritteros_optopt(void) { return &optopt; }

static void complain(const char *format, const char *argv0, const char *what, int c)
{
    if (!opterr)
        return;
    char line[512];
    int n = snprintf(line, sizeof(line), "%s: ", argv0);
    if (n < (int)sizeof(line))
        n += what ? snprintf(line + n, sizeof(line) - (size_t)n, format, what)
                  : snprintf(line + n, sizeof(line) - (size_t)n, format, c);
    if (n >= (int)sizeof(line))
        n = (int)sizeof(line) - 1;
    line[n++] = '\n';
    write(STDERR_FILENO, line, (size_t)n);
}

static int short_option(int argc, char *const argv[], const char *options)
{
    if (optind >= argc || !argv[optind])
        return -1;
    const char *word = argv[optind];
    if (!place) {
        if (word[0] != '-' || !word[1]) {
            if (options[0] == '-') {
                optarg = argv[optind++];
                return 1;
            }
            return -1;
        }
        if (word[1] == '-' && !word[2]) {
            optind++;
            return -1;
        }
        place = 1;
    }
    int c = (unsigned char)word[place++];
    if (!word[place]) {
        optind++;
        place = 0;
    }
    const char *spec = options;
    if (*spec == '+' || *spec == '-')
        spec++;
    bool colon = *spec == ':';
    const char *at = c == ':' ? 0 : strchr(spec, c);
    if (!at) {
        optopt = c;
        if (!colon)
            complain("invalid option -- '%c'", argv[0], 0, c);
        return '?';
    }
    optarg = 0;
    if (at[1] != ':')
        return c;
    if (at[2] == ':') {                         /* optional: only in the same word */
        if (place) {
            optarg = (char *)word + place;
            optind++;
            place = 0;
        }
        return c;
    }
    if (place) {
        optarg = (char *)word + place;
        optind++;
        place = 0;
    } else if (optind >= argc) {
        optopt = c;
        if (colon)
            return ':';
        complain("option requires an argument -- '%c'", argv[0], 0, c);
        return '?';
    } else {
        optarg = argv[optind++];
    }
    return c;
}

static int long_option(int argc, char *const argv[], const char *options,
                       const struct option *longopts, int *longindex, bool only)
{
    const char *word = argv[optind];
    bool dashes = word[1] == '-';
    const char *name = word + 1 + dashes;
    size_t len = strcspn(name, "=");
    const char *spec = options;
    if (*spec == '+' || *spec == '-')
        spec++;
    bool colon = *spec == ':';
    int match = -1, count = 0;
    for (int i = 0; longopts[i].name; i++) {
        if (strncmp(longopts[i].name, name, len))
            continue;
        if (strlen(longopts[i].name) == len) {  /* exact */
            match = i;
            count = 1;
            break;
        }
        if (match < 0 || longopts[i].has_arg != longopts[match].has_arg ||
            longopts[i].flag != longopts[match].flag || longopts[i].val != longopts[match].val)
            count++;
        if (match < 0)
            match = i;
    }
    if (only && !dashes && count != 1 && len == 1 && strchr(spec, name[0]))
        return short_option(argc, argv, options);  /* (a single letter: getopt's) */
    const char *dash = dashes ? "--" : "-";
    char shown[300];
    if (count > 1) {
        snprintf(shown, sizeof(shown), "%s%.*s", dash, (int)len, name);
        complain("option '%s' is ambiguous", argv[0], shown, 0);
        optopt = 0;
        optind++;
        return '?';
    }
    if (match < 0) {
        snprintf(shown, sizeof(shown), "%s%s", dash, name);
        complain("unrecognized option '%s'", argv[0], shown, 0);
        optopt = 0;
        optind++;
        return '?';
    }
    const struct option *o = &longopts[match];
    snprintf(shown, sizeof(shown), "%s%s", dash, o->name);
    optind++;
    optarg = name[len] == '=' ? (char *)name + len + 1 : 0;
    if (o->has_arg == no_argument && optarg) {
        optopt = o->flag ? 0 : o->val;
        complain("option '%s' doesn't allow an argument", argv[0], shown, 0);
        return '?';
    }
    if (o->has_arg == required_argument && !optarg) {
        if (optind >= argc) {
            optopt = o->flag ? 0 : o->val;
            if (colon)
                return ':';
            complain("option '%s' requires an argument", argv[0], shown, 0);
            return '?';
        }
        optarg = argv[optind++];
    }
    if (longindex)
        *longindex = match;
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

static int one(int argc, char *const argv[], const char *options,
               const struct option *longopts, int *longindex, bool only)
{
    const char *word = argv[optind];
    if (longopts && !place && word[0] == '-' &&
        ((word[1] == '-' && word[2]) || (only && word[1] && word[1] != '-')))
        return long_option(argc, argv, options, longopts, longindex, only);
    return short_option(argc, argv, options);
}

/* argv[from] moved back to dest, the words between one place on */
static void move_back(char *const argv[], int dest, int from)
{
    char **v = (char **)argv;
    char *word = v[from];
    for (int i = from; i > dest; i--)
        v[i] = v[i - 1];
    v[dest] = word;
}

static int next(int argc, char *const argv[], const char *options,
                const struct option *longopts, int *longindex, bool only)
{
    if (optind == 0) {
        optind = 1;
        place = 0;
    }
    if (optind >= argc || !argv[optind])
        return -1;
    bool in_order = options[0] == '+' || options[0] == '-' || getenv("POSIXLY_CORRECT");
    int skipped = optind;
    if (!in_order && !place) {
        int i = optind;
        for (;; i++) {
            if (i >= argc || !argv[i])
                return -1;                      /* (optind stays at the first operand) */
            if (argv[i][0] == '-' && argv[i][1])
                break;
        }
        optind = i;
    }
    int resumed = optind;
    int r = one(argc, argv, options, longopts, longindex, only);
    if (resumed > skipped) {
        /* the words read (and one still being read: "-ab" gave only 'a')
         * go before the skipped ones */
        int count = optind - resumed + (place != 0);
        for (int i = 0; i < count; i++)
            move_back(argv, skipped, resumed + count - 1);
        optind = skipped + count - (place != 0);
    }
    return r;
}

int getopt(int argc, char *const argv[], const char *options)
{
    return next(argc, argv, options, 0, 0, false);
}

int getopt_long(int argc, char *const argv[], const char *options,
                const struct option *longopts, int *longindex)
{
    return next(argc, argv, options, longopts, longindex, false);
}

int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *longopts, int *longindex)
{
    return next(argc, argv, options, longopts, longindex, true);
}
