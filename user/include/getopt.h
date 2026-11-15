/*
 * getopt.h (43a): GNU's long options beside unistd.h's getopt.
 * getopt_long: options as getopt's, and "--name", "--name=value" or
 * "--name value" from longopts (ended by an all-zero entry; a unique
 * prefix of a name is taken): has_arg no_argument, required_argument or
 * optional_argument (only "=value"); flag 0: the result is val, else
 * *flag = val and the result 0; *longindex (if not 0) the entry's index.
 * getopt_long_only also takes long options after a single '-'.
 */
#ifndef RITTEROS_GETOPT_H
#define RITTEROS_GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <unistd.h>

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};
#define no_argument             0
#define required_argument       1
#define optional_argument       2

int getopt_long(int argc, char *const argv[], const char *options,
                const struct option *longopts, int *longindex);
int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif
