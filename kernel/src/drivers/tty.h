#ifndef DRIVERS_TTY_H
#define DRIVERS_TTY_H

#include <stdbool.h>
#include <stdint.h>

/* who the keyboard belongs to.
 *
 * up to 0.1.2 nobody owned it. the shell sat in a loop peeking at keys
 * while a program ran, watching for ctrl+c and killing on the program's
 * behalf -- which meant a program could never really read the keyboard,
 * because the shell was standing in front of it.
 *
 * now there is a foreground process. it gets the keys, and ctrl+c is
 * delivered *to* it rather than acted on for it. that is the beginning
 * of a controlling terminal: one process at the front, everyone else
 * waiting their turn. */

/* pid 0 means the kernel shell, which is where the terminal goes back
 * to whenever a program finishes */
#define TTY_SHELL 0

void tty_set_foreground(int pid);
int  tty_foreground(void);

/* a key arrived. returns true if the tty consumed it as an interrupt
 * rather than as a character -- ctrl+c aimed at a program is a request,
 * not a byte, and must not end up in anybody's input buffer */
bool tty_intercept(int key);

/* read a line on behalf of a process, echoing it as it is typed.
 *
 * this is the line discipline, and it is the reason a terminal feels
 * like anything at all: characters appear as you type them, backspace
 * takes one back off the screen as well as out of the buffer, and the
 * line is handed over when you press enter. the kernel shell does its
 * own version of this for its prompt; a program in ring 3 has no way to
 * do it for itself, because the keys never pass through it.
 *
 * returns bytes read (including the newline), or -1 if this process is
 * not the foreground or was interrupted */
int64_t tty_read_line(int pid, char *buf, uint64_t len);

#endif
