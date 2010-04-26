#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "is_daemon.h"

char *child_log_name = NULL;
char *parent_log_name = NULL;


static void vlogmsg(int level, char *name, char *format, va_list va);



void logchild(int level, char *format, ...)
{
	va_list va;

	va_start(va, format);
	vlogmsg(level, child_log_name, format, va);
	va_end(va);
}


void logparent(int level, char *format, ...)
{
	va_list va;

	va_start(va, format);
	vlogmsg(level, parent_log_name, format, va);
	va_end(va);
}


static void vlogmsg(int level, char *name, char *format, va_list va)
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
