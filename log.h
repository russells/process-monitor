/* Log messages. */

#ifndef __log_h__
#define __log_h__

#include <sys/types.h>

enum level {
	CM_INFO,
	CM_WARN,
	CM_ERROR,
};


void set_parent_log_name(const char *name);
void set_child_log_name(const char *name);
void set_child_log_pid(pid_t pid);
const char *get_parent_log_ident(void);
const char *get_parent_log_name(void);
const char *get_child_log_ident(void);
const char *get_child_log_name(void);

/**
 * Log a message from the parent process.
 */
void logparent(int level, char *format, ...)
#ifdef __GNUC__
	__attribute__ ((format (printf, 2, 3)))
#endif
	;

/**
 * Log a message from the child process.
 */
void logchild(int level, char *format, ...)
#ifdef __GNUC__
	__attribute__ ((format (printf, 2, 3)))
#endif
	;

#endif
