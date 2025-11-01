#ifndef SCHED_AUTH_H
#define SCHED_AUTH_H

#include <stddef.h>
#include <stdbool.h>

/* who is allowed in, read out of a file in the ramdisk.
 *
 * this is worth being plain about: the passwords are stored in the
 * clear, and that is not a corner cut but the honest shape of what a
 * user means here. the interesting half of an account is not how its
 * password is kept -- storing one properly needs somewhere to write,
 * which this kernel does not have -- but what its uid can and cannot
 * reach. that half is enforced by hardware: ring 3, an address space
 * of its own, and a kernel that checks a uid before handing anything
 * over. without that boundary a "user" would be a variable that says
 * you are an admin. */

#define AUTH_MAX_ACCOUNTS 8
#define AUTH_NAME_MAX     24
#define AUTH_DESC_MAX     48

struct account {
    char name[AUTH_NAME_MAX];
    char password[AUTH_NAME_MAX];
    int  uid;
    char description[AUTH_DESC_MAX];
};

/* read the accounts out of the ramdisk. safe to call with no ramdisk */
void auth_init(void);

/* the parser, given the text directly. split out so the awkward parts
 * -- comments, blank lines, a line with a field missing -- can be fed
 * in deliberately rather than hoped about */
void auth_load(const char *text, size_t len);

/* returns the uid, or -1 if the name or the password is wrong. it does
 * not say which, because saying which tells an attacker half of it */
int auth_login(const char *name, const char *password);

const struct account *auth_find(const char *name);
const char *auth_name_for(int uid);
size_t auth_count(void);

#endif
