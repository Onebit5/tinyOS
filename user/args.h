#ifndef USER_ARGS_H
#define USER_ARGS_H

#include <stddef.h>
#include <stdbool.h>

/* arguments, parsed once and in one place.
 *
 * every program here used to read argv by hand, and no two of them
 * agreed: some took a flag anywhere, some only first, most took none at
 * all and silently treated `-v` as a filename. that is the kind of
 * inconsistency nobody notices until they trust it.
 *
 * so: a program declares what it takes, once, and that declaration is
 * the only description of it there is. the parser reads it, and so does
 * anything that wants to explain the program to somebody -- which is
 * what stops usage text from drifting away from what the code actually
 * does, the usual fate of usage text.
 *
 * what is understood is what everybody expects:
 *
 *   -v            a short option
 *   --verbose     the long spelling of the same one
 *   -abc          three short options at once
 *   -o name       a value, as the next word
 *   -oname        or stuck to it
 *   --out=name    or after an equals
 *   --            everything after this is a filename, even a dash
 *   -             on its own, a filename (it usually means stdin)
 */

#define ARGS_MAX_OPTS 8
#define ARGS_MAX_REST 32

struct opt {
    char        brief;      /* the -x form, or 0 if there is none */
    const char *name;       /* the --long form, or NULL */
    bool        takes_value;
    const char *help;       /* one line, for whoever has to explain it */
};

struct program {
    const char *name;
    const char *usage;      /* the shape of a command line */
    const char *summary;    /* one line about what it is for */
    const struct opt *opts;
    size_t      opt_count;
};

struct args {
    /* one bit per declared option, by its position in the list */
    unsigned    given;
    const char *values[ARGS_MAX_OPTS];

    /* everything that was not an option, in order */
    int         count;
    char       *rest[ARGS_MAX_REST];

    /* --help was asked for. the parser does not act on it, because what
     * to print is 0.2.6's business and a parser should not decide to
     * write things */
    bool        wants_help;
};

/* returns false and sets `error` to something worth printing */
bool args_parse(const struct program *p, int argc, char **argv,
                struct args *out, const char **error);

/* was this option given? by its short letter, or its long name if it
 * has no short one */
bool args_has(const struct args *a, const struct program *p, char brief);
bool args_has_long(const struct args *a, const struct program *p,
                   const char *name);

/* the value it was given, or NULL */
const char *args_value(const struct args *a, const struct program *p,
                       char brief);

#endif
