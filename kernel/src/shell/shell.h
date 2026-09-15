#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

/* the velvet room terminal: one session, from the greeting to whenever
 * somebody logs out.
 *
 * it used to be marked noreturn, because a shell was the last thing a
 * thread ever did. it returns on `logout` now -- and that is the point
 * rather than a detail. a session that can *end* is one that leaves
 * nothing behind for the next person, and something has to be there to
 * start the next one. that something is init */
void shell_run(void);

#endif
