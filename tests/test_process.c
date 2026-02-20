/* the process table: the one structure whose job is to outlive things.
 *
 * a thread is reaped the instant it dies, stack and address space handed
 * straight back, so an exit code kept on the thread would be gone before
 * anyone could read it. these slots stay occupied until collected, which
 * is what makes `run` able to report how a program went. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* what the lock complains through */
void kprintf(const char *fmt, ...) { (void)fmt; }
void panic(const char *fmt, ...) {
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <stdbool.h>

#include "sched/process.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void) {
    /* ---- the ordinary life of one ---- */
    int pid = process_create("bin/hello", 0, 0, false, 1000);
    CHECK(pid > 0, "a process gets a pid");
    CHECK(process_count() == 1, "and occupies a slot");

    const struct process *p = process_find(pid);
    CHECK(p != NULL && strcmp(p->name, "bin/hello") == 0, "with its name");
    CHECK(p->started_ms == 1000, "and when it began");
    CHECK(!p->exited, "and it has not finished");

    process_set_thread(pid, 7);
    CHECK(process_find(pid)->thread_id == 7, "I can note which thread runs it");

    /* collecting one that is still running must not succeed */
    int code = 999;
    CHECK(!process_collect(pid, &code), "a running process cannot be collected");
    CHECK(code == 999, "and the code is left alone");
    CHECK(process_count() == 1, "and it keeps its slot");

    /* ---- it ends, and the code survives the thread ---- */
    process_exited(pid, 42, 1500);
    p = process_find(pid);
    CHECK(p->exited && p->exit_code == 42, "the code is recorded");
    CHECK(p->ended_ms == 1500, "and when");
    CHECK(p->thread_id == 0, "and the thread is forgotten, being gone");
    CHECK(process_count() == 1,
          "the slot stays occupied -- this is the zombie, and the point");

    CHECK(process_collect(pid, &code) && code == 42, "collecting yields the code");
    CHECK(process_count() == 0, "and frees the slot");
    CHECK(process_find(pid) == NULL, "the pid means nothing afterwards");
    CHECK(!process_collect(pid, &code), "and cannot be collected twice");

    /* ---- pids are not reused while anything remembers them ---- */
    int a = process_create("one", 0, 0, false, 0);
    int b = process_create("two", a, 0, false, 0);
    CHECK(a != b, "two processes get different pids");
    CHECK(b > a, "and later ones are later");
    CHECK(process_find(b)->parent == a, "a parent is remembered");

    process_exited(a, 0, 10);
    process_collect(a, NULL);
    int c = process_create("three", 0, 0, false, 0);
    CHECK(c != a && c != b, "a freed slot does not hand back the old pid");
    CHECK(process_collect(a, NULL) == false,
          "and the collected pid stays meaningless");

    /* collecting with a NULL code pointer is allowed */
    process_exited(b, 3, 20);
    CHECK(process_collect(b, NULL), "a caller may not care what the code was");

    process_exited(c, 0, 0);
    process_collect(c, NULL);
    CHECK(process_count() == 0, "table empty again");

    /* ---- killed, and killed twice ---- */
    int k = process_create("victim", 0, 0, false, 0);
    process_exited(k, PROCESS_KILLED, 5);
    CHECK(process_find(k)->exit_code == PROCESS_KILLED, "a kill is recorded");

    /* a process already on its way out must keep its first answer --
     * otherwise a kill racing a clean exit rewrites history */
    process_exited(k, 0, 9);
    CHECK(process_find(k)->exit_code == PROCESS_KILLED,
          "the first ending is the true one");
    CHECK(process_find(k)->ended_ms == 5, "including when it happened");
    process_collect(k, NULL);

    /* ---- walking the table ---- */
    int ids[4];
    for (int i = 0; i < 4; i++) {
        char name[8] = { 'p', (char)('0' + i), 0 };
        ids[i] = process_create(name, 0, 0, false, 0);
    }
    CHECK(process_count() == 4, "four in the table");

    size_t seen = 0;
    for (size_t i = 0; process_at(i) != NULL; i++) {
        seen++;
    }
    CHECK(seen == 4, "and the walk finds all four");

    /* a hole in the middle must not stop the walk */
    process_exited(ids[1], 0, 0);
    process_collect(ids[1], NULL);
    seen = 0;
    for (size_t i = 0; process_at(i) != NULL; i++) {
        seen++;
    }
    CHECK(seen == 3, "a freed slot in the middle is skipped, not fatal");

    for (int i = 0; i < 4; i++) {
        process_exited(ids[i], 0, 0);
        process_collect(ids[i], NULL);
    }

    /* ---- a full table refuses rather than overwrites ---- */
    int made = 0;
    for (int i = 0; i < MAX_PROCESSES + 4; i++) {
        if (process_create("crowd", 0, 0, false, 0) != 0) {
            made++;
        }
    }
    CHECK(made == MAX_PROCESSES, "the table fills to exactly its size");
    CHECK(process_create("one too many", 0, 0, false, 0) == 0,
          "and then says no rather than trampling somebody");

    /* empty it again, or everything below would be testing a full table */
    for (;;) {
        const struct process *q = process_at(0);
        if (q == NULL) break;
        process_exited(q->pid, 0, 0);
        process_collect(q->pid, NULL);
    }
    CHECK(process_count() == 0, "and empties again");

    /* ---- nonsense pids ---- */
    CHECK(process_find(0) == NULL, "pid 0 is not a process");
    CHECK(process_find(-1) == NULL, "nor is a negative one");
    CHECK(!process_collect(0, NULL), "and neither can be collected");

    /* ---- file descriptors ---- */
    {
        static const char body[] = "hello there";
        int fp = process_create("reader", 0, 0, false, 0);

        CHECK(process_fd_count(fp) == 0, "a new process holds no files");

        int fd = process_fd_open(fp, body, 11);
        CHECK(fd >= FD_FIRST_FILE,
              "an opened file gets a number above the three the console owns");
        CHECK(process_fd_count(fp) == 1, "and is counted");

        const void *data = NULL;
        uint64_t left = 0;
        CHECK(process_fd_peek(fp, fd, &data, &left), "the fd can be looked at");
        CHECK(left == 11 && data == body, "at the beginning, with it all left");

        process_fd_advance(fp, fd, 5);
        process_fd_peek(fp, fd, &data, &left);
        CHECK(left == 6 && data == body + 5,
              "and remembers where it got to between reads");

        process_fd_advance(fp, fd, 999);
        process_fd_peek(fp, fd, &data, &left);
        CHECK(left == 0 && data == body + 11,
              "reading past the end lands exactly on it, rather than beyond");

        CHECK(!process_fd_peek(fp, FD_STDIN, NULL, NULL), "fd 0 is not a file");
        CHECK(!process_fd_peek(fp, FD_STDOUT, NULL, NULL), "nor fd 1");
        CHECK(!process_fd_peek(fp, 99, NULL, NULL), "nor one out of range");
        CHECK(!process_fd_peek(fp, -1, NULL, NULL), "nor a negative one");

        CHECK(process_fd_close(fp, fd), "closing works");
        CHECK(!process_fd_close(fp, fd), "but only once");
        CHECK(!process_fd_peek(fp, fd, NULL, NULL), "and the fd is gone");

        int opened = 0;
        for (int i = 0; i < MAX_FDS + 4; i++) {
            if (process_fd_open(fp, body, 11) >= 0) opened++;
        }
        CHECK(opened == MAX_FDS - FD_FIRST_FILE,
              "it fills to exactly the fds that are not the console");
        CHECK(process_fd_open(fp, body, 11) == -1,
              "and then says no rather than overwriting one");

        process_exited(fp, 0, 0);
        CHECK(process_fd_count(fp) == 0,
              "a process that ends takes its open files with it");
        process_collect(fp, NULL);

        CHECK(process_fd_open(9999, body, 11) == -1, "no process, no fd");
        CHECK(!process_fd_peek(9999, 3, NULL, NULL), "and nothing to peek at");
    }

    if (!failures) printf("all good\n");
    return failures;
}
