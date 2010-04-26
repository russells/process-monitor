/* Log messages. */

#ifndef __log_h__
#define __log_h__


enum level {
	CM_INFO,
	CM_WARN,
	CM_ERROR,
};

extern char *child_log_name;
extern char *parent_log_name;

/**
 * Log a message from the parent process.
 */
void logparent(int level, char *format, ...);
/**
 * Log a message from the child process.
 */
void logchild(int level, char *format, ...);

#endif
