#ifndef FS_PATH_H
#define FS_PATH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* working out what a name means.
 *
 * up to now every path was absolute or nearly so, and `..` was thrown
 * away by the resolver without comment -- there was nowhere to go back
 * *to*, because nothing had any idea where it was standing.
 *
 * this is that idea. a process has a working directory, a name is read
 * relative to it unless it starts with a slash, and the whole thing is
 * flattened into one absolute path before anybody goes looking on a
 * disk. `.` means here, `..` means back one, repeated slashes mean
 * nothing at all, and `..` from the root stays at the root -- which is
 * the one rule that has to be got right, because a path that can climb
 * above `/` is a path that can name anything at all.
 *
 * none of this touches a filesystem, which is the point: it is the part
 * that is fiddly and exacting and entirely testable. */

#define PATH_MAX 256

/* flatten `path`, read relative to `cwd`, into `out`.
 *
 * returns false if it will not fit, and writes nothing useful then --
 * a truncated path is a different path, and quietly acting on one is
 * how you delete the wrong thing */
bool path_resolve(const char *cwd, const char *path, char *out, size_t size);

/* the directory containing `path`, and the last component of it. either
 * may be NULL if it is not wanted. `path` must already be absolute */
bool path_split(const char *path, char *dir, size_t dir_size,
                char *name, size_t name_size);

/* is `path` the root, however it happens to be spelled */
bool path_is_root(const char *path);

#endif
