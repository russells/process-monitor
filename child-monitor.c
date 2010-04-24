/* Monitor a child process and restart it if it crashes. */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <getopt.h>
#include <pty.h>


/**
 * Contains a list of strings to set or unset in the environment.  We guarantee
 * that env[maxlen]==NULL.
 */
struct env_list {
	char **env;		/* Strings */
	size_t len;		/* The number we have */
	size_t maxlen;		/* Max size of env */
};


static void usage(int exitcode);
static void add_env(char *envvar);
static void add_env_to_list(struct env_list *el, char *envvar);
static void setup_env(void);
static void go_daemon(void);
static void set_signal_handlers(void);
static void monitor_child(void);
static void read_signal_command_pipe(void);
static void read_pty_fd(void);
static void start_child(void);
static void set_child_wait_time(void);
static void make_signal_pipe(void);
static void signal_handler(int sig);

/*
 * These are essentially event handlers for the main loop.  The real signal
 * handlers merely write a single byte to a pipe, and the main loop sees this
 * as a return from select(), and calls the right function here.
 *
 * This is called the "self-pipe trick" and is a common way to make select()
 * signal safe.
 */
static void handle_alarm_signal(void);
static void handle_child_signal(void);
static void handle_hup_signal(void);
static void handle_int_signal(void);
static void handle_term_signal(void);
static void handle_usr1_signal(void);
static void handle_usr2_signal(void);

static void vlogmsg(int level, char *name, char *format, va_list va);
static void logparent(int level, char *format, ...);
static void logchild(int level, char *format, ...);


static char *  myname;
static int     go_daemon_flag = 0;
static int     is_daemon = 0;
static char *  email_address = NULL;
static char *  log_name = NULL;
static char ** child_args = NULL;
static int     clear_env_flag = 0;
/** List of env vars to set in the child. */
struct env_list child_env_list = { NULL, 0, 0 };
/** List of env vars to remove from the child environment. */
struct env_list child_unenv_list = { NULL, 0, 0 };
static pid_t   child_pid = 0;
static char *  pid_file = NULL;
static int     do_restart = 1;
static int     do_exit = 0;
static int     signal_command_pipe[2];
static int     pty_fd = -1;
#define PTY_LINE_LEN 2048
static char    pty_data[PTY_LINE_LEN];
static int     pty_data_len = 0;
static int     min_child_wait_time = 2;
static int     max_child_wait_time = 300; /* 5 minutes */
static int     child_wait_time = 2;


static const char *short_options = "dCE:e:hL:l:M:m:p:";
static struct option long_options[] = {
	{ "daemon"        , 0, NULL, 'd' },
	{ "clear-env"     , 0, NULL, 'C' },
	{ "email"         , 1, NULL, 'e' },
	{ "env"           , 1, NULL, 'E' },
	{ "child-log-name", 1, NULL, 'L' },
	{ "help"          , 1, NULL, 'h' },
	{ "log-name"      , 1, NULL, 'l' },
	{ "max-wait-time" , 1, NULL, 'M' },
	{ "min-wait-time" , 1, NULL, 'm' },
	{ "pid-file"      , 1, NULL, 'p' },
	{ 0               , 0,    0,   0 }
};


int main(int argc, char **argv)
{
	int c;
	char *endptr;
	char *slashptr;

	slashptr = strrchr(argv[0], '/');
	if (slashptr)
		myname = slashptr + 1;
	else
		myname = argv[0];

	while (1) {
		c = getopt_long(argc, argv, short_options, long_options, NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'd':
			go_daemon_flag = 1;
			break;
		case 'C':
			clear_env_flag = 1;
			break;
		case 'E':
			add_env(optarg);
			break;
		case 'e':
			email_address = optarg;
			break;
		case 'h':
			usage(0);
			break;
		case 'L':
			log_name = optarg;
			break;
		case 'l':
			myname = optarg;
			break;
		case 'M':
			max_child_wait_time = (int)strtol(optarg, &endptr, 10);
			if (*endptr || max_child_wait_time < 0) {
				logparent(LOG_ERR,
					  "strange max wait time: %d\n",
					  max_child_wait_time);
				exit(1);
			}
			break;
		case 'm':
			min_child_wait_time = (int)strtol(optarg, &endptr, 10);
			if (*endptr || min_child_wait_time < 0) {
				logparent(LOG_ERR,
					  "strange min wait time: %d\n",
					  min_child_wait_time);
				exit(1);
			}
			break;
		case 'p':
			pid_file = optarg;
			break;
		case '?':
			usage(1);
			break;
		default:
			if (isprint(c))
				fprintf(stderr,
					"%s: unknown option char '%c'\n",
					myname, c);
			else
				fprintf(stderr,
					"%s: unknown option char 0x%02x\n",
					myname, c);
			usage(1);
			break;
		}
	}

	child_wait_time = min_child_wait_time;
	if (max_child_wait_time < min_child_wait_time) {
		max_child_wait_time = min_child_wait_time;
		logparent(LOG_INFO, "max wait time set to %d seconds\n",
			  max_child_wait_time);
	}

	if (! argv[optind]) {
		usage(1);
	}
	if (! log_name) {
		log_name = argv[optind];
		slashptr = strrchr(log_name, '/');
		if (slashptr)
			log_name = slashptr + 1;
	}
	child_args = argv + optind;

	make_signal_pipe();
	if (go_daemon_flag) {
		/* Open the log first, for error messages in go_daemon(). */
		openlog(log_name, LOG_PID, LOG_DAEMON);
		go_daemon();
	}
	set_signal_handlers();
	monitor_child();
	logparent(LOG_WARNING, "monitor_child() returned."
		  "  This should not happen.\n");
	exit(88);
}


void usage(int exitcode)
{
	/* We haven't become a daemon yet, so use printf instead of logmsg.
	 * Also, logmsg prefixes the message with myname, which we don't want
	 * here.
	 */
	fprintf(stderr, "\
Usage: %s [args] [--] childpath [child_args...]\n\
  -d|--daemon                 Go into the background\n\
                                (changes some signal handling behaviour)\n\
  -C|--clear-env              Clear the environment before setting the vars\n\
                              specified with -E\n\
  -E|--env <var=value>        Environment var for child process\n\
                                (can use multiple times)\n\
  -e|--email <addr>           Email when child restarts\n\
                                (not implemented)\n\
  -h|--help                   This message\n\
  -L|--child-log-name <name>  Name to use in messages that come from the\n\
                               child process\n\
  -l|--log-name <name>        Name to use in our own messages\n\
  -M|--max-wait-time <time>   Maximum time between child starts\n\
  -m|--min-wait-time <time>   Minimum time between child starts\n\
                                (seconds, cannot be less than 1)\n\
  -p|--pid-file <file>        Write PID to <file>, if in the background\n\
  -- is required if childpath or any of child_args begin with -\n",
		myname);
	exit(exitcode);
}


static void setup_env()
{
	char **envvars;
	int ret;

	if (clear_env_flag)
		clearenv();

	if (child_env_list.env && child_env_list.len) {
		envvars = child_env_list.env;
		while (*envvars) {
			ret = putenv(*envvars);
			if (-1 == ret) {
				logchild(LOG_WARNING,
					 "error   setting %s\n", envvars);
			}
			envvars++;
		}
	}
	if (child_unenv_list.env && child_unenv_list.len) {
		envvars = child_unenv_list.env;
		while (*envvars) {
			ret = unsetenv(*envvars);
			if (-1 == ret) {
				logchild(LOG_WARNING,
					 "error unsetting %s\n", envvars);
			}
			envvars++;
		}
	}
}


static void add_env_to_list(struct env_list *envp, char *envvar)
{
	if (! envp->env) {
		/* New array */
		envp->maxlen = 10;
		envp->env = malloc(sizeof(char*)*envp->maxlen);
		if (! envp->env) {
			logparent(LOG_ERR,
				  "cannot malloc() for env: %s\n",
				  strerror(errno));
			exit(2);
		}



	} else if (envp->len == envp->maxlen-2) {
		/* Extend the existing array */
		char **new_env;
		envp->maxlen += 10;
		new_env = realloc(envp->env, sizeof(char*)*envp->maxlen);
		if (! new_env) {
			logparent(LOG_ERR,
				  "cannot realloc() for env: %s\n",
				  strerror(errno));
			exit(2);
		}
	}
	envp->env[envp->len++] = envvar;
	envp->env[envp->len  ] = NULL;
}


static void add_env(char *envvar)
{
	char *equals;

	equals = strchr(envvar, '=');
	if (equals == envvar) {
		/* = at the beginning of the string */
		logparent(LOG_ERR, "bad environment variable: %s\n", envvar);
		exit(1);
	}
	if (equals) {
		add_env_to_list(&child_env_list, envvar);
	} else {
		add_env_to_list(&child_unenv_list, envvar);
	}
}


static void go_daemon(void)
{
	int ret;
	pid_t sid;

	/* APUE, p?? */
	ret = fork();
	if (-1 == ret) {
		logparent(LOG_WARNING, "cannot fork: %s\n", strerror(errno));
		exit(2);
	}
	if (0 != ret) {
		/* parent */
		exit(0);
	}

	/* We're not the foreground process any more. */
	is_daemon = 1;

	close(0);
	close(1);
	close(2);
	/* Make sure something is open on fd 0,1,2. */
	open("/dev/null", O_RDONLY);
	open("/dev/null", O_WRONLY);
	open("/dev/null", O_WRONLY);
	sid = setsid();
	if (-1 == sid) {
		logparent(LOG_CRIT, "cannot setsid(): %s\n", strerror(errno));
		exit(2);
	}
	if (pid_file) {
		FILE *pid_file_file;

		pid_file_file = fopen(pid_file, "w");
		if (! pid_file_file) {
			logparent(LOG_WARNING,
				  "cannot open %s for writing: %s\n",
				  pid_file, strerror(errno));
		} else {
			fprintf(pid_file_file, "%d\n", (int)getpid());
			fclose(pid_file_file);
		}
	}
}


static void set_signal_handlers(void)
{
	struct sigaction sa;

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;

	sigaction(SIGALRM, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGHUP , &sa, NULL);
	sigaction(SIGINT , &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}


/**
 * Run child and monitor the process.
 *
 * The child will be restarted when it exits.  Some signals are passed on to
 * the child process: SIGHUP, SIGINT and SIGTERM.
 *
 * We wait in a call to select(), so that our signal handlers can send bytes to
 * us.  In response to those bytes, we send signals to the child process and do
 * other stuff.
 */
static void monitor_child(void)
{

	start_child();
	while (1) {
		fd_set read_fds;
		struct timeval timeout;
		int ret;
		int nfds;

		FD_ZERO(&read_fds);
		FD_SET(signal_command_pipe[0], &read_fds);
		nfds = signal_command_pipe[0];
		/* logparent(LOG_INFO, "--- select pty_fd==%d\n", pty_fd); */
		if (pty_fd >= 0) {
			FD_SET(pty_fd, &read_fds);
			if (pty_fd > signal_command_pipe[0])
				nfds = pty_fd;
		}
		nfds++;
		timeout.tv_sec = child_wait_time;
		timeout.tv_usec = 0;
		ret = select(nfds, &read_fds, 0, 0, &timeout);
		/* logparent(LOG_INFO, "--- select returns %d\n", ret); */
		if (-1 == ret && errno != EINTR) {
			logparent(LOG_WARNING, "select error: %s\n",
				  strerror(errno));
		}
		if (FD_ISSET(signal_command_pipe[0], &read_fds)) {
			read_signal_command_pipe();
		}
		if (pty_fd >= 0) {
			if (FD_ISSET(pty_fd, &read_fds)) {
				read_pty_fd();
			}
		}
	}
}


/**
 * Read from the signal pipe while bytes are available.
 *
 * The read end of the pipe is non-blocking, so we keep attempting to read
 * until we get an error, which _should_ only ever be EWOULDBLOCK.
 */
static void read_signal_command_pipe(void)
{
	char c;
	int ret;

	while (1) {
		ret = read(signal_command_pipe[0], &c, 1);
		if (0 == ret) {
			logparent(LOG_WARNING, "read end of pipe closed!!\n");
			/* Make the pipe again. */
			make_signal_pipe();
			return;
		}
		else if (-1 == ret) {
			if (errno == EWOULDBLOCK) {
				return;
			}
			logparent(LOG_WARNING,
				  "cannot read from pipe: %s\n",
				  strerror(errno));
			return;
		}

		/*
		if (isprint(c))
			logparent(LOG_INFO, "read '%c'\n", c);
		else
			logparent(LOG_INFO, "read 0x%02x\n", c);
		*/

		switch (c) {
		case 'A':
			handle_alarm_signal();
			break;
		case 'C':
			handle_child_signal();
			break;
		case 'H':
			handle_hup_signal();
			break;
		case 'I':
			handle_int_signal();
			break;
		case 'T':
			handle_term_signal();
			break;
		case '1':
			handle_usr1_signal();
			break;
		case '2':
			handle_usr2_signal();
			break;
		default:
			logparent(LOG_WARNING,
				  "unknown pipe char: 0x%02x\n", c);
			break;
		}
	}
}


/**
 * Read data from the pty.
 */
static void read_pty_fd(void)
{
	char buf[1024];

	while (1) {
		int ret;
		int i;

		ret = read(pty_fd, buf, 1024);
		if (0 == ret) {
			/* pty closed - dead child? */
			logparent(LOG_INFO, "pty closed\n");
			pty_fd = -1;
			return;
		} else if (-1 == ret) {
			if (errno != EWOULDBLOCK) {
				logparent(LOG_INFO, "cannot read from pty: %s\n",
					  strerror(errno));
				close(pty_fd);
				pty_fd = -1;
			}
			return;
		}
		for (i=0; i<ret; i++) {
			pty_data[pty_data_len++] = buf[i];
			if (buf[i] == '\n' || buf[i] == '\0') {
				pty_data[pty_data_len] = '\0';
				/* If the line ends in \r\n, move the \n back
				   one and re-terminate it so it ends in only
				   \n. */
				if (pty_data_len >=2
				    && pty_data[pty_data_len-1] == '\n'
				    && pty_data[pty_data_len-2] == '\r') {
					pty_data[pty_data_len-2] = '\n';
					pty_data[pty_data_len-1] = '\0';
				}
				logchild(LOG_INFO, "%s", pty_data);
				pty_data_len = 0;
				continue;
			}
			if (pty_data_len == PTY_LINE_LEN-1) {
				pty_data[pty_data_len] = '\0';
				logchild(LOG_INFO, "%s\n", pty_data);
				pty_data_len = 0;
				continue;
			}
		}
	}
}


/**
 * On SIGALRM, restart the child if it's not running.
 */
static void handle_alarm_signal(void)
{
	if (do_restart) {
		if (child_pid <= 0) {
			start_child();
		}
	}

	if (do_exit) {
		exit(1);
	}
}


static void handle_child_signal(void)
{
	int status;
	int wait_time;

	wait(&status);
	if (WIFSIGNALED(status)) {
		logparent(LOG_INFO,
			  "%s[%d] exited due to signal %d with status %d\n",
			  child_args[0], child_pid, WTERMSIG(status),
			  WEXITSTATUS(status));
	} else {
		int retval = WEXITSTATUS(status);
		/* 99 is returned by the child when the exec fails.  Don't log
		   that as the child will already have logged the failure. */
		if (retval != 99) {
			logparent(LOG_INFO,
				  "%s[%d] exited with status %d\n",
				  child_args[0], child_pid,
				  WEXITSTATUS(status));
		}
	}
	child_pid = 0;
	if (pty_fd >= 0) {
		logparent(LOG_INFO, "closing pty_fd (%d)\n", pty_fd);
		close(pty_fd);
		pty_fd = -1;
	}

	if (do_exit) {
		logparent(LOG_INFO, "child-monitor exiting\n");
		exit(0);
	}

	if (do_restart) {
		if (child_wait_time == 0)
			wait_time = 1;
		else
			wait_time = child_wait_time;
		logparent(LOG_INFO, "waiting for %d seconds\n", wait_time);
		alarm(wait_time);
		set_child_wait_time();
	}
}


/**
 * Adjust the child wait time.
 *
 * The child wait time increases (doubles) each time the child exits, up to a
 * maximum.
 */
static void set_child_wait_time(void)
{
	child_wait_time *= 2;
	if (child_wait_time >= max_child_wait_time)
		child_wait_time = max_child_wait_time;
}


/**
 * Pass SIGHUP to the child.  If we're not a daemon, don't restart the child
 * when it exits.  For a daemon, keep running as normal.
 */
static void handle_hup_signal(void)
{
	if (is_daemon) {
		if (child_pid <= 0) {
			logparent(LOG_INFO, "SIGHUP but no child\n");
		} else {
			logparent(LOG_INFO, "passing SIGHUP to %s[%d]\n",
				  child_args[0], child_pid);
			kill(child_pid, SIGHUP);
		}
	}
	else {
		if (child_pid >= 0) {
			kill(child_pid, SIGHUP);
			do_restart = 0;
			do_exit = 1;
		} else {
			logparent(LOG_INFO, "exiting on SIGHUP\n");
			exit(1);
		}
	}
}


/**
 * Pass SIGINT to the child, and don't restart it if it exits.
 */
static void handle_int_signal(void)
{
	if (child_pid <= 0) {
		logparent(LOG_INFO, "exiting on SIGINT\n");
		exit(1);
	}

	logparent(LOG_INFO, "passing SIGINT to %s[%d]\n",
		  child_args[0], child_pid);
	kill(child_pid, SIGINT);
	do_restart = 0;
	do_exit = 1;
}


/**
 * Pass SIGTERM to the child and exit.
 */
static void handle_term_signal(void)
{
	if (child_pid <= 0) {
		logparent(LOG_INFO, "exiting on SIGTERM\n");
		exit(1);
	}

	logparent(LOG_INFO, "passing SIGTERM to %s[%d]\n",
		  child_args[0], child_pid);
	kill(child_pid, SIGTERM);
	do_restart = 0;
	do_exit = 1;
}


static void handle_usr1_signal(void)
{
	logparent(LOG_INFO, "SIGUSR1: I will not monitor %s\n",
		  child_args[0]);
	do_restart = 0;
}


static void handle_usr2_signal(void)
{
	logparent(LOG_INFO, "SIGUSR2: I will monitor %s again\n",
		  child_args[0]);
	do_restart = 1;
	if (child_pid <= 0) {
		start_child();
	}

}


/**
 * Fork/exec the child process.
 */
static void start_child(void)
{
	pid_t pid;
	int forkpty_errno;

	logparent(LOG_INFO, "starting %s\n", child_args[0]);

	pid = forkpty(&pty_fd, NULL, NULL, NULL);
	forkpty_errno = errno;

	if (-1 == pid) {
		child_pid = -1;
		logparent(LOG_WARNING, "cannot fork: %s\n",
			  strerror(forkpty_errno));
		child_wait_time = 60;
		return;
	} else if (0 != pid) {
		/* parent */
		/* logparent(LOG_INFO, "after forkpty, pty_fd==%d\n", pty_fd); */
		child_pid = pid;
		fcntl(pty_fd, F_SETFL, O_NONBLOCK);
		return;
	}

	/* Child */
	setup_env();
	if (execv(child_args[0], child_args)) {
		logparent(LOG_WARNING, "cannot exec %s: %s\n",
			  child_args[0], strerror(errno));
		exit(99);
	}
}


static void make_signal_pipe(void)
{
	int ret;

	ret = pipe(signal_command_pipe);
	if (-1 == ret) {
		fprintf(stderr, "%s: cannot make pipe: %s\n",
			myname, strerror(errno));
		exit(2);
	}
	fcntl(signal_command_pipe[0], F_SETFL, O_NONBLOCK);
}


static void signal_handler(int sig)
{
	char c = '?';

	switch (sig) {
	case SIGALRM: c = 'A'; break;
	case SIGCHLD: c = 'C'; break;
	case SIGHUP : c = 'H'; break;
	case SIGINT : c = 'I'; break;
	case SIGTERM: c = 'T'; break;
	case SIGUSR1: c = '1'; break;
	case SIGUSR2: c = '2'; break;
	}
	write(signal_command_pipe[1], &c, 1);
}


static void logchild(int level, char *format, ...)
{
	va_list va;

	va_start(va, format);
	vlogmsg(level, log_name, format, va);
	va_end(va);
}


static void logparent(int level, char *format, ...)
{
	va_list va;

	va_start(va, format);
	vlogmsg(level, myname, format, va);
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
		syslog(level|LOG_DAEMON, "%s", msg);
	} else {
		FILE *f;
		if (level == LOG_INFO)
			f = stdout;
		else
			f = stderr;
		fprintf(f, "%s", msg);
	}
}
