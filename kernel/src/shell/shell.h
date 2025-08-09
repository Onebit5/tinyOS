#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

/* the velvet room terminal. never returns -- whoever calls this has
 * volunteered to be the shell thread for the rest of time */
void shell_run(void) __attribute__((noreturn));

#endif
