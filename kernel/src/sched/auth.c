#include "sched/auth.h"
#include "fs/vfs.h"
#include "lib/string.h"

static struct account accounts[AUTH_MAX_ACCOUNTS];
static size_t count;

/* copy one colon-delimited field, stopping at the delimiter, the end of
 * the line, or the end of the buffer I was given */
static size_t take_field(const char *text, size_t len, size_t at,
                         char *out, size_t max) {
    size_t n = 0;
    while (at < len && text[at] != ':' && text[at] != '\n') {
        if (n + 1 < max) {
            out[n++] = text[at];
        }
        at++;
    }
    out[n] = '\0';
    return at;
}

void auth_load(const char *text, size_t len) {
    count = 0;
    if (text == NULL) {
        return;
    }

    size_t at = 0;
    while (at < len && count < AUTH_MAX_ACCOUNTS) {
        /* skip blank lines and comments, which is most of the file */
        while (at < len && (text[at] == '\n' || text[at] == ' ')) {
            at++;
        }
        if (at < len && text[at] == '#') {
            while (at < len && text[at] != '\n') {
                at++;
            }
            continue;
        }
        if (at >= len) {
            break;
        }

        struct account a;
        char uid_text[12];

        at = take_field(text, len, at, a.name, sizeof a.name);
        if (at < len && text[at] == ':') at++;
        at = take_field(text, len, at, a.password, sizeof a.password);
        if (at < len && text[at] == ':') at++;
        at = take_field(text, len, at, uid_text, sizeof uid_text);
        if (at < len && text[at] == ':') at++;
        at = take_field(text, len, at, a.description, sizeof a.description);

        /* a line missing its name or its uid is not an account. skip it
         * rather than inventing one -- a half-read passwd file letting
         * somebody in would be the worst possible failure here */
        if (a.name[0] != '\0' && uid_text[0] != '\0') {
            int uid = 0;
            bool ok = true;
            for (const char *p = uid_text; *p; p++) {
                if (*p < '0' || *p > '9') {
                    ok = false;
                    break;
                }
                uid = uid * 10 + (*p - '0');
            }
            if (ok) {
                a.uid = uid;
                accounts[count++] = a;
            }
        }

        while (at < len && text[at] != '\n') {
            at++;
        }
    }
}

const struct account *auth_find(const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(accounts[i].name, name) == 0) {
            return &accounts[i];
        }
    }
    return NULL;
}

int auth_login(const char *name, const char *password) {
    const struct account *a = auth_find(name);
    /* one answer for a wrong name and a wrong password. saying which
     * was wrong hands over half of it */
    if (a == NULL || strcmp(a->password, password) != 0) {
        return -1;
    }
    return a->uid;
}

const char *auth_name_for(int uid) {
    for (size_t i = 0; i < count; i++) {
        if (accounts[i].uid == uid) {
            return accounts[i].name;
        }
    }
    return "somebody";
}

size_t auth_count(void) {
    return count;
}

#ifndef TINYOS_HOSTED

void auth_init(void) {
    /* a bare name, so a disk may supply its own passwd and a machine
     * without one still finds the copy it booted with. worth being
     * clear-eyed about: that means whoever can write the disk can
     * decide who the master is. only the master can write it, so the
     * circle closes -- but it is the sort of thing that stops being
     * true the moment anyone else is allowed to */
    const void *data = NULL;
    uint64_t size = 0;
    bool owned = false;

    if (!vfs_slurp("passwd", &data, &size, &owned)) {
        auth_load(NULL, 0);
        return;
    }
    auth_load(data, size);
    vfs_release(data, owned);
}

#endif
