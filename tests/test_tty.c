/* who the keyboard belongs to.
 *
 * before this there was no answer to that question: the shell peeked at
 * keys while a program ran, so a program could never read one. now the
 * terminal has a foreground process, and ctrl+c aimed at it is an
 * interrupt delivered to it rather than something done on its behalf. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* what the lock complains through */
void panic(const char *fmt, ...) {
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <stdbool.h>

static char out[2048];
static size_t out_len;
static void out_reset(void) { out[0] = 0; out_len = 0; }
void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    out_len += vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
    va_end(ap);
}

/* what the tty does to threads, recorded rather than done */
static int woken_thread = -1;
static int killed_thread = -1;
void sched_wake_thread(int id) { woken_thread = id; }
#include "sched/sched.h"
enum sched_kill_result sched_kill(int id) { killed_thread = id; return SCHED_KILL_OK; }

/* which threads ctrl+z suspended. recorded rather than done, since the
 * question here is *who* it reached rather than what the scheduler does
 * about it */
static int stopped[8];
static int stopped_count;
void sched_set_stopped(int id, bool stop) {
    if (stop && stopped_count < 8) {
        stopped[stopped_count++] = id;
    }
}
bool sched_thread_stopped(int id) {
    for (int i = 0; i < stopped_count; i++) {
        if (stopped[i] == id) return true;
    }
    return false;
}

/* a scripted keyboard: the test says what gets typed, and the line
 * discipline reads it exactly as it would read a person */
static const int *script;
static int script_len, script_at;
int input_getchar_blocking(void) {
    return (script_at < script_len) ? script[script_at++] : '\n';
}

#include "sched/process.h"
#include "drivers/tty.h"

/* a pipe a forked child inherits gains a holder rather than being
 * copied. nothing here forks, so it only has to exist */
struct pipe;
void pipe_share(struct pipe *p, bool writing) { (void)p; (void)writing; }

#include "drivers/input.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void) {
    /* ---- who is at the front ---- */
    CHECK(tty_foreground() == TTY_SHELL,
          "the shell holds the terminal when nothing is running");

    /* the shell wants ctrl+c as an ordinary key: its line editor uses it
     * to abandon a line, so the tty must not swallow it */
    CHECK(!tty_intercept(KEY_CTRL_C),
          "ctrl+c reaches the shell as a key, not as an interrupt");
    CHECK(!tty_intercept('a'), "and so does everything else");
    CHECK(!tty_intercept(KEY_UP), "including keys that are not characters");

    /* ---- a program takes the front ---- */
    int pid = process_create("bin/ask", 0, 0, false, 0);
    process_set_thread(pid, 9);
    tty_set_foreground(pid);
    CHECK(tty_foreground() == pid, "a program can hold the terminal");

    CHECK(!tty_intercept('x'), "ordinary keys still go to the buffer");
    CHECK(!process_interrupt_pending(pid), "and are not interrupts");

    /* ---- ctrl+c is delivered rather than acted upon ---- */
    woken_thread = -1;
    killed_thread = -1;
    CHECK(tty_intercept(KEY_CTRL_C),
          "ctrl+c aimed at a program is taken as an interrupt");
    CHECK(process_interrupt_pending(pid), "and delivered to it");
    CHECK(woken_thread == 9,
          "and it is woken, so a sleeping program finds out now rather "
          "than whenever it next happens to ask for something");
    CHECK(killed_thread == -1, "but it is not killed -- it gets to decide");

    /* asking does not consume it; taking does */
    CHECK(process_interrupt_pending(pid), "asking again still finds it");
    CHECK(process_take_interrupt(pid), "taking it works");
    CHECK(!process_interrupt_pending(pid), "and it is gone afterwards");
    CHECK(!process_take_interrupt(pid), "and cannot be taken twice");

    /* ---- asking twice stops being a request ---- */
    tty_intercept(KEY_CTRL_C);
    CHECK(process_interrupt_pending(pid), "one interrupt is pending");

    killed_thread = -1;
    out_reset();
    CHECK(tty_intercept(KEY_CTRL_C), "a second ctrl+c is also consumed");
    CHECK(killed_thread == 9,
          "but this one kills, because the program had its chance");
    CHECK(strstr(out, "did not take the hint") != NULL, "and says why");

    /* ---- a job is a group, and the terminal talks to all of it ----
     *
     * `cat x | grep y | wc -l` is three processes and one thing the
     * person typing it is thinking about. interrupting only the last of
     * three would leave the other two writing into a pipe nobody reads */
    {
        int a = process_create("cat", 0, 0, false, 0);
        int b = process_create("grep", 0, 0, false, 0);
        int c = process_create("wc", 0, 0, false, 0);
        process_set_thread(a, 21);
        process_set_thread(b, 22);
        process_set_thread(c, 23);
        process_set_pgid(b, a);
        process_set_pgid(c, a);
        tty_set_foreground(a);

        CHECK(process_pgid(a) == a && process_pgid(b) == a,
              "a pipeline is one group, named after the first of them");

        CHECK(tty_intercept(KEY_CTRL_C), "ctrl+c is taken");
        CHECK(process_interrupt_pending(a), "and reaches the first");
        CHECK(process_interrupt_pending(b), "and the middle");
        CHECK(process_interrupt_pending(c),
              "and the last -- all of it, or the survivors write into a "
              "pipe nobody is reading");

        process_take_interrupt(a);
        process_take_interrupt(b);
        process_take_interrupt(c);

        /* ---- ctrl+z ----
         *
         * nothing is asked of the program. every thread in the group is
         * simply marked unpickable, keeping whatever it was halfway
         * through, so that continuing it later is one bit rather than a
         * recovery */
        stopped_count = 0;
        out_reset();
        CHECK(tty_intercept(KEY_CTRL_Z), "ctrl+z is taken as well");
        CHECK(stopped_count == 3, "and stops every member of the job");
        CHECK(sched_thread_stopped(21) && sched_thread_stopped(22)
              && sched_thread_stopped(23), "all three of them");
        CHECK(!process_interrupt_pending(a),
              "without delivering anything -- a stopped program is not "
              "asked, it is simply not run");

        CHECK(tty_foreground() == TTY_SHELL,
              "and the terminal comes straight back to the shell");

        /* the shell has no other way to hear about it: there are no
         * signals here, so a note is left where it will look */
        int which = 0;
        CHECK(tty_take_stopped(&which), "a note is left saying which job");
        CHECK(which == a, "naming the group");
        CHECK(!tty_take_stopped(&which), "and taking it twice finds nothing");

        /* a background job may not read the keyboard. the keys belong
         * to whoever is being typed at */
        char buf[16];
        CHECK(tty_read_line(a, buf, sizeof buf) == -1,
              "a job that is not at the front cannot read a line");

        process_exited(a, 0, 0); process_collect(a, NULL);
        process_exited(b, 0, 0); process_collect(b, NULL);
        process_exited(c, 0, 0); process_collect(c, NULL);
    }

    /* ctrl+z with the shell at the front means nothing, but it must not
     * reach the line editor as a stray character either */
    tty_set_foreground(TTY_SHELL);
    stopped_count = 0;
    CHECK(tty_intercept(KEY_CTRL_Z),
          "ctrl+z at a prompt is swallowed rather than typed");
    CHECK(stopped_count == 0, "and stops nobody");

    /* ---- handing the terminal back ---- */
    tty_set_foreground(TTY_SHELL);
    killed_thread = -1;
    CHECK(!tty_intercept(KEY_CTRL_C),
          "with the shell back at the front, ctrl+c is a key again");
    CHECK(killed_thread == -1, "and kills nobody");

    /* a foreground pid that no longer exists must not crash the tty */
    tty_set_foreground(4242);
    (void)tty_intercept(KEY_CTRL_C);
    (void)tty_intercept(KEY_CTRL_C);
    CHECK(true, "a foreground pid that has gone does not take me with it");
    tty_set_foreground(TTY_SHELL);

    /* ---- the line discipline ----
     * a program in ring 3 never sees the keys go past, so it cannot
     * echo them itself. if the terminal does not do it, you type into
     * a void and see nothing until the program answers -- which is
     * exactly how this felt before it existed */
    {
        int pid2 = process_create("bin/ask", 0, 0, false, 0);
        process_set_thread(pid2, 11);
        tty_set_foreground(pid2);

        char buf[64];

        /* typing a word and pressing enter */
        static const int typed[] = { 'I', 'g', 'o', 'r', '\n' };
        script = typed; script_len = 5; script_at = 0;
        out_reset();
        int64_t n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 5, "a line comes back with its newline");
        CHECK(memcmp(buf, "Igor\n", 5) == 0, "and holds what was typed");
        CHECK(strcmp(out, "Igor\n") == 0,
              "and every character was echoed as it was typed -- without "
              "this you type into a void");

        /* backspace takes a character off the screen as well as the
         * buffer, or the display and the line stop agreeing */
        static const int fixed[] = { 'I', 'g', 'p', '\b', 'o', 'r', '\n' };
        script = fixed; script_len = 7; script_at = 0;
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 5 && memcmp(buf, "Igor\n", 5) == 0,
              "backspace removes the character from the line");
        CHECK(strstr(out, "\b \b") != NULL,
              "and erases it from the screen too");

        /* backspace on an empty line must not run backwards past the
         * start, over the prompt somebody else printed */
        static const int over[] = { '\b', '\b', 'a', '\n' };
        script = over; script_len = 4; script_at = 0;
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 2 && buf[0] == 'a', "backspace on an empty line does nothing");
        CHECK(strstr(out, "\b \b") == NULL, "and erases nothing");

        /* arrows have no meaning in a line this simple, and echoing
         * them would draw nonsense */
        static const int arrows[] = { 'h', KEY_UP, KEY_LEFT, 'i', '\n' };
        script = arrows; script_len = 5; script_at = 0;
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 3 && memcmp(buf, "hi\n", 3) == 0, "arrows are ignored");
        CHECK(strcmp(out, "hi\n") == 0, "and never echoed");

        /* a full buffer hands over what it has rather than writing past
         * the end or dropping keys in silence */
        static const int lots[] = { 'a','b','c','d','e','f','\n' };
        script = lots; script_len = 7; script_at = 0;
        n = tty_read_line(pid2, buf, 4);
        CHECK(n == 3, "a full buffer returns early");
        CHECK(memcmp(buf, "abc", 3) == 0, "with exactly what fitted");

        /* an interrupt part way through abandons the line */
        static const int cut[] = { 'x', 'y' };
        script = cut; script_len = 2; script_at = 0;
        process_interrupt(pid2);
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == -1, "an interrupt before the first key abandons the read");

        /* and a process that is not at the front may not read at all */
        tty_set_foreground(TTY_SHELL);
        CHECK(tty_read_line(pid2, buf, sizeof buf) == -1,
              "a background process is refused the keyboard");
        CHECK(tty_read_line(pid2, buf, 0) == -1, "and a zero-length read too");

        process_exited(pid2, 0, 0);
        process_collect(pid2, NULL);
    }

    if (!failures) printf("all good\n");
    return failures;
}
