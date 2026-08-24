#include "sched/process.h"
#include "lib/string.h"

/* an environment, as a block of "NAME=value" strings.
 *
 * each ended by a NUL, with an empty string for the end of the lot.
 * that shape is not nostalgia: it is what makes the whole thing one
 * memcpy to inherit, and inheriting is most of what an environment is
 * *for*. a table of pointers would need every one of them rewritten on
 * the way into a child.
 *
 * this is here rather than in process.c because the shell needs exactly
 * these operations and has no process to perform them on -- it is a
 * kernel thread, so its environment lives in its session. one
 * implementation and two callers, which is one fewer chance for them to
 * disagree about what "already set" means.
 *
 * every operation is a walk. that is fine: an environment has tens of
 * entries and is read at the speed somebody types. */


static size_t entry_len(const char *p) {
    size_t n = 0;
    while (p[n] != '\0') {
        n++;
    }
    return n;
}

/* does this entry name that variable? the name ends at the equals, and
 * "PATHological=x" must not answer to "PATH" */
static bool names(const char *entry, const char *name) {
    size_t i = 0;
    while (name[i] != '\0') {
        if (entry[i] != name[i]) {
            return false;
        }
        i++;
    }
    return entry[i] == '=';
}

bool env_block_get(const char *block, size_t len, const char *name,
                   char *out, size_t max) {
    size_t at = 0;
    while (at < len && block[at] != '\0') {
        const char *entry = &block[at];
        size_t n = entry_len(entry);
        if (names(entry, name)) {
            const char *value = entry;
            while (*value != '=') {
                value++;
            }
            value++;
            size_t i = 0;
            while (value[i] != '\0' && i + 1 < max) {
                out[i] = value[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
        at += n + 1;
    }
    if (max > 0) {
        out[0] = '\0';
    }
    return false;
}

bool env_block_set(char *block, size_t *len, size_t max, const char *name,
                   const char *value) {
    /* take the old one out first, wherever it was. setting a variable
     * twice must not leave two of it -- and a walk would then find
     * whichever came first, which is not necessarily the newer */
    size_t at = 0;
    while (at < *len && block[at] != '\0') {
        size_t n = entry_len(&block[at]);
        if (names(&block[at], name)) {
            size_t after = at + n + 1;
            memmove(&block[at], &block[after], *len - after);
            *len -= n + 1;
            continue;       /* the same slot now holds the next entry */
        }
        at += n + 1;
    }

    if (value == NULL) {
        return true;        /* removing it, and it is gone */
    }

    size_t name_len = entry_len(name);
    size_t value_len = entry_len(value);
    size_t need = name_len + 1 + value_len + 1;

    /* the block always ends in an empty string, so there has to be room
     * for that too */
    if (*len + need + 1 > max) {
        return false;
    }

    /* the new one goes at the end, just before the terminator */
    size_t put = (*len > 0) ? *len - 1 : 0;
    memcpy(&block[put], name, name_len);
    block[put + name_len] = '=';
    memcpy(&block[put + name_len + 1], value, value_len);
    block[put + name_len + 1 + value_len] = '\0';
    block[put + need] = '\0';       /* and the end of the lot */
    *len = put + need + 1;
    return true;
}

