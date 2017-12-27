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
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>
#include <getopt.h>
#include <pty.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>

#include "log.h"
#include "envlist.h"
#include "is_daemon.h"


static void usage(int exitcode);
static void add_env(char *envvar);
static void setup_env(void);
static void go_daemon(void);
static void set_signal_handlers(void);
static void monitor_child(void);
static void wait_in_select(void);
static void read_signal_command_pipe(void);
static void read_command_fifo_fd(void);
static void read_pty_fd(void);
static void maybe_create_pid_file(void);
static void delete_pid_file(void);
static void start_child(void);
static void set_child_wait_time(void);
static void make_signal_command_pipe(void);
static void make_command_fifo(void);
static void signal_handler(int sig);
static void get_user_and_group_names(char *names);
static void send_command(void);
static void stop_monitoring(const char *reason);
static void start_monitoring(const char *reason);
static void send_hup_to_child(void);
static void send_int_to_child(void);
static void send_kill_to_child(void);
static void send_term_to_child(void);
static void kill_child_and_exit(void);
static void close_all_fd(void);

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


static char *           child_dir = NULL;
static char *			startup_sh = NULL;
static int              go_daemon_flag = 0;
static char *           email_address = NULL;
static char **          child_args = NULL;
static int              clear_env_flag = 0;
/** List of env vars to set in the child. */
static struct envlist * child_envlist = NULL;
/** List of env vars to remove from the child environment. */
static struct envlist * child_unenvlist = NULL;
/** PID of our child process.  Set to -1 to indicate that the child is not
    running. */
static pid_t            child_pid = -1;
static char *           pid_file = NULL;
static int              do_restart = 1;
static int              do_exit = 0;
static int              signal_command_pipe[2];
static int              command_fifo_fd = -1;
static int              command_fifo_write_fd = -1;
static char *           command_fifo_name = NULL;
static char *           command_name = NULL;
static int              pty_fd = -1;
#define PTY_LINE_LEN 2048
static char             pty_data[PTY_LINE_LEN];
static int              pty_data_len = 0;
static int              min_child_wait_time = 2;
static int              max_child_wait_time = 300; /* 5 minutes */
static int              child_wait_time = 2;
static uid_t            child_uid = 0;
static char *           child_username = NULL;
static gid_t            child_gid = 0;
static char *           child_groupname = NULL;
static int              release_allfd = 0;


struct pmCommand { char *command; char c; };

static struct pmCommand pmCommands[] = {
	{ "start"    , '+' },
	{ "stop"     , '-' },
	{ "exit"     , 'x' },
	{ "hup"      , 'h' },
	{ "int"      , 'i' },
	{ NULL       , '\0'}
};


static const char *short_options = "D:dCc:E:e:hL:l:M:m:P:p:S:u:V:z";
static struct option long_options[] = {
	{ "dir"           , 1, NULL, 'D' },
	{ "daemon"        , 0, NULL, 'd' },
	{ "clear-env"     , 0, NULL, 'C' },
	{ "command"       , 1, NULL, 'c' },
	{ "command-pipe"  , 1, NULL, 'P' },
	{ "email"         , 1, NULL, 'e' },
	{ "env"           , 1, NULL, 'E' },
	{ "child-log-name", 1, NULL, 'L' },
	{ "help"          , 0, NULL, 'h' },
	{ "log-name"      , 1, NULL, 'l' },
	{ "max-wait-time" , 1, NULL, 'M' },
	{ "min-wait-time" , 1, NULL, 'm' },
	{ "pid-file"      , 1, NULL, 'p' },
	{ "startup-script", 1, NULL, 'S' },
	{ "user"          , 1, NULL, 'u' },
	{ "version"       , 0, NULL, 'V' },
	{ "release-allfd" , 0, NULL, 'z' },
	{ 0               , 0,    0,   0 }
};


int main(int argc, char **argv)
{
	int c;
	char *endptr;
	char *slashptr;

	slashptr = strrchr(argv[0], '/');
	if (slashptr)
		set_parent_log_name(slashptr + 1);
	else
		set_parent_log_name(argv[0]);

	while (1) {
		c = getopt_long(argc, argv, short_options, long_options, NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'D':
			child_dir = optarg;
			break;
		case 'd':
			go_daemon_flag = 1;
			break;
		case 'C':
			clear_env_flag = 1;
			break;
		case 'c':
			command_name = optarg;
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
			set_child_log_name(optarg);
			break;
		case 'l':
			set_parent_log_name(optarg);
			break;
		case 'M':
			max_child_wait_time = (int)strtol(optarg, &endptr, 10);
			if (*endptr || max_child_wait_time < 0) {
				logparent(CM_ERROR,
					  "strange max wait time: %d\n",
					  max_child_wait_time);
				exit(1);
			}
			break;
		case 'm':
			min_child_wait_time = (int)strtol(optarg, &endptr, 10);
			if (*endptr || min_child_wait_time < 0) {
				logparent(CM_ERROR,
					  "strange min wait time: %d\n",
					  min_child_wait_time);
				exit(1);
			}
			break;
		case 'p':
			pid_file = optarg;
			break;
		case 'P':
			command_fifo_name = optarg;
			break;
		case 'S':
			startup_sh = optarg;
			break;
		case 'u':
			get_user_and_group_names(optarg);
			break;
		case 'V':
			printf("process-monitor 0.1\n");
			exit(0);
			break;
		case 'z':
			release_allfd = 1;
			break;
		case '?':
			exit(1);
			break;
		default:
			if (isprint(c))
				fprintf(stderr,
					"%s: unknown option char '%c'\n",
					get_parent_log_name(), c);
			else
				fprintf(stderr,
					"%s: unknown option char 0x%02x\n",
					get_parent_log_name(), c);
			exit(1);
			break;
		}
	}
	if (release_allfd) {
		close_all_fd();
	}

	if (child_username) {
		struct passwd *pw;
		int getpwnam_errno = 0;

		errno = 0;
		/* Look for this as a login name. */
		pw = getpwnam(child_username);
		if (pw) {
			child_uid = pw->pw_uid;
		} else {
			/* Save errno from getpwnam(), for use in a later error
			   message. */
			getpwnam_errno = errno;
			/* Now try the name as a uid. */
			child_uid = (int)strtol(child_username, &endptr, 10);
			if (*endptr || child_uid < 0) {
				if (getpwnam_errno) {
					logparent(CM_ERROR,
						  "unknown user name: %s: %s\n",
						  child_username,
						  strerror(getpwnam_errno));
				} else {
					logparent(CM_ERROR,
						  "unknown user name %s\n",
						  child_username);
				}
				exit(1);
			}
		}
	}

	if (child_groupname) {
		struct group *gr;
		int getgrnam_errno = 0;

		errno = 0;
		/* Look for this as a login name. */
		gr = getgrnam(child_groupname);
		if (gr) {
			child_gid = gr->gr_gid;
		} else {
			/* Save errno from getgrnam(), for use in a later error
			   message. */
			getgrnam_errno = errno;
			/* Now try the name as a uid. */
			child_gid = (gid_t)strtol(child_groupname, &endptr, 10);
			if (*endptr || child_gid < 0) {
				if (getgrnam_errno) {
					logparent(CM_ERROR,
						  "unknown group name: %s: %s\n",
						  child_groupname,
						  strerror(getgrnam_errno));
				} else {
					logparent(CM_ERROR,
						  "unknown group name %s\n",
						  child_groupname);
				}
				exit(1);
			}
		}
	}

	child_wait_time = min_child_wait_time;
	if (max_child_wait_time < min_child_wait_time) {
		max_child_wait_time = min_child_wait_time;
		logparent(CM_INFO, "max wait time set to %d seconds\n",
			  max_child_wait_time);
	}

	if (! argv[optind]) {
		if (command_name) {
			send_command();
			/*NOTREACHED - send_command() does not return. */
		}
		else {
			fprintf(stderr,
				"%s: need a program to run, or a command\n"
				"  -h for help\n",
				get_parent_log_name());
			exit(1);
		}
	}

	if (command_name) {
		/* We have both a program to run and a command. */
		fprintf(stderr,
			"%s: Can't use a program name and a command.\n"
			"   -h for help\n",
			get_parent_log_name());
		exit(1);
	}
	if (get_child_log_ident() == NULL) {
		slashptr = strrchr(argv[optind], '/');
		if (slashptr)
			set_child_log_name(slashptr + 1);
		else
			set_child_log_name(argv[optind]);
	}
	child_args = argv + optind;

	make_signal_command_pipe();
	make_command_fifo();
	if (go_daemon_flag) {
		go_daemon();
	}
	maybe_create_pid_file();

	set_signal_handlers();
	monitor_child();
	logparent(CM_ERROR, "monitor_child() returned."
		  "  This should not happen.\n");
	exit(88);
}


static void get_user_and_group_names(char *names)
{
	char *colon;
	char *username = NULL;
	char *groupname = NULL;

	colon = strchr(names, ':');
	if (NULL == colon) {
		/* No : in names */
		username = names;
	} else if (colon == names) {
		/* : at start of names - set group but not user */
		groupname = colon + 1;
	} else {
		/* : somewhere else */
		username = names;
		*colon = '\0';
		groupname = colon + 1;
	}
	if (username) {
		if (child_username) {
			logparent(CM_ERROR,
				  "username specified twice, "
				  "which one do I use?\n");
			exit(1);
		}
		child_username = username;
	}
	if (groupname) {
		if (child_groupname) {
			logparent(CM_ERROR,
				  "group name specified twice, "
				  "which one do I use?\n");
			exit(1);
		}
		child_groupname = groupname;
	}
}


void usage(int exitcode)
{
	/* We haven't become a daemon yet, so use printf instead of logmsg.
	 * Also, logmsg prefixes the message with parent_log_name, which we
	 * don't want here.
	 */
	fprintf(stderr, "\
Usage: %s [args] [--] childpath [child_args...]\n\
       %s -P <pipe> --command=stop|start|exit|hup|int\n\
  -C|--clear-env              Clear the environment before setting the vars\n\
                              specified with -E\n\
  -c|--command <command>      Make a running process-monitor react to\n\
                              <command>\n\
  -D|--dir <dirname>          Change to <dirname> before starting child\n\
  -d|--daemon                 Go into the background\n\
                                (changes some signal handling behaviour)\n\
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
  -P|--command-pipe <pipe>    Open named pipe <pipe> to receive commands\n\
  -p|--pid-file <file>        Write PID to <file>, if in the background\n\
  -u|--user <user>            User to run child as (name or uid)\n\
                                (can be user:group)\n\
  -z|--release-allfd          Release all opened file descriptors\n\
  -- is required if childpath or any of child_args begin with -\n",
		get_parent_log_name(), get_parent_log_name());
	exit(exitcode);
}


static void setup_env()
{
	char **envvars;
	int ret;

	if (clear_env_flag)
		clearenv();

	if (child_envlist && child_envlist->env && child_envlist->len) {
		envvars = child_envlist->env;
		while (*envvars) {
			ret = putenv(*envvars);
			if (-1 == ret) {
				logchild(CM_WARN,
					 "error   setting %s\n", *envvars);
			}
			envvars++;
		}
	}
	if (child_unenvlist && child_unenvlist->env && child_unenvlist->len) {
		envvars = child_unenvlist->env;
		while (*envvars) {
			ret = unsetenv(*envvars);
			if (-1 == ret) {
				logchild(CM_WARN,
					 "error unsetting %s\n", *envvars);
			}
			envvars++;
		}
	}
}


static void add_env(char *envvar)
{
	char *equals;
	struct envlist **el;

	equals = strchr(envvar, '=');
	if (equals == envvar) {
		/* = at the beginning of the string */
		logparent(CM_ERROR, "bad environment variable: %s\n", envvar);
		exit(1);
	}
	if (equals) {
		el = &child_envlist;
	} else {
		el = &child_unenvlist;
	}
	if (! *el) {
		*el = envlist_new();
	}
	envlist_add(*el, envvar);
}


static void go_daemon(void)
{
	int ret;
	pid_t sid;

	/* APUE, p?? */
	ret = fork();
	if (-1 == ret) {
		logparent(CM_WARN, "cannot fork: %s\n", strerror(errno));
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
		wait_in_select();
	}
}


/**
 * Do one iteration of the select() loop.
 *
 * This is separate from monitor_child() so that we can call it recursively
 * when it's time to exit.  Doing that makes the main select() loop much less
 * complex, as it does not need to know about the case where we're actively
 * waiting for the child to die, rather than just waiting to see if it does
 * die.
 */
static void wait_in_select(void)
{
	fd_set read_fds;
	struct timeval timeout;
	int ret;
	int nfds;

	FD_ZERO(&read_fds);
	FD_SET(signal_command_pipe[0], &read_fds);
	nfds = signal_command_pipe[0];
	/* logparent(CM_INFO, "--- select pty_fd==%d\n", pty_fd); */
	if (pty_fd >= 0) {
		FD_SET(pty_fd, &read_fds);
		if (pty_fd > nfds)
			nfds = pty_fd;
	}
	if (command_fifo_fd >= 0) {
		FD_SET(command_fifo_fd, &read_fds);
		if (command_fifo_fd > nfds)
			nfds = command_fifo_fd;
	}
	nfds++;
	timeout.tv_sec = child_wait_time;
	timeout.tv_usec = 0;
	ret = select(nfds, &read_fds, 0, 0, &timeout);
	/* logparent(CM_INFO, "--- select returns %d\n", ret); */
	if (-1 == ret && errno != EINTR) {
		logparent(CM_WARN, "select error: %s\n",
			  strerror(errno));
	}
	/* Read data on the pty first so we don't miss any. */
	if (pty_fd >= 0) {
		if (FD_ISSET(pty_fd, &read_fds)) {
			read_pty_fd();
		}
	}
	if (FD_ISSET(signal_command_pipe[0], &read_fds)) {
		read_signal_command_pipe();
	}
	if (command_fifo_fd >= 0
	    && FD_ISSET(command_fifo_fd, &read_fds)) {
		read_command_fifo_fd();
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
			logparent(CM_WARN, "read end of pipe closed!!\n");
			/* Make the pipe again. */
			make_signal_command_pipe();
			return;
		}
		else if (-1 == ret) {
			if (errno == EWOULDBLOCK) {
				return;
			}
			logparent(CM_WARN,
				  "cannot read from pipe: %s\n",
				  strerror(errno));
			return;
		}

		/*
		if (isprint(c))
			logparent(CM_INFO, "read '%c'\n", c);
		else
			logparent(CM_INFO, "read 0x%02x\n", c);
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
			logparent(CM_WARN,
				  "unknown pipe char: 0x%02x\n", c);
			break;
		}
	}
}


static void read_command_fifo_fd(void)
{
	int read_ret;
	char c;

	while (1) {
		read_ret = read(command_fifo_fd, &c, 1);
		switch (read_ret) {
		case 0:
			/* eof - this should never happen since we have a file
			   descriptor open for writing */
			logparent(CM_WARN, "command fifo closed, reopening\n");
			close(command_fifo_fd);
			make_command_fifo();
			return;
		case -1:
			/* error */
			if (errno != EWOULDBLOCK) {
				logparent(CM_WARN,
					  "Error reading from %s: %s\n",
					  command_fifo_name, strerror(errno));
				make_command_fifo();
			}
			return;
		default:
			/* logparent(CM_INFO, "command fifo: %c\n", c); */
			switch (c) {
			case '+':
				start_monitoring("Command");
				break;
			case '-':
				stop_monitoring("Command");
				break;
			case 'h':
				send_hup_to_child();
				break;
			case 'i':
				send_int_to_child();
				break;
			case 'x':
				kill_child_and_exit();
			default:
				if (isprint(c))
					logparent(CM_WARN,
						  "Unknown command char %c\n",
						  c);
				else
					logparent(CM_WARN,
						  "Unknown command char 0x%02x\n",
						  c);
			}
		}
	}
}


static void kill_child_and_exit(void)
{
	time_t start;

	start = time(0);

	if (child_pid <= 0)
		exit(0);
	do_restart = 0;
	do_exit = 1;
	send_term_to_child();
	min_child_wait_time = 5;
	max_child_wait_time = 5;
	while ((time(0) - start) < 6 && child_pid > 0)
		wait_in_select();
	if (child_pid > 0)
		send_kill_to_child();
	exit(0);
}

static void close_all_fd(void)
{
	int i = 3;
	for(; i < sysconf(_SC_OPEN_MAX); i++)
	{
		close(i);
	}
}


/**
 * Read data from the pty.
 */
static void read_pty_fd(void)
{
	char buf[1024];

	if (pty_fd <= 0)
		return;

	while (1) {
		int ret;
		int i;

		ret = read(pty_fd, buf, 1024);
		if (0 == ret) {
			/* pty closed - dead child? */
			logparent(CM_INFO, "pty closed\n");
			pty_fd = -1;
			return;
		} else if (-1 == ret) {
			if (errno != EWOULDBLOCK) {
				/* When the child exits we get EIO on the pty.
				 * Ignore this since it's a normal
				 * occurrence.
				 */
				if (errno != EIO)
					logparent(CM_INFO,
						  "cannot read from pty: %s\n",
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
				logchild(CM_INFO, "%s", pty_data);
				pty_data_len = 0;
				continue;
			}
			if (pty_data_len == PTY_LINE_LEN-1) {
				pty_data[pty_data_len] = '\0';
				logchild(CM_INFO, "%s\n", pty_data);
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
	pid_t pid;

	/* Read data from the child here so we flush that file before it's
	 * closed.  We seem to sometimes get the SIGCHLD (and hence end up here
	 * in the signal command pipe handler) before select notifies us that
	 * the pty is readable.  So we take the opportunity here to read
	 * anything that's available.
	 *
	 * I don't understand the buffering that's happening here, but
	 * there is definitely something buffering data on the pty.
	 */
	read_pty_fd();

	/* We do the check for child_pid==-1 after the call to waitpid() since
	 * if we get a SIGCHLD, we need to call waitpid() in any case, even if
	 * we're ignoring that child.
	 */
	pid = waitpid(-1, &status, WNOHANG);
	if (-1 == pid || -1 == child_pid || pid != child_pid) {
		return;
	}

	if (WIFSIGNALED(status)) {
		logparent(CM_INFO,
			  "%s[%d] exited due to signal %d with status %d\n",
			  child_args[0], child_pid, WTERMSIG(status),
			  WEXITSTATUS(status));
	} else {
		int retval = WEXITSTATUS(status);
		/* 99 is returned by the child when the exec fails.  Don't log
		   that as the child will already have logged the failure. */
		if (retval != 99) {
			logparent(CM_INFO,
				  "%s[%d] exited with status %d\n",
				  child_args[0], child_pid,
				  WEXITSTATUS(status));
		}
	}
	child_pid = -1;
	if (pty_fd >= 0) {
		logparent(CM_INFO, "closing pty_fd (%d)\n", pty_fd);
		close(pty_fd);
		pty_fd = -1;
	}

	if (do_exit) {
		logparent(CM_INFO, "process-monitor exiting\n");
		exit(0);
	}

	if (do_restart) {
		if (child_wait_time == 0)
			wait_time = 1;
		else
			wait_time = child_wait_time;
		logparent(CM_INFO, "waiting for %d seconds\n", wait_time);
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
static void send_hup_to_child(void)
{
	if (is_daemon) {
		if (child_pid <= 0) {
			logparent(CM_INFO, "SIGHUP but no child\n");
		} else {
			logparent(CM_INFO, "passing SIGHUP to %s[%d]\n",
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
			logparent(CM_INFO, "exiting on SIGHUP\n");
			exit(1);
		}
	}
}


static void handle_hup_signal(void)
{
	send_hup_to_child();
}


/**
 * Pass SIGINT to the child.  If we're a daemon, restart the child if it exits
 * or exit when the child exits only if we were going to do that anyway (ie
 * don't change that behaviour because we got SIGINT).  If we're not a daemon,
 * don't restart the child when it exits, and exit ourselves then.
 */
static void send_int_to_child(void)
{
	if (child_pid <= 0) {
		if (is_daemon) {
			logparent(CM_INFO, "SIGINT but no child process (%s)\n",
				  child_args[0]);
		} else {
			logparent(CM_INFO, "exiting on SIGINT\n");
			exit(1);
		}
		return;
	}

	/* We have a child process. */
	if (is_daemon) {
		logparent(CM_INFO, "passing SIGINT to %s[%d]\n",
			  child_args[0], child_pid);
		kill(child_pid, SIGINT);
		/* Don't change do_restart and do_exit. */
	} else {
		kill(child_pid, SIGINT);
		/* If we're not a daemon, then probably the user typed ^C on
		   our terminal, so when the child process exits, we should
		   also exit. */
		do_restart = 0;
		do_exit = 1;
	}
}


static void send_term_to_child(void)
{
	if (child_pid <= 0) {
		return;
	}
	logparent(CM_INFO, "Sending SIGTERM\n");
	kill(child_pid, SIGTERM);
}


static void send_kill_to_child(void)
{
	if (child_pid <= 0) {
		return;
	}
	logparent(CM_INFO, "Sending SIGKILL\n");
	kill(child_pid, SIGKILL);
}


static void handle_int_signal(void)
{
	send_int_to_child();
}


/**
 * Pass SIGTERM to the child and exit.
 */
static void handle_term_signal(void)
{
	if (child_pid <= 0) {
		logparent(CM_INFO, "exiting on SIGTERM\n");
		exit(1);
	}

	logparent(CM_INFO, "passing SIGTERM to %s[%d]\n",
		  child_args[0], child_pid);
	kill(child_pid, SIGTERM);
	do_restart = 0;
	do_exit = 1;
}


static void stop_monitoring(const char *reason)
{
	logparent(CM_INFO, "%s: I will not monitor %s\n",
		  reason, child_args[0]);
	do_restart = 0;
}


static void handle_usr1_signal(void)
{
	stop_monitoring("SIGUSR1");
}


static void start_monitoring(const char *reason)
{
	logparent(CM_INFO, "%s: I will monitor %s again\n",
		  reason, child_args[0]);
	do_restart = 1;
	child_wait_time = min_child_wait_time;
	if (child_pid <= 0) {
		start_child();
	}
}


static void handle_usr2_signal(void)
{
	start_monitoring("SIGUSR2");
}


/**
 * Fork/exec the child process.
 */
static void start_child(void)
{
	pid_t pid;
	int forkpty_errno;

	logparent(CM_INFO, "starting %s\n", child_args[0]);

	pid = forkpty(&pty_fd, NULL, NULL, NULL);
	forkpty_errno = errno;

	if (-1 == pid) {
		child_pid = -1;
		logparent(CM_ERROR, "cannot fork: %s\n",
			  strerror(forkpty_errno));
		child_wait_time = 60;
		return;
	} else if (0 != pid) {
		/* parent */
		/* logparent(CM_INFO, "after forkpty, pty_fd==%d\n", pty_fd); */
		child_pid = pid;
		set_child_log_pid(child_pid);
		fcntl(pty_fd, F_SETFL, O_NONBLOCK);
		return;
	}

	/* Child */
	close(signal_command_pipe[0]);
	close(signal_command_pipe[1]);
	if (-1 != command_fifo_fd) {
		close(command_fifo_fd);
	}
	if (-1 != command_fifo_write_fd) {
		close(command_fifo_write_fd);
	}
	setup_env();
	/* Set gid before uid, so that setting gid does not fail if we're no
	   longer root. */
	if (child_groupname && setgid(child_gid)) {
		logparent(CM_ERROR, "cannot setgid(%d): %s\n",
			  (int)child_gid, strerror(errno));
		exit(99);
	}
	if (child_uid && setuid(child_uid)) {
		logparent(CM_ERROR, "cannot setuid(%d): %s\n",
			  (int)child_uid, strerror(errno));
		exit(99);
	}
	if (child_dir && chdir(child_dir)) {
		logparent(CM_ERROR, "cannot chdir() to %s: %s\n",
			  child_dir, strerror(errno));
		exit(99);
	}
	if (startup_sh) {
		int ret = system(startup_sh);
		if (WIFSIGNALED(ret) && (WTERMSIG(ret) == SIGINT || WTERMSIG(ret) == SIGQUIT))
			exit(99);
	}
	if (execv(child_args[0], child_args)) {
		logparent(CM_ERROR, "cannot exec %s: %s\n",
			  child_args[0], strerror(errno));
		exit(99);
	}
}


static void make_signal_command_pipe(void)
{
	int ret;

	ret = pipe(signal_command_pipe);
	if (-1 == ret) {
		fprintf(stderr, "%s: cannot make pipe: %s\n",
			get_parent_log_name(), strerror(errno));
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


static void maybe_create_pid_file(void)
{
	if (pid_file) {
		FILE *pid_file_file;

		pid_file_file = fopen(pid_file, "w");
		if (! pid_file_file) {
			logparent(CM_ERROR,
				  "cannot open %s for writing: %s\n",
				  pid_file, strerror(errno));
			logparent(CM_ERROR, "exiting\n");
			exit(1);
		}
		if (0 > fprintf(pid_file_file, "%d\n", (int)getpid())) {
			logparent(CM_ERROR,
				  "cannot write to %s: %s\n",
				  pid_file, strerror(errno));
			logparent(CM_ERROR, "exiting\n");
			exit(1);
		}
		fclose(pid_file_file);
		atexit(delete_pid_file);
	}
}


static void delete_pid_file(void)
{
	if (! pid_file) {
		return;
	}
	if (unlink(pid_file)) {
		logparent(CM_WARN, "cannot unlink %s: %s\n",
			  pid_file, strerror(errno));
	}
}


/**
 * Create the command fifo if necessary and possible, then open it for reading.
 */
static void make_command_fifo(void)
{
	int stat_ret, mkfifo_ret;
	struct stat statbuf;

	if (! command_fifo_name)
		return;

	stat_ret = stat(command_fifo_name, &statbuf);
	if (-1 == stat_ret) {
		if (errno != ENOENT) {
			const char *er = strerror(errno);
			fprintf(stderr,
				"%s: cannot stat %s: %s\n",
				get_parent_log_name(), command_fifo_name, er);
			exit(1);
		}
		mkfifo_ret = mkfifo(command_fifo_name, 0610);
		if (mkfifo_ret) {
			const char *er = strerror(errno);
			fprintf(stderr,
				"%s: cannot make fifo %s: %s\n",
				get_parent_log_name(), command_fifo_name, er);
			exit(1);
		}
	}
	if (! stat_ret && ! S_ISFIFO(statbuf.st_mode)) {
		/* The path exists but is not a fifo.  Bail out. */
		fprintf(stderr,
			"%s: %s exists but is not a fifo\n",
			get_parent_log_name(), command_fifo_name);
		exit(1);
	}

	/* When we get here, the fifo exists. */
	command_fifo_fd = open(command_fifo_name, O_RDONLY|O_NONBLOCK);
	if (-1 == command_fifo_fd) {
		const char *er = strerror(errno);
		fprintf(stderr, "%s: cannot open %s: %s\n",
			get_parent_log_name(), command_fifo_name, er);
		exit(1);
	}

	/* Also open the fifo for writing so we never get eof returned by read
	   from the fifo.  O_RDWR should work instead of opening the fifo
	   twice, but POSIX says that O_RDWR is undefined when used with a
	   fifo. */
	command_fifo_write_fd = open(command_fifo_name, O_WRONLY);
	if (-1 == command_fifo_write_fd) {
		const char *er = strerror(errno);
		fprintf(stderr, "%s: cannot open %s for writing: %s\n",
			get_parent_log_name(), command_fifo_name, er);
		exit(1);
	}

	/* logparent(CM_INFO, "command fifo %d\n", command_fifo_fd); */
}


/**
 * Send a command to a running process-monitor.
 *
 * We write a single byte representing the command into the command fifo.
 */
static void send_command(void)
{
	struct pmCommand *pmc = pmCommands;
	char c = '\0';
	int ret;

	for (pmc=pmCommands; pmc->command; pmc++) {
		if (!strcmp(pmc->command, command_name)) {
			c = pmc->c;
			break;
		}
	}
	if (! c) {
		fprintf(stderr,
			"%s: unknown command %s\n",
			get_parent_log_name(), command_name);
		exit(1);
	}
	/* Find the command fifo to send to. */
	if (! command_fifo_name) {
		fprintf(stderr,
			"%s: need a command pipe name\n",
			get_parent_log_name());
		exit(1);
	}
	command_fifo_fd = open(command_fifo_name, O_WRONLY|O_NONBLOCK);
	if (-1 == command_fifo_fd) {
		int saved_errno = errno;
		const char *er = strerror(saved_errno);
		fprintf(stderr, "%s: cannot open %s: %s\n",
			get_parent_log_name(), command_fifo_name, er);
		if (saved_errno == ENXIO) {
			fprintf(stderr, "  Is there a reader process?\n");
		}
		exit(1);
	}
	ret = write(command_fifo_fd, &c, 1);
	if (-1 == ret) {
		const char *er = strerror(errno);
		fprintf(stderr, "%s: cannot write to %s: %s\n",
			get_parent_log_name(), command_fifo_name, er);
		exit(1);
	}
	exit(0);
}
