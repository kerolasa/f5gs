/* This is F5 Graceful Scaling helper daemon.
 *
 * The f5gs has BSD 2-clause license which also known as "Simplified
 * BSD License" or "FreeBSD License".
 *
 * Copyright 2013- Sami Kerola. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the
 *       distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR AND CONTRIBUTORS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of Sami Kerola.
 */

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/wait.h>

#include "close-stream.h"
#include "closeout.h"
#include "progname.h"
#include "xalloc.h"

#ifdef USE_SYSTEMD
# include <systemd/sd-daemon.h>
# include <systemd/sd-journal.h>
#endif

#include "f5gs.h"

/* global variables */
static struct runtime_config rtc;
sigset_t set;

/* keep functions in the order that allows skipping the function
 * definition lines */
static void __attribute__ ((__noreturn__))
    usage(FILE *out)
{
	fputs("\nUsage:\n", out);
	fprintf(out, " %s [options]\n", program_invocation_short_name);
	fputs("\nOptions:\n", out);
	fputs(" -d, --disable        disable service\n", out);
	fputs(" -m, --maintenance    disallow new connections\n", out);
	fputs(" -e, --enable         enable service\n", out);
	fputs("\n", out);
	fputs(" -s, --server         start up health check daemon\n", out);
	fputs(" -l, --listen <addr>  ip address daemon will listen\n", out);
	fprintf(out, " -p, --port <port>    health check tcp port (default: %s)\n", PORT_NUM);
	fprintf(out, "     --state <dir>    path of the state dir (default: %s)\n", F5GS_RUNDIR);
	fputs("     --no-scripts     do not run pre or post scripts\n", out);
	fputs("     --foreground     do not run as daemon process\n", out);
	fputs("\n", out);
	fputs(" -h, --help           display this help and exit\n", out);
	fputs(" -V, --version        output version information and exit\n", out);
	fputs("\n", out);
	fprintf(out, "For more details see %s(8).\n", PACKAGE_NAME);
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void __attribute__ ((__noreturn__))
    faillog(char *msg)
{
#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=%s", msg,
			"STRERROR=%s", strerror(errno),
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR), "PRIORITY=3", NULL);
#else
	syslog(LOG_ERR, "%s: %s", msg, strerror(errno));
#endif
	exit(EXIT_FAILURE);
}

static void __attribute__ ((__noreturn__))
    *handle_request(void *voidsocket)
{
	int sock = *(int *)voidsocket;
	char in_buf[IGNORE_BYTES];
	struct timeval timeout;

	pthread_detach(pthread_self());
	pthread_rwlock_rdlock(&(rtc.lock));
	send(sock, state_messages[rtc.msg_type], rtc.msg_len, 0);
	pthread_rwlock_unlock(&(rtc.lock));
	/* let the client send, and ignore */
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)))
		err(EXIT_FAILURE, "setsockopt failed\n");
	recv(sock, in_buf, sizeof(in_buf), 0);
	close(sock);
	free(voidsocket);
	pthread_exit(NULL);
}

static char *construct_pidfile(struct runtime_config *rtc)
{
	char *path;
	void *p;
	char s[INET6_ADDRSTRLEN];
	int ret;

	inet_ntop(rtc->res->ai_family, rtc->res->ai_addr->sa_data, s, sizeof(s));
	switch (rtc->res->ai_family) {
	case AF_INET:
		p = &((struct sockaddr_in *)rtc->res->ai_addr)->sin_addr;
		break;
	case AF_INET6:
		p = &((struct sockaddr_in6 *)rtc->res->ai_addr)->sin6_addr;
		break;
	}
	inet_ntop(rtc->res->ai_family, p, s, sizeof(s));
	ret = asprintf(&path, "%s/%s:%d", rtc->statedir, s,
		       ntohs(((struct sockaddr_in *)(rtc->res->ai_addr))->sin_port));
	if (ret < 0)
		faillog("cannot allocate memory");
	return path;
}

static int update_pid_file(struct runtime_config *rtc)
{
	FILE *fd;

	if (access(rtc->statedir, W_OK))
		if (mkdir(rtc->statedir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
			return 1;
	if (!(fd = fopen(rtc->pidfile, "w"))) {
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=could not open file",
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"PID_FILE=%s", rtc->pidfile, "STRERROR=%s", strerror(errno), "PRIORITY=3", NULL);
#else
		syslog(LOG_ERR, "could not open file: %s: %s", rtc->pidfile, strerror(errno));
#endif
		return 1;
	}
	fprintf(fd, "%u %d", getpid(), rtc->msg_type);
	if (close_stream(fd))
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=close failed",
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"PID_FILE=%s", rtc->pidfile, "STRERROR=%s", strerror(errno), "PRIORITY=3", NULL);
#else
		syslog(LOG_ERR, "close failed: %s: %s", rtc->pidfile, strerror(errno));
#endif
	return 0;
}

static void catch_signals(int signal)
{
	if (pthread_rwlock_wrlock(&rtc.lock)) {
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=could not get lock",
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"STRERROR=%s", strerror(errno), "PRIORITY=3", NULL);
#endif
		return;
	}
	switch (signal) {
	case SIG_DISABLE:
		rtc.msg_type = STATE_DISABLE;
		break;
	case SIG_MAINTENANCE:
		rtc.msg_type = STATE_MAINTENANCE;
		break;
	case SIG_ENABLE:
		rtc.msg_type = STATE_ENABLE;
		break;
	default:
		/* should be impossible to reach */
		abort();
	}
	rtc.msg_len = strlen(state_messages[rtc.msg_type]);
	pthread_rwlock_unlock(&(rtc.lock));
	update_pid_file(&rtc);
#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=signal received",
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE),
			"NEW_STATE=%s", state_messages[rtc.msg_type], "PRIORITY=6", NULL);
#else
	syslog(LOG_INFO, "signal received, state is %s", state_messages[rtc.msg_type]);
#endif
}

static void read_status_from_file(struct runtime_config *rtc)
{
	FILE *pidfd;
	int ignored;

	if (!(pidfd = fopen(rtc->pidfile, "r")))
		return;
	fscanf(pidfd, "%d %d", &ignored, &(rtc->msg_type));
	switch (rtc->msg_type) {
	case STATE_DISABLE:
	case STATE_MAINTENANCE:
	case STATE_ENABLE:
	case STATE_UNKNOWN:
		break;
	default:
		rtc->msg_type = STATE_UNKNOWN;
	}
	rtc->msg_len = strlen(state_messages[rtc->msg_type]);
	fclose(pidfd);
}

static void daemonize(void)
{
	int fd;

	switch (fork()) {
	case -1:
		err(EXIT_FAILURE, "cannot fork");
	case 0:
		break;
	default:
		_exit(EXIT_SUCCESS);
	}
	if (!setsid())
		err(EXIT_FAILURE, "cannot setsid");
	chdir("/");
	if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}
}

static void *signal_handler_thread(void *arg)
{
	sigset_t *set = arg;
	int sig;

	while (1) {
		if (sigwait(set, &sig))
			stop_server(0);
		switch (sig) {
		case SIG_DISABLE:
		case SIG_MAINTENANCE:
		case SIG_ENABLE:
			catch_signals(sig);
			break;
		default:
			printf("got stop\n");
			stop_server(0);
		}
	}
	return NULL;
}

static void setup_signal_handling(void)
{
	pthread_t thread;

	sigemptyset(&set);
	sigaddset(&set, SIG_DISABLE);
	sigaddset(&set, SIG_MAINTENANCE);
	sigaddset(&set, SIG_ENABLE);

	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);

	if (pthread_sigmask(SIG_BLOCK, &set, NULL))
		err(EXIT_FAILURE, "cannot set signal handler");
	if (pthread_create(&thread, NULL, &signal_handler_thread, (void *)&set))
		err(EXIT_FAILURE, "could not set start signal handler thread");
}

static void __attribute__ ((__noreturn__))
    stop_server(int sig __attribute__ ((__unused__)))
{
	pthread_rwlock_destroy(&(rtc.lock));
	freeaddrinfo(rtc.res);
	close(rtc.server_s);

#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=service stopped", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START),
			"PRIORITY=6", NULL);
#else
	syslog(LOG_INFO, "stopped");
	closelog();
#endif
	_exit(EXIT_SUCCESS);
}

static void run_server(struct runtime_config *rtc)
{
	struct sockaddr_in client_addr;
	socklen_t addr_len;
	pthread_attr_t attr;
#ifdef USE_SYSTEMD
	int ret;

	ret = sd_listen_fds(0);
	if (1 < ret)
		faillog("no or too many file descriptors received");
	else if (ret == 1)
		rtc->server_s = SD_LISTEN_FDS_START + 0;
	else {
#else
	{
#endif
		int on = 1;
		if (!(rtc->server_s = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol)))
			err(EXIT_FAILURE, "cannot create socket");
		if (setsockopt(rtc->server_s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
			err(EXIT_FAILURE, "cannot set socket options");
		if (bind(rtc->server_s, rtc->res->ai_addr, rtc->res->ai_addrlen))
			err(EXIT_FAILURE, "unable to bind");
		if (listen(rtc->server_s, SOMAXCONN))
			err(EXIT_FAILURE, "unable to listen");
	}
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (pthread_rwlock_init(&(rtc->lock), NULL))
		err(EXIT_FAILURE, "cannot init read-write lock");

	if (!rtc->run_foreground)
		daemonize();

	setup_signal_handling();

	if (rtc->msg_type == STATE_UNKNOWN)
		read_status_from_file(rtc);
		if (update_pid_file(rtc))
			faillog("cannot write pid file");
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=service started",
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START),
			"STATE=%s", state_messages[rtc->msg_type], "PRIORITY=6", NULL);
	sd_notify(0, "READY=1");
#else
	syslog(LOG_INFO, "started in state %s", state_messages[rtc->msg_type]);
#endif

	while (1) {
		pthread_t thread;
		int *newsock;

		addr_len = sizeof(client_addr);
		newsock = xmalloc(sizeof(int));
		*newsock = accept(rtc->server_s, (struct sockaddr *)&client_addr, &addr_len);
		pthread_create(&thread, NULL, handle_request, newsock);
	}
}

static int run_script(struct runtime_config *rtc, char *script)
{
	pid_t child;
	int status;

	child = fork();
	if (0 <= child) {
		if (child == 0) {
			setuid(geteuid());
#ifdef HAVE_CLEARENV
			clearenv();
#else
			environ = NULL;
#endif
			setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 1);
			return execv(script, rtc->argv);
		} else {
			wait(&status);
			return WEXITSTATUS(status);
		}
	}
	warn("running %s failed", script);
	return 1;
}

static int change_state(struct runtime_config *rtc, pid_t pid)
{
	int ret = 0;
	if (rtc->run_scripts && !access(F5GS_PRE, X_OK))
		ret = run_script(rtc, F5GS_PRE);
	if (!ret)
		ret = kill(pid, rtc->send_signal);
	if (rtc->run_scripts && !ret && !access(F5GS_POST, X_OK))
		ret = run_script(rtc, F5GS_POST);
	return ret;
}

static char *get_server_status(struct runtime_config *rtc)
{
	int sfd;
	static char buf[12] = { 0 };	/* 'maintenance' is the longest reply. */
	if (!(sfd = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol)))
		err(EXIT_FAILURE, "cannot create socket");
	if (connect(sfd, rtc->res->ai_addr, rtc->res->ai_addrlen))
		err(EXIT_FAILURE, "cannot connect");
	read(sfd, buf, 12);
	return buf;
}

static int set_server_status(struct runtime_config *rtc)
{
	char *eptr;
	pid_t pid;
	FILE *pidfd;

	if (!(pidfd = fopen(rtc->pidfile, "r")))
		err(EXIT_FAILURE, "cannot open pid file: %s", rtc->pidfile);
	fscanf(pidfd, "%d", &pid);
#ifndef USE_SYSTEMD
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
#endif
	if (close_stream(pidfd))
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=close failed",
				"PID_FILE=%s", rtc->pidfile,
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"STRERROR=%s", strerror(errno), "PRIORITY=3", NULL);
#else
		syslog(LOG_ERR, "close failed: %s: %s", rtc->pidfile, strerror(errno));
#endif
	if (change_state(rtc, pid)) {
		if (errno == 0)
			warnx("execution of %s failed", F5GS_PRE);
		else
			warn("sending signal failed");
		return EXIT_FAILURE;
	}
	eptr = getenv("USER");
	if (eptr != NULL)
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=signal was sent",
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE),
				"USER=%s", eptr, "PRIORITY=6", NULL);
#else
		syslog(LOG_INFO, "signal was sent by USER: %s", eptr);
#endif
	eptr = getenv("SUDO_USER");
	if (eptr != NULL)
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=signal was sent",
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE),
				"SUDO_USER=%s", eptr, "PRIORITY=6", NULL);
#else
		syslog(LOG_INFO, "signal was sent by SUDO_USER: %s", eptr);
#endif
#ifndef USE_SYSTEMD
	closelog();
#endif
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	int c, server = 0, retval = EXIT_SUCCESS;
	char *listen = NULL, *port = PORT_NUM;
	struct addrinfo hints;
	int e;
	enum {
		STATEDIR_OPT = CHAR_MAX + 1,
		NO_SCRIPTS_OPT,
		FOREGROUND_OPT
	};

	static const struct option longopts[] = {
		{"disable", no_argument, NULL, 'd'},
		{"maintenance", no_argument, NULL, 'm'},
		{"enable", no_argument, NULL, 'e'},
		{"server", no_argument, NULL, 's'},
		{"listen", required_argument, NULL, 'l'},
		{"port", required_argument, NULL, 'p'},
		{"state", required_argument, NULL, STATEDIR_OPT},
		{"no-scripts", no_argument, NULL, NO_SCRIPTS_OPT},
		{"foreground", no_argument, NULL, FOREGROUND_OPT},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	set_program_name(argv[0]);
	atexit(close_stdout);

	memset(&rtc, 0, sizeof(struct runtime_config));
	rtc.argv = argv;
	rtc.statedir = F5GS_RUNDIR;
	rtc.run_scripts = 1;

	while ((c = getopt_long(argc, argv, "dmesl:p:Vh", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			rtc.send_signal = state_signals[STATE_DISABLE];
			break;
		case 'm':
			rtc.send_signal = state_signals[STATE_MAINTENANCE];
			break;
		case 'e':
			rtc.send_signal = state_signals[STATE_ENABLE];
			break;
		case 's':
			server = 1;
			break;
		case 'l':
			listen = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case STATEDIR_OPT:
			rtc.statedir = optarg;
			break;
		case NO_SCRIPTS_OPT:
			rtc.run_scripts = 0;
			break;
		case FOREGROUND_OPT:
			rtc.run_foreground = 1;
			break;
		case 'V':
			printf("%s version %s", PACKAGE_NAME, PACKAGE_VERSION);
#ifdef USE_SYSTEMD
			puts(" with systemd support");
#else
			putc('\n', stdout);
#endif
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	e = getaddrinfo(listen, port, &hints, &(rtc.res));
	if (e)
		errx(EXIT_FAILURE, "getaddrinfo: %s port %s: %s", listen, port, gai_strerror(e));
	rtc.pidfile = construct_pidfile(&rtc);

	if (server) {
		if (rtc.send_signal) {
			rtc.msg_type = signal_state[rtc.send_signal];
			rtc.msg_len = strlen(state_messages[rtc.msg_type]);
			if (update_pid_file(&rtc))
				err(EXIT_FAILURE, "cannot write pid file");
		} else {
			rtc.msg_type = STATE_UNKNOWN;
			rtc.msg_len = strlen(state_messages[STATE_UNKNOWN]);
		}
		run_server(&rtc);
	} else if (rtc.send_signal)
		retval = set_server_status(&rtc);

	printf("current status is: %s\n", get_server_status(&rtc));
	freeaddrinfo(rtc.res);

	return retval;
}
