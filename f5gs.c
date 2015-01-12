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
#include <paths.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#elif defined HAVE_WS2TCPIP_H
# include <ws2tcpip.h>
#endif

#ifdef HAVE_LIBSYSTEMD
# include <systemd/sd-daemon.h>
# include <systemd/sd-journal.h>
#endif

#include "close-stream.h"
#include "closeout.h"
#include "progname.h"
#include "xalloc.h"

#include "f5gs.h"

/* global variables */
volatile sig_atomic_t daemon_running;

/* keep functions in the order that allows skipping the function
 * definition lines */
static void __attribute__((__noreturn__))
    usage(FILE *restrict out)
{
	fputs("\nUsage:\n", out);
	fprintf(out, " %s [options]\n", program_invocation_short_name);
	fputs("\nOptions:\n", out);
	fputs(" -e, --enable         set enable status\n", out);
	fputs(" -m, --maintenance    set maintenance status\n", out);
	fputs(" -d, --disable        set disable status\n", out);
	fputs("\n", out);
	fputs(" -s, --server         start up daemon\n", out);
	fputs(" -a, --address <addr> address (ip or name) daemon will listen\n", out);
	fprintf(out, " -p, --port <port>    deamon tcp port (default: %s)\n", F5GS_TCP_PORT);
	fprintf(out, "     --statedir <dir> status dir path (default: %s)\n", F5GS_RUNDIR);
	fputs("     --foreground     run daemon in foreground\n", out);
	fputs(" -q, --quiet          do not print status, use exit values\n", out);
	fputs("     --reason <text>  add explanation to status change\n", out);
	fputs("     --why            query reason, and when status was changed\n", out);
	fputs("     --force          run pre and post script and ignore return values\n", out);
	fputs("     --no-scripts     do not run pre or post status change scripts\n", out);
	fputs("\n", out);
	fputs(" -h, --help           display this help and exit\n", out);
	fputs(" -V, --version        output version information and exit\n", out);
	fputs("\n", out);
	fprintf(out, "For more details see %s(8).\n", PACKAGE_NAME);
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void warnlog(const struct runtime_config *restrict rtc, const char *restrict msg)
{
	char buf[256];

	if (rtc->run_foreground && getppid() != 1)
		warn("%s", msg);
	if (strerror_r(errno, buf, sizeof(buf)))
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=%s", msg, "STRERROR=%s", buf, "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "%s: %s", msg, buf);
#endif
}

static void __attribute__((__noreturn__))
    faillog(struct runtime_config *restrict rtc, const char *restrict msg)
{
	warnlog(rtc, msg);
	stop_server(rtc);
	exit(EXIT_FAILURE);
}

static int make_socket_none_blocking(struct runtime_config *restrict rtc, int socket)
{
	int flags;
	if ((flags = fcntl(socket, F_GETFL)) < 0) {
		warnlog(rtc, "fcntl F_GETFL failed");
		return -1;
	}
	flags |= O_NONBLOCK;
	if (fcntl(socket, F_SETFL, flags) < 0) {
		warnlog(rtc, "fcntl F_SETFL failed");
		return -1;
	}
	return 0;
}

static void accept_connection(struct runtime_config *restrict rtc)
{
	int client_socket;
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof client_addr;
	const struct timeval timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	struct epoll_event event;

	if ((client_socket = accept(rtc->server_socket, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
		warnlog(rtc, "accept failed");
		return;
	}
	if (pthread_rwlock_rdlock(&rtc->lock)) {
		warnlog(rtc, "thread read-lock failed");
		return;
	}
	if (send(client_socket, state_message[rtc->current_state], rtc->message_length, 0) < 0) {
		pthread_rwlock_unlock(&rtc->lock);
		warnlog(rtc, "send failed");
		return;
	}
	pthread_rwlock_unlock(&(rtc->lock));
	if (make_socket_none_blocking(rtc, client_socket)) {
		warnlog(rtc, "fcntl none-blocking failed");
		return;
	}
	if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout))) {
		warnlog(rtc, "setsockopt failed");
		return;
	}
	event.events = EPOLLIN | EPOLLET;
	event.data.fd = client_socket;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, client_socket, &event) < 0) {
		warnlog(rtc, "epoll_ctl failed");
		return;
	}
}

static void write_reason(struct runtime_config *restrict rtc, int socket)
{
	char io_buf[IGNORE_BYTES];
	enum {
		SECONDS_IN_DAY = 86400,
		SECONDS_IN_HOUR = 3600,
		SECONDS_IN_MIN = 60
	};

	if (recv(socket, io_buf, sizeof(io_buf), 0) < 0) {
		if (errno != EAGAIN)
			warnlog(rtc, "receive failed");
		return;
	}
	if (!strcmp(io_buf, WHYWHEN)) {
		struct timeval now, delta;

		if (send(socket, rtc->current_reason, strlen(rtc->current_reason), 0) < 0) {
			warnlog(rtc, "sending reason failed");
		}
		gettimeofday(&now, NULL);
		if (timercmp(&now, &rtc->previous_change, <))
			/* time went backwards, ignore result */ ;
		else {
			timersub(&now, &rtc->previous_change, &delta);
			int len = sprintf(io_buf, "\n%ld days %02ld:%02ld:%02ld ago",
					  delta.tv_sec / SECONDS_IN_DAY,
					  delta.tv_sec % SECONDS_IN_DAY / SECONDS_IN_HOUR,
					  delta.tv_sec % SECONDS_IN_HOUR / SECONDS_IN_MIN,
					  delta.tv_sec % SECONDS_IN_MIN);
			if (send(socket, io_buf, len, 0) < 0)
				warnlog(rtc, "send failed");
		}
	}
}

static void __attribute__((__noreturn__)) *handle_requests(void *voidpt)
{
	struct runtime_config *rtc = voidpt;
	struct epoll_event *events;
	int r, i;

	events = xmalloc(NUM_EVENTS * sizeof(struct epoll_event));
	while (daemon_running) {
		r = epoll_wait(rtc->epollfd, events, NUM_EVENTS, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			warnlog(rtc, "epoll_wait failed");
			continue;
		}
		for (i = 0; i < r; i++) {
			if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
			    || (!(events[i].events & EPOLLIN))) {
				close(events[i].data.fd);
				continue;
			}
			if (events[i].data.fd == rtc->server_socket)
				accept_connection(rtc);
			else {
				write_reason(rtc, events[i].data.fd);
				close(events[i].data.fd);
			}
		}
	}
	free(events);
	pthread_exit(NULL);
}

static char *construct_pid_file(struct runtime_config *restrict rtc)
{
	char *path;
	void *p;
	char s[INET6_ADDRSTRLEN];
	int separator = 0, ret;
	const char *last_slash = strrchr(rtc->state_dir, '/');

	if (last_slash && *(last_slash + 1) != '\0' && *(last_slash + 1) != '/')
		separator = 1;
	else if (!last_slash)
		separator = 1;
	if (inet_ntop(rtc->res->ai_family, rtc->res->ai_addr->sa_data, s, sizeof(s)) == NULL)
		err(EXIT_FAILURE, "inet_ntop failed");
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
	if (inet_ntop(rtc->res->ai_family, p, s, sizeof(s)) == NULL)
		err(EXIT_FAILURE, "inet_ntop failed");
	ret = asprintf(&path, "%s%s%s:%d", rtc->state_dir, separator ? "/" : "", s,
		       ntohs(((struct sockaddr_in *)(rtc->res->ai_addr))->sin_port));
	if (ret < 0)
		err(EXIT_FAILURE, "asprintf failed");
	return path;
}

static int open_pid_file(struct runtime_config *restrict rtc)
{
	if (access(rtc->state_dir, F_OK))
		if (mkdir(rtc->state_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
			err(EXIT_FAILURE, "cannot create directory: %s", rtc->state_dir);
	if (!(rtc->pid_filefd = fopen(rtc->pid_file, "w")))
		err(EXIT_FAILURE, "cannot not open file: %s", rtc->pid_file);
	return 0;
}

static void update_pid_file(const struct runtime_config *restrict rtc)
{
	if (ftruncate(fileno(rtc->pid_filefd), 0)) {
		warnlog(rtc, "pid_file ftruncate failed");
		return;
	}
	rewind(rtc->pid_filefd);
	fprintf(rtc->pid_filefd, "%u %d %d\n", getpid(), rtc->current_state, STATE_FILE_VERSION);
	fprintf(rtc->pid_filefd, "%ld.%ld:%s", rtc->previous_change.tv_sec, rtc->previous_change.tv_usec,
		rtc->current_reason + TIME_STAMP_LEN);
	fflush(rtc->pid_filefd);
}

static int close_pid_file(struct runtime_config *restrict rtc)
{
	char buf[256];

	if (close_stream(rtc->pid_filefd)) {
		if (strerror_r(errno, buf, sizeof(buf)))
#ifdef HAVE_LIBSYSTEMD
			sd_journal_send("MESSAGE=closing %s failed", rtc->pid_file, "MESSAGE_ID=%s",
					SD_ID128_CONST_STR(MESSAGE_ERROR), "STRERROR=%s", buf, "PRIORITY=%d", LOG_ERR,
					NULL);
#else
			syslog(LOG_ERR, "close failed: %s: %s", rtc->pid_file, buf);
#endif
		return 1;
	}
	return 0;
}

static int add_tstamp_to_reason(struct runtime_config *restrict rtc)
{
	char explanation[MAX_MESSAGE];
	char *p = explanation;
	time_t prev_c;
	struct tm prev_tm;

	prev_c = rtc->previous_change.tv_sec;
	*p = '\n';
	p++;
	if (localtime_r(&prev_c, &prev_tm) == NULL) {
		warnlog(rtc, "localtime_r() failed");
		return 1;
	}
	if (strftime(p, 20, "%Y-%m-%dT%H:%M:%S", &prev_tm) == 0) {
		warnlog(rtc, "strftime failed");
		return 1;
	}
	p += strlen(p);
	snprintf(p, 7, ",%06ld", rtc->previous_change.tv_usec);
	p += strlen(p);
	strftime(p, 7, "%z ", &prev_tm);
	p += strlen(p);
	strcpy(p, rtc->current_reason);
	strcpy(rtc->current_reason, explanation);
	return 0;
}

static int valid_state(const int state)
{
	if (state < STATE_ENABLE || STATE_UNKNOWN < state)
		return 0;
	return 1;
}

static void read_status_from_file(struct runtime_config *restrict rtc)
{
	FILE *pidfd;
	int ignored, state, version;

	if (!(pidfd = fopen(rtc->pid_file, "r")))
		goto err;
	errno = 0;
	if (fscanf(pidfd, "%10d %1d %1d", &ignored, &state, &version) != 3 || errno != 0)
		goto err;
	if (version < 0 || STATE_FILE_VERSION < version)
		goto err;
	if (0 < version) {
		size_t len;
		if (fscanf(pidfd, "%10ld.%6ld:", &(rtc->previous_change.tv_sec), &(rtc->previous_change.tv_usec)) != 2
		    || errno != 0)
			goto err;
		len = fread(rtc->current_reason, sizeof(char), REASON_TEXT, pidfd);
		rtc->current_reason[len] = '\0';
	}
	if (valid_state(state))
		rtc->current_state = (state_code) state;
	else
 err:
		rtc->current_state = STATE_UNKNOWN;
	if (pidfd)
		fclose(pidfd);
	rtc->message_length = strlen(state_message[rtc->current_state]);
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
		err(EXIT_FAILURE, "cannot chdir to root");
	if ((fd = open("/dev/null", O_RDWR, 0)) < 0)
		err(EXIT_FAILURE, "cannot open /dev/null");
	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);
}

static void wait_state_change(struct runtime_config *rtc)
{
	int msqid;
	struct state_change_msg buf;

	if ((msqid = msgget(rtc->ipc_key, 0600 | IPC_CREAT)) == -1)
		faillog(rtc, "could not create message queue");
	while (daemon_running) {
		if (msgrcv(msqid, &buf, sizeof(buf.info), IPC_MSG_ID, MSG_NOERROR) == -1) {
			if (errno == EINTR)
				continue;
#ifdef HAVE_LIBSYSTEMD
			char ebuf[256];
			if (strerror_r(errno, ebuf, sizeof(ebuf)))
				sd_journal_send("MESSAGE=receiving ipc message failed", "MESSAGE_ID=%s",
						SD_ID128_CONST_STR(MESSAGE_ERROR), "STRERROR=%s", ebuf, "PRIORITY=%d",
						LOG_ERR, NULL);
#else
			syslog(LOG_ERR, "receiving ipc message failed");
#endif
			continue;
		}
		if (!valid_state(buf.info.nstate)) {
#ifdef HAVE_LIBSYSTEMD
			sd_journal_send("MESSAGE=unknown state change: %d", buf.info.nstate,
					"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR), "PRIORITY=%d", LOG_ERR,
					NULL);
#else
			syslog(LOG_INFO, "unknown state change: %d", buf.info.nstate);
#endif
			continue;
		}
		if (pthread_rwlock_wrlock(&rtc->lock)) {
#ifdef HAVE_LIBSYSTEMD
			char ebuf[256];
			if (strerror_r(errno, ebuf, sizeof(ebuf)))
				sd_journal_send("MESSAGE=could not get state change lock", "MESSAGE_ID=%s",
						SD_ID128_CONST_STR(MESSAGE_ERROR), "STRERROR=%s", ebuf, "PRIORITY=%d",
						LOG_ERR, NULL);
#else
			syslog(LOG_ERR, "could not get state change lock");
#endif
			continue;
		}
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=state change %s -> %s", state_message[rtc->current_state],
				state_message[buf.info.nstate], "MESSAGE_ID=%s",
				SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE), "PRIORITY=%d", LOG_INFO,
				"SENDER_UID=%ld", buf.info.uid, "SENDER_PID=%ld", buf.info.pid,
				"SENDER_TTY=%s", buf.info.tty, NULL);
#else
		syslog(LOG_INFO, "state change received from uid %d pid %d tty %s, state %s -> %s", buf.info.uid,
		       buf.info.pid, buf.info.tty, state_message[rtc->current_state], state_message[buf.info.nstate]);
#endif
		rtc->current_state = buf.info.nstate;
		rtc->message_length = strlen(state_message[rtc->current_state]);
		strcpy(rtc->current_reason, buf.info.reason);
		gettimeofday(&rtc->previous_change, NULL);
		if (add_tstamp_to_reason(rtc) == 0)
			update_pid_file(rtc);
		pthread_rwlock_unlock(&rtc->lock);
	}
}

static void stop_server(struct runtime_config *restrict rtc)
{
	int qid;

#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STOPPING=1");
#endif
	daemon_running = 0;
	pthread_kill(rtc->worker, SIGHUP);
	pthread_join(rtc->worker, NULL);
	pthread_rwlock_destroy(&rtc->lock);
	qid = msgget(rtc->ipc_key, 0600);
	msgctl(qid, IPC_RMID, NULL);
	close(rtc->server_socket);
	freeaddrinfo(rtc->res);
	close_pid_file(rtc);
	if (access(rtc->pid_file, F_OK)) {
		open_pid_file(rtc);
		update_pid_file(rtc);
		close_pid_file(rtc);
	}
	free(rtc->pid_file);
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service stopped", "MESSAGE_ID=%s",
			SD_ID128_CONST_STR(MESSAGE_STOP_START), "PRIORITY=%d", LOG_INFO, NULL);
#else
	syslog(LOG_INFO, "service stopped");
	closelog();
#endif
}

static void catch_stop(const int sig __attribute__((unused)))
{
	daemon_running = 0;
}

static void create_worker(struct runtime_config *restrict rtc)
{
	pthread_attr_t attr;

	if (pthread_attr_init(&attr))
		faillog(rtc, "cannot init thread attribute");
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
		faillog(rtc, "cannot use PTHREAD_CREATE_DETACHED");
	if (pthread_create(&rtc->worker, NULL, handle_requests, rtc))
		faillog(rtc, "could not create worker thread");
}

static void setup_sigaction(struct runtime_config *restrict rtc, int sig, struct sigaction *sigact)
{
	if (sigaction(sig, sigact, NULL))
		faillog(rtc, "sigaction failed");
}

static void run_server(struct runtime_config *restrict rtc)
{
	struct epoll_event event;
	pthread_attr_t attr;
	struct sigaction sigact = {
		.sa_handler = catch_stop,
		.sa_flags = 0
	};
#ifdef HAVE_LIBSYSTEMD
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
		if (make_socket_none_blocking(rtc, rtc->server_socket))
			err(EXIT_FAILURE, "cannot set server socket none-blocking");
		if (listen(rtc->server_socket, SOMAXCONN))
			err(EXIT_FAILURE, "unable to listen");
	}
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (pthread_rwlock_init(&rtc->lock, NULL))
		err(EXIT_FAILURE, "cannot init read-write lock");

	if (!rtc->run_foreground)
		daemonize();
	daemon_running = 1;
#ifdef HAVE_EPOLL_CREATE1
	if ((rtc->epollfd = epoll_create1(0)) < 0)
#else
	if ((rtc->epollfd = epoll_create(NUM_EVENTS)) < 0)
#endif
		faillog(rtc, "epoll_create failed");
	event.events = EPOLLIN | EPOLLET;
	event.data.fd = rtc->server_socket;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, rtc->server_socket, &event) < 0)
		faillog(rtc, "epoll_ctl add socket failed");
	create_worker(rtc);
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service started", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START), "STATE=%s",
			state_message[rtc->current_state], "PRIORITY=%d", LOG_INFO, NULL);
	sd_notify(0, "READY=1");
#else
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "started in state %s", state_message[rtc->current_state]);
#endif
	/* clean up after receiving signal */
	if (sigemptyset(&sigact.sa_mask))
		faillog(rtc, "sigemptyset failed");
#ifdef SIGHUP
	setup_sigaction(rtc, SIGHUP,  &sigact);
#endif
#ifdef SIGINT
	setup_sigaction(rtc, SIGINT,  &sigact);
#endif
#ifdef SIGQUIT
	setup_sigaction(rtc, SIGQUIT,  &sigact);
#endif
#ifdef SIGTERM
	setup_sigaction(rtc, SIGTERM,  &sigact);
#endif
#ifdef SIGUSR1
	setup_sigaction(rtc, SIGUSR1,  &sigact);
#endif
#ifdef SIGUSR2
	setup_sigaction(rtc, SIGUSR2,  &sigact);
#endif
	while (daemon_running)
		wait_state_change(rtc);
	stop_server(rtc);
}

static int run_script(const struct runtime_config *restrict rtc, const char *restrict script)
{
	pid_t child;
	int status = 0;

	if (rtc->no_scripts || access(script, X_OK))
		return 0;
	child = fork();
	if (child < 0)
		err(EXIT_FAILURE, "cannot fork %s", script);
	if (child == 0) {
		if (setuid(geteuid()))
			err(EXIT_FAILURE, "setuid() failed");
#ifdef HAVE_CLEARENV
		clearenv();
#else
		environ = NULL;
#endif
		if (setenv("PATH", _PATH_STDPATH, 1) < 0)
			err(EXIT_FAILURE, "cannot setenv");
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

static int change_state(struct runtime_config *restrict rtc)
{
	struct state_change_msg buf = {
		.mtype = IPC_MSG_ID,
	};
	int qid;

	buf.info.nstate = rtc->new_state;
	buf.info.uid = getuid();
	buf.info.pid = getpid();
	if (ttyname_r(STDIN_FILENO, buf.info.tty, TTY_NAME_LEN) != 0) {
		if (errno != ENOTTY)
			warn("ttyname_r failed");
	}
	if (rtc->new_reason)
		memcpy(buf.info.reason, rtc->new_reason, strlen(rtc->new_reason) + 1);
	else
		buf.info.reason[0] = '\0';
	if (run_script(rtc, F5GS_PRE) && !rtc->force)
		return 1;
	if ((rtc->ipc_key = ftok(rtc->pid_file, buf.mtype)) < 0)
		errx(EXIT_FAILURE, "ftok: is f5gs server process running?");
	if ((qid = msgget(rtc->ipc_key, 0600)) < 0)
		errx(EXIT_FAILURE, "msgget failed: server stopped or a version mismatch?");
	if (msgsnd(qid, (void *)&buf, sizeof(buf.info), 0) != 0)
		err(EXIT_FAILURE, "ipc message sending failed");
	if (run_script(rtc, F5GS_POST) && !rtc->force)
		return 2;
	return 0;
}

static char *get_server_status(const struct runtime_config *restrict rtc)
{
	int sfd;
	const struct timeval timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	static char buf[CLIENT_SOCKET_BUF];
	ssize_t buflen;

	if (!(sfd = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol))) {
		if (rtc->quiet)
			exit(STATE_UNKNOWN);
		else
			err(EXIT_FAILURE, "cannot create socket");
	}
	if (setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)))
		err(EXIT_FAILURE, "setsockopt failed");
	if (connect(sfd, rtc->res->ai_addr, rtc->res->ai_addrlen)) {
		if (rtc->quiet)
			exit(STATE_UNKNOWN);
		else
			err(EXIT_FAILURE, "cannot connect");
	}
	if (rtc->why) {
		struct timespec waittime = {
			.tv_sec = 0L,
			.tv_nsec = 1000000L
		};
		if (send(sfd, WHYWHEN, sizeof(WHYWHEN), 0) < 0)
			err(EXIT_FAILURE, "sending why request failed");
		/* this gives handle_requests() time to write both status
		 * and reason to socket */
		nanosleep(&waittime, NULL);
	}
	buflen = recv(sfd, buf, CLIENT_SOCKET_BUF, 0);
	if (buflen < 0)
		err(EXIT_FAILURE, "reading socket failed");
	else
		buf[buflen] = '\0';
	return buf;
}

static char *getenv_str(const char *restrict name)
{
	const char *temp = getenv(name);
	char *tmpvar;

	if (temp != NULL)
		tmpvar = xstrdup(temp);
	else
		tmpvar = xstrdup("NULL");
	return tmpvar;
}

static int set_server_status(struct runtime_config *restrict rtc)
{
	char *username, *sudo_user;

	switch (change_state(rtc)) {
	case 0:		/* all ok */
		break;
	case 1:
		errx(EXIT_FAILURE, "consider running with --no-scripts or --force");
	case 2:
		warnx("it is too late to abort, continueing to the end");
		break;
	default:	/* should be impossible */
		abort();
	}
	username = getenv_str("USER");
	sudo_user = getenv_str("SUDO_USER");
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=state change was sent", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE),
			"USER=%s", username, "SUDO_USER=%s", sudo_user, "PRIORITY=%d", LOG_INFO, NULL);
#else
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "state change was sent by USER: %s SUDO_USER: %s", username, sudo_user);
	closelog();
#endif
	free(username);
	free(sudo_user);
	return EXIT_SUCCESS;
}

int main(const int argc, char **argv)
{
	static struct runtime_config rtc = {
		.state_dir = F5GS_RUNDIR,
		.new_state = STATE_UNKNOWN,
		0
	};
	int c, server = 0, retval = EXIT_SUCCESS;
	const char *address = NULL, *port = F5GS_TCP_PORT;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE
	};
	enum {
		STATEDIR_OPT = CHAR_MAX + 1,
		REASON_OPT,
		WHY_OPT,
		FORCE_OPT,
		NO_SCRIPTS_OPT,
		FOREGROUND_OPT
	};

	static const struct option longopts[] = {
		{"disable", no_argument, NULL, 'd'},
		{"maintenance", no_argument, NULL, 'm'},
		{"enable", no_argument, NULL, 'e'},
		{"server", no_argument, NULL, 's'},
		{"address", required_argument, NULL, 'a'},
		{"listen", required_argument, NULL, 'l'},
		{"port", required_argument, NULL, 'p'},
		{"statedir", required_argument, NULL, STATEDIR_OPT},
		{"quiet", no_argument, NULL, 'q'},
		{"reason", required_argument, NULL, REASON_OPT},
		{"why", no_argument, NULL, WHY_OPT},
		{"force", no_argument, NULL, NO_SCRIPTS_OPT},
		{"no-scripts", no_argument, NULL, NO_SCRIPTS_OPT},
		{"foreground", no_argument, NULL, FOREGROUND_OPT},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	set_program_name(argv[0]);
	atexit(close_stdout);
	rtc.argv = argv;

	while ((c = getopt_long(argc, argv, "dmesa:l:p:qVh", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			rtc.new_state = STATE_DISABLE;
			break;
		case 'm':
			rtc.new_state = STATE_MAINTENANCE;
			break;
		case 'e':
			rtc.new_state = STATE_ENABLE;
			break;
		case 's':
			server = 1;
			break;
		case 'l':	/* FIXME: to be removed after 2015-08-01 */
			warnx("--listen is deprecated, use --address instead");
		case 'a':
			address = optarg;
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
		case REASON_OPT:
			if (REASON_TEXT < strlen(optarg)) {
				warnx("too long reason, truncating to %d characters", REASON_TEXT);
				optarg[REASON_TEXT] = '\0';
			}
			rtc.new_reason = optarg;
			break;
		case WHY_OPT:
			rtc.why = 1;
			break;
		case FORCE_OPT:
			rtc.force = 1;
			break;
		case NO_SCRIPTS_OPT:
			rtc.no_scripts = 1;
			break;
		case FOREGROUND_OPT:
			rtc.run_foreground = 1;
			break;
		case 'V':
			printf("%s version %s", PACKAGE_NAME, PACKAGE_VERSION);
#ifdef HAVE_LIBSYSTEMD
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
	c = getaddrinfo(address, port, &hints, &rtc.res);
	if (c) {
		if (rtc.quiet)
			exit(STATE_UNKNOWN);
		else
			errx(EXIT_FAILURE, "getaddrinfo: %s port %s: %s", address, port, gai_strerror(c));
	}
	rtc.pid_file = construct_pid_file(&rtc);
	/* run server */
	if (server) {
		gettimeofday(&rtc.previous_change, NULL);
		memcpy(rtc.current_reason, "<program started>", 18);
		read_status_from_file(&rtc);
		if (add_tstamp_to_reason(&rtc))
			exit(EXIT_FAILURE);
		open_pid_file(&rtc);
		update_pid_file(&rtc);
		if (!(rtc.ipc_key = ftok(rtc.pid_file, IPC_MSG_ID)))
			err(EXIT_FAILURE, "ftok failed");
		run_server(&rtc);
		return EXIT_SUCCESS;
	}
	/* change server state */
	if (rtc.new_state != STATE_UNKNOWN) {
		struct timespec waittime = {
			.tv_sec = 0L,
			.tv_nsec = 1000000L
		};
		retval = set_server_status(&rtc);
		/* sleep a bit before get_server_status(), else sometimes
		 * server replies using old information */
		nanosleep(&waittime, NULL);
	}
	/* request server state */
	if (rtc.quiet) {
		char *s;
		int i;

		s = get_server_status(&rtc);
		for (i = 0; i <= STATE_UNKNOWN; i++)
			if (!strcmp(s, state_message[i]))
				break;
		retval = i;
	} else {
		if (address)
			printf("%s: ", address);
		printf("current status is: %s\n", get_server_status(&rtc));
	}
	/* clean up and exit */
	freeaddrinfo(rtc.res);
	free(rtc.pid_file);
	return retval;
}
