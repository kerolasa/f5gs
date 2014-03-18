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
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include "close-stream.h"
#include "closeout.h"
#include "progname.h"
#include "xalloc.h"

#if HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#elif HAVE_WS2TCPIP_H
# include <ws2tcpip.h>
#endif

#ifdef USE_SYSTEMD
# include <systemd/sd-daemon.h>
# include <systemd/sd-journal.h>
#endif

#ifdef HAVE_SIGNALFD
# include <sys/signalfd.h>
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
	fputs(" -q, --quiet          do not print status, use exit values for states\n", out);
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
    faillog(struct runtime_config *rtc, char *msg, ...)
{
	va_list args;
	char *s;

	va_start(args, msg);
	if (vasprintf(&s, msg, args) < 0)
		goto fail;
	if (rtc->run_foreground) {
		err(EXIT_FAILURE, "%s", s);
	}
#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=%s", s,
			"STRERROR=%s", strerror(errno),
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR), "PRIORITY=%d", LOG_ERR, NULL);
#else
	syslog(LOG_ERR, "%s: %s", s, strerror(errno));
#endif
 fail:
	free(s);
	va_end(args);
	exit(EXIT_FAILURE);
}

static void __attribute__ ((__noreturn__))
    *handle_request(void *voidsocket)
{
	int sock = *(int *)voidsocket;
	char in_buf[IGNORE_BYTES];
	struct timeval timeout;

	pthread_detach(pthread_self());
	pthread_rwlock_rdlock(&rtc.lock);
	send(sock, state_message[rtc.state_code], rtc.message_lenght, 0);
	pthread_rwlock_unlock(&rtc.lock);
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

static char *construct_pid_file(struct runtime_config *rtc)
{
	char *path;
	void *p;
	char separator[1] = { '\0' }, *last_slash;
	char s[INET6_ADDRSTRLEN];
	int ret;

	last_slash = strrchr(rtc->state_dir, '/');
	if (*(last_slash + 1) != '\0' && *(last_slash + 1) != '/')
		separator[0] = '/';
	inet_ntop(rtc->res->ai_family, rtc->res->ai_addr->sa_data, s, sizeof(s));
	switch (rtc->res->ai_family) {
	case AF_INET:
		p = &((struct sockaddr_in *)rtc->res->ai_addr)->sin_addr;
		break;
	case AF_INET6:
		p = &((struct sockaddr_in6 *)rtc->res->ai_addr)->sin6_addr;
		break;
	default:
		abort();
	}
	inet_ntop(rtc->res->ai_family, p, s, sizeof(s));
	ret = asprintf(&path, "%s%s%s:%d", rtc->state_dir, separator, s,
		       ntohs(((struct sockaddr_in *)(rtc->res->ai_addr))->sin_port));
	if (ret < 0)
		faillog(rtc, "cannot allocate memory");
	return path;
}

static int update_pid_file(struct runtime_config *rtc)
{
	FILE *fd;

	if (access(rtc->state_dir, W_OK))
		if (mkdir(rtc->state_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
			return 1;
	if (!(fd = fopen(rtc->pid_file, "w"))) {
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=could not open file %s", rtc->pid_file,
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"STRERROR=%s", strerror(errno), "PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "could not open file: %s: %s", rtc->pid_file, strerror(errno));
#endif
		return 1;
	}
	fprintf(fd, "%u %d %d", getpid(), rtc->state_code, STATE_FILE_VERSION);
	if (close_stream(fd))
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=closing %s failed", rtc->pid_file,
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"STRERROR=%s", strerror(errno), "PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "close failed: %s: %s", rtc->pid_file, strerror(errno));
#endif
	return 0;
}

#ifdef HAVE_SIGNALFD
static void catch_signals(struct signalfd_siginfo *info)
#else
static void catch_signals(int signal)
#endif
{
	int old_state;

	if (pthread_rwlock_wrlock(&rtc.lock)) {
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=could not get state change lock",
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"STRERROR=%s", strerror(errno), "PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "could not get state change lock");
#endif
		return;
	}
	old_state = rtc.state_code;
#ifdef HAVE_SIGNALFD
	switch (info->ssi_signo) {
#else
	switch (signal) {
#endif

	case SIG_DISABLE:
		rtc.state_code = STATE_DISABLE;
		break;
	case SIG_MAINTENANCE:
		rtc.state_code = STATE_MAINTENANCE;
		break;
	case SIG_ENABLE:
		rtc.state_code = STATE_ENABLE;
		break;
	default:
		/* should be impossible to reach */
		abort();
	}
	rtc.message_lenght = strlen(state_message[rtc.state_code]);
	pthread_rwlock_unlock(&rtc.lock);
	update_pid_file(&rtc);
#ifdef HAVE_SIGNALFD
# ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=state change %s -> %s", state_message[old_state], state_message[rtc.state_code],
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE), "PRIORITY=%d", LOG_INFO,
			"SIGNAL_SENDER_UID=%" PRIu32, info->ssi_uid, "SIGNAL_SENDER_PID=%" PRIu32, info->ssi_pid, NULL);
# else
	syslog(LOG_INFO, "signal received from uid: %" PRIu32 " pid: %" PRIu32 ", state %s -> %s",
	       info->ssi_uid, info->ssi_pid, state_message[old_state], state_message[rtc.state_code]);
# endif
#else	/* HAVE_SIGNALFD */
# ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=state change %s -> %s", state_message[old_state], state_message[rtc.state_code],
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE), "PRIORITY=%d", LOG_INFO, NULL);
# else
	syslog(LOG_INFO, "signal received" ", state %s -> %s", state_message[old_state], state_message[rtc.state_code]);
# endif
#endif	/* HAVE_SIGNALFD */
}

static void read_status_from_file(struct runtime_config *rtc)
{
	FILE *pidfd;
	int ignored, version;

	if (!(pidfd = fopen(rtc->pid_file, "r")))
		return;
	if (fscanf(pidfd, "%d %d %d", &ignored, &(rtc->state_code), &version) != 3)
		goto err;
	if (version != STATE_FILE_VERSION)
		goto err;
	switch (rtc->state_code) {
	case STATE_DISABLE:
	case STATE_MAINTENANCE:
	case STATE_ENABLE:
	case STATE_UNKNOWN:
		break;
	default:
 err:
		rtc->state_code = STATE_UNKNOWN;
	}
	rtc->message_lenght = strlen(state_message[rtc->state_code]);
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
	if (chdir("/"))
		err(EXIT_FAILURE, "cannot chdir");
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
#ifdef HAVE_SIGNALFD
	int fd;
	struct pollfd pfd[1];
	struct signalfd_siginfo info;

	fd = signalfd(-1, set, 0);
	pfd[0].fd = fd;
	pfd[0].events = POLLIN | POLLERR | POLLHUP;
	if (fd < 0)
		err(EXIT_FAILURE, "signalfd");
	while (1) {
		ssize_t sz;
		if (poll(pfd, 1, -1) < 0) {
			warn("signalfd poll failed");
			stop_server(0);
		}
		sz = read(fd, &info, sizeof(info));
		if (sz != sizeof(info)) {
			warn("read from signalfd failed");
			stop_server(0);
		}
		switch (info.ssi_signo) {
#else /* HAVE_SIGNALFD */
	int sig;

	while (1) {
		if (sigwait(set, &sig))
			stop_server(0);
		switch (sig) {
#endif /* HAVE_SIGNALFD */
		case SIG_DISABLE:
		case SIG_MAINTENANCE:
		case SIG_ENABLE:
#ifdef HAVE_SIGNALFD
			catch_signals(&info);
#else
			catch_signals(sig);
#endif
			break;
		default:
			stop_server(0);
		}
	}
	return NULL;
}

static void setup_signal_handling(void)
{
	pthread_t thread;

	sigemptyset(&set);	/* sigset_t set is global variable. */
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
	free(rtc.pid_file);
	close(rtc.server_socket);

#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=service stopped", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START),
			"PRIORITY=%d", LOG_INFO, NULL);
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
		faillog(rtc, "no or too many file descriptors received");
	else if (ret == 1)
		rtc->server_socket = SD_LISTEN_FDS_START + 0;
	else {
#else
	{
#endif
		int on = 1;
		if (!(rtc->server_socket = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol)))
			err(EXIT_FAILURE, "cannot create socket");
		if (setsockopt(rtc->server_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
			err(EXIT_FAILURE, "cannot set socket options");
		if (bind(rtc->server_socket, rtc->res->ai_addr, rtc->res->ai_addrlen))
			err(EXIT_FAILURE, "unable to bind");
		if (listen(rtc->server_socket, SOMAXCONN))
			err(EXIT_FAILURE, "unable to listen");
	}
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (pthread_rwlock_init(&rtc->lock, NULL))
		err(EXIT_FAILURE, "cannot init read-write lock");

	if (!rtc->run_foreground)
		daemonize();

	setup_signal_handling();

	if (rtc->state_code == STATE_UNKNOWN)
		read_status_from_file(rtc);
	if (update_pid_file(rtc))
		faillog(rtc, "cannot write pid file %s", rtc->pid_file);
#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=service started",
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START),
			"STATE=%s", state_message[rtc->state_code], "PRIORITY=%d", LOG_INFO, NULL);
	sd_notify(0, "READY=1");
#else
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "started in state %s", state_message[rtc->state_code]);
#endif

	while (1) {
		pthread_t thread;
		int *new_socket;

		addr_len = sizeof(client_addr);
		new_socket = xmalloc(sizeof(int));
		*new_socket = accept(rtc->server_socket, (struct sockaddr *)&client_addr, &addr_len);
		pthread_create(&thread, NULL, handle_request, new_socket);
	}
}

static int run_script(struct runtime_config *rtc, char *script)
{
	pid_t child;
	int status = 0;

	if (rtc->no_scripts || access(script, X_OK))
		return 0;
	child = fork();
	if (child < 0)
		errx(EXIT_FAILURE, "running %s failed", script);
	if (child == 0) {
		if (setuid(geteuid()))
			err(EXIT_FAILURE, "setuid() failed");
#ifdef HAVE_CLEARENV
		clearenv();
#else
		environ = NULL;
#endif
		setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 1);
		exit(execv(script, rtc->argv));
	}
	while (child) {
		if (waitpid(child, &status, WUNTRACED | WCONTINUED) < 0)
			err(EXIT_FAILURE, "waitpid() failed");
		if (WIFSIGNALED(status))
			errx(EXIT_FAILURE, "script %s killed (signal %d)", script, WTERMSIG(status));
		else if (WIFEXITED(status)) {
			if (WEXITSTATUS(status))
				warnx("execution of %s failed (return: %d)", script, WEXITSTATUS(status));
			return WEXITSTATUS(status);
		}
	}
	abort();
}

static int change_state(struct runtime_config *rtc, pid_t pid)
{
	if (run_script(rtc, F5GS_PRE))
		return 1;
	if (kill(pid, rtc->client_signal))
		err(EXIT_FAILURE, "sending signal failed");
	run_script(rtc, F5GS_POST);
	return 0;
}

static char *get_server_status(struct runtime_config *rtc)
{
	int sfd;
	static char buf[sizeof(state_message)];

	if (!(sfd = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol))) {
		if (rtc->quiet)
			exit(STATE_UNKNOWN);
		else
			err(EXIT_FAILURE, "cannot create socket");
	}
	if (connect(sfd, rtc->res->ai_addr, rtc->res->ai_addrlen)) {
		if (rtc->quiet)
			exit(STATE_UNKNOWN);
		else
			err(EXIT_FAILURE, "cannot connect");
	}
	if (read(sfd, buf, sizeof(state_message)) < 0)
		err(EXIT_FAILURE, "reading socket failed");
	return buf;
}

static int set_server_status(struct runtime_config *rtc)
{
	char *username, *sudo_user;
	pid_t pid;
	FILE *pidfd;

	if (!(pidfd = fopen(rtc->pid_file, "r")))
		err(EXIT_FAILURE, "cannot open pid file: %s", rtc->pid_file);
	if (fscanf(pidfd, "%d", &pid) != 1)
		err(EXIT_FAILURE, "broken pid file: %s", rtc->pid_file);
#ifndef USE_SYSTEMD
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
#endif
	if (close_stream(pidfd))
#ifdef USE_SYSTEMD
		sd_journal_send("MESSAGE=closing %s failed", rtc->pid_file,
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"STRERROR=%s", strerror(errno), "PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "close failed: %s: %s", rtc->pid_file, strerror(errno));
#endif
	if (change_state(rtc, pid))
		errx(EXIT_FAILURE, "aborting action, consider running with --no-scripts");
	username = getenv("USER");
	sudo_user = getenv("SUDO_USER");
#ifdef USE_SYSTEMD
	sd_journal_send("MESSAGE=signal was sent",
			"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE),
			"USER=%s", username, "SUDO_USER=%s", sudo_user,
			"PRIORITY=%d", LOG_INFO, NULL);
#else
	syslog(LOG_INFO, "signal was sent by USER: %s SUDO_USER: %s", username, sudo_user);
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
		{"quiet", no_argument, NULL, 'q'},
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
	rtc.state_dir = F5GS_RUNDIR;

	while ((c = getopt_long(argc, argv, "dmesl:p:qVh", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			rtc.client_signal = state_signal[STATE_DISABLE];
			break;
		case 'm':
			rtc.client_signal = state_signal[STATE_MAINTENANCE];
			break;
		case 'e':
			rtc.client_signal = state_signal[STATE_ENABLE];
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
			rtc.state_dir = optarg;
			break;
		case 'q':
			rtc.quiet = 1;
			break;
		case NO_SCRIPTS_OPT:
			rtc.no_scripts = 1;
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
	if (0 < argc - optind)
		errx(EXIT_FAILURE, "too many arguments");
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	e = getaddrinfo(listen, port, &hints, &rtc.res);
	if (e) {
		if (rtc.quiet)
			exit(STATE_UNKNOWN);
		else
			errx(EXIT_FAILURE, "getaddrinfo: %s port %s: %s", listen, port, gai_strerror(e));
	}
	rtc.pid_file = construct_pid_file(&rtc);

	if (server) {
		if (rtc.client_signal) {
			rtc.state_code = signal_state[rtc.client_signal];
			rtc.message_lenght = strlen(state_message[rtc.state_code]);
			if (update_pid_file(&rtc))
				err(EXIT_FAILURE, "cannot write pid file");
		} else {
			rtc.state_code = STATE_UNKNOWN;
			rtc.message_lenght = strlen(state_message[STATE_UNKNOWN]);
		}
		run_server(&rtc);
	} else if (rtc.client_signal)
		retval = set_server_status(&rtc);

	if (rtc.quiet) {
		char *s;
		int i;

		s = get_server_status(&rtc);
		for (i = 0; i <= STATE_UNKNOWN; i++)
			if (!strcmp(s, state_message[i]))
				break;
		retval = i;
	} else
		printf("current status is: %s\n", get_server_status(&rtc));
	freeaddrinfo(rtc.res);
	free(rtc.pid_file);

	return retval;
}
