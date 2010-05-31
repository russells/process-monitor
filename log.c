#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "log.h"
#include "is_daemon.h"

/*
 * X_log_name is the process name, eg "foo".
 * X_log_ident is the string for syslog including pid, eg "foo[1234]".
 */
static char child_log_ident[50];
static const char *child_log_name = NULL;
static char parent_log_ident[50];
static const char *parent_log_name = NULL;
static pid_t parent_pid = 0;
static pid_t child_pid = 0;
static const char *log_ident = NULL;


static void format_parent_log_ident(pid_t pid);
static void format_child_log_ident(void);
static void vlogmsg(int level, const char const *name,
		    const char const *format, va_list va);


/**
 * Format a log name and a pid into an ident string for syslog.
 *
 * This is used as the the ident parameter to openlog().  We are simulating the
 * LOG_PID flag to openlog, so the message begins with "foo[1234]", where "foo"
 * is the process name (either the process name or a name specified on startup)
 * and 1234 is the pid.
 *
 * This needs to be called when either of the process name or pid is originally
 * set or changes.  The pid can change when we fork(), and the process name is
 * an arbitrary string, so the caller can change it if desired.
 *
 * \param ident place the output string here.  Up to the first 20 chars of
 * log_name will be placed here.
 * \param ident_len the total number of bytes available in ident.  Must be at
 * least 30.
 * \param log_name the name to use for syslog
 * \param pid the process id to use for syslog
 */
static void format_log_ident(char *ident, size_t ident_len,
			     const char *log_name, pid_t pid)
{
	const char *format;
	size_t name_len = 20;

	/* 30 chars allows for one null byte, "[]" and 7 chars for the pid. */
	assert( ident_len >= 30 );

	if (strlen(log_name) > name_len) {
		if (pid)
			format = "%20s[%d]";
		else
			format = "%20s";
	} else {
		if (pid)
			format = "%s[%d]";
		else
			format = "%s";
	}
	snprintf(ident, ident_len-1, format, log_name, pid);
	ident[ident_len-1] = '\0';
}


/**
 * Format the child name and child pid into a prefix for the child messages.
 *
 * This needs to be called when either of the child name or child pid change.
 */
static void format_child_log_ident(void)
{
	format_log_ident(child_log_ident, 50, child_log_name, child_pid);
}


/**
 * Set the child's name for messages that come from it.
 *
 * See format_log_ident() for details on how the log ident is constructed.
 */
void set_child_log_name(const char *name)
{
	child_log_name = name;
	format_child_log_ident();
}


/**
 * Set the child's pid for messages that come from it.
 *
 * This needs to be called whenever the child's pid changes (ie a new child is
 * run), since there is no way for the log routines to otherwise know the
 * child's pid.
 *
 * See format_log_ident() for details on how the log ident is constructed.
 */
void set_child_log_pid(pid_t pid)
{
	child_pid = pid;
	format_child_log_ident();
}


void set_parent_log_name(const char *name)
{
	parent_log_name = name;
	format_parent_log_ident(getpid());
}


/**
 * Set the string we use for logging parent messages.
 *
 * See format_log_ident() for details on how the log ident is constructed.
 */
static void format_parent_log_ident(pid_t pid)
{
	format_log_ident(parent_log_ident, 50, parent_log_name, pid);
	parent_pid = pid;
}


const char *get_parent_log_ident(void)
{
	if (parent_log_name)
		return parent_log_ident;
	else
		return NULL;
}


const char *get_parent_log_name(void)
{
	return parent_log_name;
}


const char *get_child_log_ident(void)
{
	if (child_log_name)
		return child_log_ident;
	else
		return NULL;
}


const char *get_child_log_name(void)
{
	return child_log_name;
}


void logchild(int level, char *format, ...)
{
	va_list va;

	va_start(va, format);
	vlogmsg(level, child_log_ident, format, va);
	va_end(va);
}


void logparent(int level, char *format, ...)
{
	va_list va;
	pid_t pid;

	/* Check to see if our pid has changed.  This does happen, when we do
	   the process gymnastics to detach from our terminal and become a
	   daemon.  The only down side here is a getpid() call on every log
	   message, but that's not a huge penalty. */
	if ( (pid = getpid()) != parent_pid) {
		format_parent_log_ident(pid);
	}
	va_start(va, format);
	vlogmsg(level, parent_log_ident, format, va);
	va_end(va);
}


static void vlogmsg(int level, const char const *name,
		    const char const *format, va_list va)
{
	char msg[400];
	size_t msg_avail_len = 399;
	char *msgstart = msg;

	/* If we're not a daemon, prepend the program name to the message.  If
	   we're a daemon, syslog does that for us. */
	if (! is_daemon) {
		size_t log_name_len = strlen(name);
		strncpy(msg, name, msg_avail_len);
		msgstart += log_name_len;
		msg_avail_len -= log_name_len;
		strncpy(msgstart, ": ", msg_avail_len);
		msgstart += 2;
		msg_avail_len -= 2;
	}

	vsnprintf(msgstart, msg_avail_len, format, va);
	msg[399] = '\0';
	if (is_daemon) {
		int syslog_level;
		switch (level) {
		case CM_INFO:  syslog_level = LOG_INFO;    break;
		case CM_WARN:  syslog_level = LOG_WARNING; break;
		case CM_ERROR: syslog_level = LOG_ERR;     break;
		}
		/* On Linux, this does not reopen the syslog connection each
		   time we change between logging child and parent messages.
		   It seems to only save a copy of log_ident (the pointer, not
		   the string).  Other systems may act differently. */
		if (! log_ident || (log_ident != name)) {
			log_ident = name;
			openlog(log_ident, 0, LOG_DAEMON);
		}
		syslog(syslog_level|LOG_DAEMON, "%s", msg);
	} else {
		FILE *f;
		if (level == CM_INFO)
			f = stdout;
		else
			f = stderr;
		fprintf(f, "%s", msg);
	}
}
