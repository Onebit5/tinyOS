#ifndef USER_LINES_H
#define USER_LINES_H

#include "syscall.h"

/* reading input a line at a time.
 *
 * every program that 0.2.8 adds does the same thing: take lines from
 * somewhere, do something to them, put lines out. the somewhere is
 * either a file named on the command line or standard input, and a
 * program should not be able to tell which -- that indifference is what
 * makes a pipeline possible at all.
 *
 * it is a header rather than another .c file because it is small and
 * every program that wants it wants all of it. the buffer lives in the
 * caller's struct, so nothing here is shared and nothing is allocated.
 *
 * the buffering matters more than it looks. a read per character would
 * be a syscall per character, and through a pipe that is a lock, a
 * wake, and possibly a context switch each time -- for one byte. so it
 * takes a chunk and hands out lines from it. */

#define LINES_BUF  1024
#define LINE_CAP   512

struct lines {
    long fd;
    char buf[LINES_BUF];
    long len;               /* how much of buf is real */
    long at;                /* how far through it I am */
    bool done;              /* the fd said end of file */
};

static inline void lines_open(struct lines *l, long fd) {
    l->fd = fd;
    l->len = 0;
    l->at = 0;
    l->done = false;
}

/* the next line, without its newline, NUL-terminated. false at the end
 * of the input.
 *
 * a final line with no newline on the end is still a line -- text files
 * without a trailing newline are common enough that dropping the last
 * line would be a bug people hit immediately */
static inline bool lines_next(struct lines *l, char *out, long cap,
                              long *out_len) {
    long n = 0;

    for (;;) {
        if (l->at >= l->len) {
            if (l->done) {
                break;
            }
            l->len = read_fd(l->fd, l->buf, LINES_BUF);
            l->at = 0;
            if (l->len <= 0) {
                l->len = 0;
                l->done = true;
                break;
            }
        }

        char c = l->buf[l->at++];
        if (c == '\n') {
            out[n < cap - 1 ? n : cap - 1] = '\0';
            *out_len = n;
            return true;
        }
        if (n < cap - 1) {
            out[n++] = c;
        }
        /* a line longer than the buffer is truncated rather than split
         * into two lines, because splitting it would silently turn one
         * line into two and change every count downstream */
    }

    out[n < cap - 1 ? n : cap - 1] = '\0';
    *out_len = n;
    return n > 0;
}

/* write a line back out, newline and all */
static inline void put_line(const char *s) {
    write(s);
    write("\n");
}

#endif
