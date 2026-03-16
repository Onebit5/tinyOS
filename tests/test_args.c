/* host-side test for the argument parser.
 *
 * this is the one piece of the user library with enough rules to get
 * wrong, and the failures are quiet ones: a flag silently treated as a
 * filename, a value swallowed from the wrong place, `--` not stopping
 * what it is supposed to stop. none of those announce themselves. */
#include <stdio.h>
#include <string.h>

#include "../user/args.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static const struct opt opts[] = {
    { 'v', "verbose", false, "say more about it" },
    { 'a', "all",     false, "everything, not just the interesting parts" },
    { 'o', "out",     true,  "where to put it" },
    { 0,   "colour",  false, "a long one with no short spelling" },
};

static const struct program prog = {
    .name = "thing",
    .usage = "thing [options] <file>...",
    .summary = "does a thing",
    .opts = opts,
    .opt_count = sizeof opts / sizeof opts[0],
};

/* the parser takes argv as a program gets it, so argv[0] is the name */
static struct args parse(const char **error, int argc, ...) {
    static char *words[16];
    __builtin_va_list ap;
    __builtin_va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        words[i] = __builtin_va_arg(ap, char *);
    }
    __builtin_va_end(ap);

    static struct args a;
    if (!args_parse(&prog, argc, words, &a, error)) {
        a.count = -1;
    }
    return a;
}

int main(void) {
    const char *error;
    struct args a;

    /* ---- nothing at all ---- */

    a = parse(&error, 1, "thing");
    CHECK(a.count == 0, "no arguments is no arguments");
    CHECK(!args_has(&a, &prog, 'v'), "and no options either");

    /* ---- the short forms ---- */

    a = parse(&error, 3, "thing", "-v", "file.txt");
    CHECK(args_has(&a, &prog, 'v'), "a short option is seen");
    CHECK(a.count == 1 && strcmp(a.rest[0], "file.txt") == 0,
          "and the file is still a file");

    a = parse(&error, 3, "thing", "file.txt", "-v");
    CHECK(args_has(&a, &prog, 'v') && a.count == 1,
          "an option after the arguments counts too");

    a = parse(&error, 2, "thing", "-va");
    CHECK(args_has(&a, &prog, 'v') && args_has(&a, &prog, 'a'),
          "several short options stuck together are all of them");

    /* ---- the long ones ---- */

    a = parse(&error, 2, "thing", "--verbose");
    CHECK(args_has(&a, &prog, 'v'), "the long spelling is the same option");

    a = parse(&error, 2, "thing", "--colour");
    CHECK(args_has_long(&a, &prog, "colour"),
          "and one with no short form still works");

    /* ---- values, from wherever they are put ---- */

    a = parse(&error, 3, "thing", "-o", "out.txt");
    CHECK(strcmp(args_value(&a, &prog, 'o'), "out.txt") == 0,
          "a value can be the next word");
    CHECK(a.count == 0, "and is not left lying around as an argument");

    a = parse(&error, 2, "thing", "-oout.txt");
    CHECK(strcmp(args_value(&a, &prog, 'o'), "out.txt") == 0,
          "or stuck to the option");

    a = parse(&error, 2, "thing", "--out=out.txt");
    CHECK(strcmp(args_value(&a, &prog, 'o'), "out.txt") == 0,
          "or after an equals");

    a = parse(&error, 3, "thing", "--out", "out.txt");
    CHECK(strcmp(args_value(&a, &prog, 'o'), "out.txt") == 0,
          "or as the next word after a long one");

    a = parse(&error, 2, "thing", "-vo");
    CHECK(a.count == -1, "an option wanting a value with none is refused");

    /* ---- the dashes that are not options ---- */

    a = parse(&error, 2, "thing", "-");
    CHECK(a.count == 1 && strcmp(a.rest[0], "-") == 0,
          "a bare dash is a filename, as it has been for fifty years");

    a = parse(&error, 4, "thing", "--", "-v", "-x");
    CHECK(a.count == 2, "everything after -- is an argument");
    CHECK(!args_has(&a, &prog, 'v'), "even something that looks like an option");
    CHECK(strcmp(a.rest[0], "-v") == 0 && strcmp(a.rest[1], "-x") == 0,
          "and arrives unchanged");

    /* that matters for a real reason: it is the only way to name a file
     * whose name begins with a dash */
    a = parse(&error, 3, "thing", "--", "-weird-name");
    CHECK(a.count == 1 && strcmp(a.rest[0], "-weird-name") == 0,
          "which is how a file called -weird-name can be opened at all");

    /* ---- things that are not options at all ---- */

    a = parse(&error, 2, "thing", "-z");
    CHECK(a.count == -1, "an option nobody declared is refused");
    CHECK(error != NULL, "with something to say about it");

    a = parse(&error, 2, "thing", "--nonsense");
    CHECK(a.count == -1, "and so is a long one");

    a = parse(&error, 2, "thing", "--verbose=yes");
    CHECK(a.count == -1, "a value given to an option that takes none is refused");

    /* ---- help, which the parser notes and does not act on ---- */

    a = parse(&error, 2, "thing", "--help");
    CHECK(a.wants_help, "--help is noticed");
    CHECK(a.count == 0, "and is not mistaken for a filename");

    a = parse(&error, 2, "thing", "-h");
    CHECK(a.wants_help, "and so is -h");

    a = parse(&error, 3, "thing", "-vh", "x");
    CHECK(a.wants_help && args_has(&a, &prog, 'v'),
          "even bundled up with something else");

    /* ---- the declaration is the description ---- */

    CHECK(prog.opt_count == 4, "the program says how many options it has");
    CHECK(strcmp(prog.opts[0].help, "say more about it") == 0,
          "and what each one is for, once, where the parser can see it too");

    if (failures == 0) printf("all good\n");
    return failures;
}
