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

#include "config.h"

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

#include "close-stream.h"
#include "closeout.h"
#include "progname.h"

#define IGNORE_BYTES	      256

enum {
	STATE_DISABLE,
	STATE_MAINTENANCE,
	STATE_ENABLE,
	STATE_UNKNOWN
};

/* Check get_server_status() is valid after changing message text(s). */
static const char *state_messages[] = {
	[STATE_DISABLE] = "disable",
	[STATE_MAINTENANCE] = "maintenance",
	[STATE_ENABLE] = "enable",
	[STATE_UNKNOWN] = "unknown"
};

static const int state_signals[] = {
	[STATE_DISABLE] = SIGUSR1,
	[STATE_MAINTENANCE] = SIGUSR2,
	[STATE_ENABLE] = SIGWINCH
};

struct runtime_config {
	struct addrinfo *res;
	int server_s;
	pthread_rwlock_t lock;
	int msg_type;
	size_t msg_len;
	char *statedir;
};
static struct runtime_config rtc;

static void __attribute__((__noreturn__))
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
	fputs(" -l, --listen <addr>  ip address deamon will listen\n", out);
	fprintf(out, " -p, --port <port>    health check tcp port (default: %s)\n", PORT_NUM);
	fprintf(out, "     --state <dir>    path of the state dir (default: %s)\n", F5GS_RUNDIR);
	fputs("\n", out);
	fputs(" -h, --help           display this help and exit\n", out);
	fputs(" -V, --version        output version information and exit\n", out);
	fputs("\n", out);
	fprintf(out, "For more details see %s(8).\n", PACKAGE_NAME);
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void __attribute__((__noreturn__))
    faillog(char *msg)
{
	syslog(LOG_ERR, "%s: %s", msg, strerror(errno));
	exit(EXIT_FAILURE);
}

static void *response_thread(void *arg)
{
	char in_buf[IGNORE_BYTES];
	int client_s = (*(int *)arg);

	if (pthread_rwlock_rdlock(&(rtc.lock))) {
		syslog(LOG_ERR, "could not get lock");
		return NULL;
	}
	send(client_s, state_messages[rtc.msg_type], rtc.msg_len, 0);
	pthread_rwlock_unlock(&(rtc.lock));
	/* let the client send, and ignore */
	recv(client_s, in_buf, IGNORE_BYTES, 0);
	close(client_s);
	pthread_exit(NULL);
	/* should be impossible to reach */
	return NULL;
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
	ret = asprintf(&path, "%s/%s:%d", rtc->statedir, s, ntohs(((struct sockaddr_in *)(rtc->res->ai_addr))->sin_port));
	if (ret < 0)
		faillog("cannot allocate memory");
	return path;
}

static int update_pid_file(struct runtime_config *rtc)
{
	FILE *fd;
	char *pidfile;

	if (access(rtc->statedir, W_OK))
		if (mkdir(rtc->statedir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
			return 1;
	pidfile = construct_pidfile(rtc);
	if (!(fd = fopen(pidfile, "w"))) {
		syslog(LOG_ERR, "could not open file: %s: %s", pidfile, strerror(errno));
		return 1;
	}
	fprintf(fd, "%u %d", getpid(), rtc->msg_type);
	fclose(fd);
	free(pidfile);
	return 0;
}

static void catch_signals(int signal)
{
	if (pthread_rwlock_wrlock(&rtc.lock)) {
		syslog(LOG_ERR, "could not get lock");
		return;
	}
	switch (signal) {
	case SIGUSR1:
		rtc.msg_type = STATE_DISABLE;
		break;
	case SIGUSR2:
		rtc.msg_type = STATE_MAINTENANCE;
		break;
	case SIGWINCH:
		rtc.msg_type = STATE_ENABLE;
		break;
	default:
		/* should be impossible to reach */
		abort();
	}
	rtc.msg_len = strlen(state_messages[rtc.msg_type]);
	pthread_rwlock_unlock(&(rtc.lock));
	update_pid_file(&rtc);
}

static int read_status_from_file(struct runtime_config *rtc)
{
	char *pid_file = construct_pidfile(rtc);
	FILE *pidfd;
	int ret;

	if (!(pidfd = fopen(pid_file, "r")))
		return 1;
	fscanf(pidfd, "%d %d", &ret, &(rtc->msg_type));
	switch (rtc->msg_type) {
	case STATE_DISABLE:
	case STATE_MAINTENANCE:
	case STATE_ENABLE:
		ret = 0;
		break;
	default:
		rtc->msg_type = STATE_UNKNOWN;
		ret = 1;
	}
	rtc->msg_len = strlen(state_messages[rtc->msg_type]);
	fclose(pidfd);
	return ret;
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

void stop_server(int sig __attribute__((__unused__)))
{
	pthread_rwlock_destroy(&(rtc.lock));
	close(rtc.server_s);
	syslog(LOG_INFO, "stopped");
	closelog();
}

static void run_server(struct runtime_config *rtc)
{
	struct sockaddr_in client_addr;
	socklen_t addr_len;
	unsigned int ids;
	pthread_attr_t attr;
	pthread_t threads;

	if (!(rtc->server_s = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol)))
		err(EXIT_FAILURE, "cannot create socket");
	if (bind(rtc->server_s, rtc->res->ai_addr, rtc->res->ai_addrlen))
		err(EXIT_FAILURE, "unable to bind");
	if (listen(rtc->server_s, SOMAXCONN))
		err(EXIT_FAILURE, "unable to listen");
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (pthread_rwlock_init(&(rtc->lock), NULL))
		err(EXIT_FAILURE, "cannot init read-write lock");

	daemonize();
	if (rtc->msg_type == STATE_UNKNOWN && read_status_from_file(rtc) && update_pid_file(rtc))
		faillog("cannot write pid file");
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	signal(SIGHUP, stop_server);
	signal(SIGINT, stop_server);
	signal(SIGTERM, stop_server);
	syslog(LOG_INFO, "started");

	while (1) {
		int client_s;
		addr_len = sizeof(client_addr);
		client_s = accept(rtc->server_s, (struct sockaddr *)&client_addr, &addr_len);

		if (client_s < 0)
			continue;
		else {
			ids = client_s;
			if (pthread_create(&threads, &attr, response_thread, &ids)) {
				syslog(LOG_ERR, "thread creation failed");
				continue;
			}
			pthread_join(threads, NULL);
		}
	}
}

char *get_server_status(struct runtime_config *rtc)
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

int main(int argc, char **argv)
{
	int c, server = 0, send_signal = 0;
	char *listen = NULL, *port = PORT_NUM;
	struct addrinfo hints;
	int e;
	enum {
		STATEDIR_OPT = CHAR_MAX + 1
	};

	static const struct option longopts[] = {
		{"disable", no_argument, NULL, 'd'},
		{"maintenance", no_argument, NULL, 'm'},
		{"enable", no_argument, NULL, 'e'},
		{"server", no_argument, NULL, 's'},
		{"listen", required_argument, NULL, 'l'},
		{"port", required_argument, NULL, 'p'},
		{"state", required_argument, NULL, STATEDIR_OPT},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	set_program_name(argv[0]);
	atexit(close_stdout);

	memset(&rtc, 0, sizeof(struct runtime_config));
	rtc.statedir = F5GS_RUNDIR;

	while ((c = getopt_long(argc, argv, "dmesl:p:Vh", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			send_signal = state_signals[STATE_DISABLE];
			break;
		case 'm':
			send_signal = state_signals[STATE_MAINTENANCE];
			break;
		case 'e':
			send_signal = state_signals[STATE_ENABLE];
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
		case 'V':
			printf("%s version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	if (signal(state_signals[STATE_DISABLE], catch_signals) == SIG_ERR ||
	    signal(state_signals[STATE_MAINTENANCE], catch_signals) == SIG_ERR ||
	    signal(state_signals[STATE_ENABLE], catch_signals) == SIG_ERR)
		err(EXIT_FAILURE, "cannot set signal handler");

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	e = getaddrinfo(listen, port, &hints, &(rtc.res));
	if (e) {
		warnx("getaddrinfo: %s port %s: %s", listen, port, gai_strerror(e));
		exit(EXIT_FAILURE);
	}

	if (send_signal && server)
		kill(getpid(), send_signal);
	else if (server) {
		rtc.msg_type = STATE_UNKNOWN;
		rtc.msg_len = strlen(state_messages[STATE_UNKNOWN]);
	} else if (send_signal) {
		pid_t pid;
		FILE *pidfd;
		char *pid_file = construct_pidfile(&rtc);
		if (!(pidfd = fopen(pid_file, "r")))
			err(EXIT_FAILURE, "cannot open pid file: %s", pid_file);
		fscanf(pidfd, "%d", &pid);
		fclose(pidfd);
		free(pid_file);
		if (kill(pid, send_signal))
			err(EXIT_FAILURE, "sending signal failed");
	}

	if (server)
		run_server(&rtc);

	printf("current status is: %s\n", get_server_status(&rtc));

	freeaddrinfo(rtc.res);

	return EXIT_SUCCESS;
}
