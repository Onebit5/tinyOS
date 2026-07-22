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

/* group 0 means the kernel shell, which is where the terminal goes back
 * to whenever a program finishes */
#define TTY_SHELL 0

/* ---- and now there are four of them ---------------------------------
 *
 * every question below used to have one answer for the machine. each of
 * them now has one answer *per console*: which group is at the front of
 * console 2 is a different question from which is at the front of
 * console 1, and only the console being looked at is the one the
 * keyboard is talking to.
 *
 * which console a caller means is not passed in. it is whichever the
 * calling thread belongs to -- because that is always the right answer
 * and passing it would only be an opportunity to pass the wrong one */

/* the front of the terminal is a *group*, not a process. `cat x | wc`
 * is three processes and one thing the person typing is thinking about,
 * and every question the terminal asks -- may you read these keys, does
 * this ctrl+c reach you -- has to be asked of the whole job */
void tty_set_foreground(int pgid);
int  tty_foreground(void);

/* a key arrived. returns true if the tty consumed it rather than
 * passing it on as a character -- ctrl+c and ctrl+z aimed at a program
 * are requests, not bytes, and must not end up in anybody's buffer */
bool tty_intercept(int key);

/* did ctrl+z stop the foreground since I last asked? the shell polls
 * this while it waits, because it is the only way it finds out: there
 * are no signals here, so a suspended job announces itself by the
 * waiting having a second reason to end */
bool tty_take_stopped(int *pgid);

/* may this process read the keyboard at all? it must be at the front of
 * its own console *and* that console must be the one on the screen --
 * a shell on console 3 is at the front of console 3 and is still not
 * being typed at */
bool tty_is_current(int pid);

/* which console the calling thread belongs to. for the console driver's
 * owner hook, and for anything that wants to say where it is */
unsigned tty_my_console(void);

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
