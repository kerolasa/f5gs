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

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_TIMERFD_CREATE
# include <sys/timerfd.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#elif defined HAVE_WS2TCPIP_H
# include <ws2tcpip.h>
#endif

#ifdef HAVE_LIBSYSTEMD
# include <systemd/sd-daemon.h>
# include <systemd/sd-journal.h>
#else
# include <syslog.h>
#endif

#include "close-stream.h"
#include "xalloc.h"

#include "f5gs.h"

/* unavoidable function prototypes */
static void stop_server(struct runtime_config *restrict rtc);

/* global variables */
volatile sig_atomic_t daemon_running;

static void gettime_monotonic(struct timespec *ts)
{
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, ts);
#else
	clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

static void timespec_subtract(const struct timespec *restrict a, const struct timespec *restrict b,
			      struct timespec *restrict c)
{
	if (a->tv_nsec - b->tv_nsec < 0) {
		c->tv_nsec = a->tv_nsec + 1000000000 - b->tv_nsec;
		c->tv_sec = a->tv_sec - 1 - b->tv_sec;
		return;
	}
	c->tv_nsec = a->tv_nsec - b->tv_nsec;
	c->tv_sec = a->tv_sec - b->tv_sec;
}

static void warnlog(const struct runtime_config *restrict rtc, const char *restrict msg)
{
	char buf[STRERRNO_BUF];

	if (rtc->run_foreground && getppid() != 1)
		warn("%s", msg);
	if (strerror_r(errno, buf, sizeof(buf)))
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=%s", msg, "STRERROR=%s", buf, "MESSAGE_ID=%s",
				SD_ID128_CONST_STR(MESSAGE_ERROR), "PRIORITY=%d", LOG_ERR, NULL);
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

static int accept_connection(struct runtime_config *restrict rtc)
{
	int client_socket;
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof client_addr;

	if ((client_socket = accept(rtc->server_socket, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
		warnlog(rtc, "accept failed");
		return -1;
	}
	if (send(client_socket, state_message[rtc->current[rtc->s].state], rtc->current[rtc->s].len, 0) < 0) {
		warnlog(rtc, "send failed");
		return -1;
	}
	return client_socket;
}

static void write_reason(struct runtime_config *restrict rtc, int socket)
{
	char io_buf[IGNORE_BYTES];

	if (recv(socket, io_buf, sizeof(io_buf), 0) < 0) {
		if (errno != EAGAIN)
			warnlog(rtc, "receive failed");
		return;
	}
	if (!memcmp(io_buf, WHYWHEN, sizeof(WHYWHEN))) {
		struct timespec now, delta;
		int len;
		enum {
			SECONDS_IN_DAY = 86400,
			SECONDS_IN_HOUR = 3600,
			SECONDS_IN_MIN = 60
		};

		if (send(socket, rtc->current[rtc->s].reason, strlen(rtc->current[rtc->s].reason), 0) < 0)
			warnlog(rtc, "sending reason failed");
		if (rtc->monotonic) {
			gettime_monotonic(&now);
			timespec_subtract(&now, &rtc->previous_mono, &delta);
		} else {
			clock_gettime(CLOCK_REALTIME, &now);
			timespec_subtract(&now, &rtc->previous_change, &delta);
		}
		len = sprintf(io_buf, "\n%ld days %02ld:%02ld:%02ld,%09ld ago",
			      delta.tv_sec / SECONDS_IN_DAY,
			      delta.tv_sec % SECONDS_IN_DAY / SECONDS_IN_HOUR,
			      delta.tv_sec % SECONDS_IN_HOUR / SECONDS_IN_MIN,
			      delta.tv_sec % SECONDS_IN_MIN, delta.tv_nsec);
		if (len < 0) {
			warnlog(rtc, "reason output truncated");
			return;
		}
		if (send(socket, io_buf, len, 0) < 0)
			warnlog(rtc, "send failed");
	}
}

static void __attribute__((__noreturn__)) *serve_one_request(void *voidpt)
{
	struct socket_pass *sp = voidpt;
	const struct timeval timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	enum {
		SECONDS_IN_DAY = 86400,
		SECONDS_IN_HOUR = 3600,
		SECONDS_IN_MIN = 60
	};

	pthread_detach(pthread_self());
	/* wait a second if client wants more info */
	if (setsockopt(sp->socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout))) {
		warnlog(sp->rtc, "setsockopt failed");
		goto alldone;
	}
	write_reason(sp->rtc, sp->socket);
 alldone:
	close(sp->socket);
	free(sp);
	pthread_exit(NULL);
}

static void __attribute__((__noreturn__)) *handle_requests(void *voidpt)
{
	struct runtime_config *rtc = voidpt;

#ifdef HAVE_PTHREAD_SETNAME_NP
	pthread_setname_np(pthread_self(), "handle_requests");
#endif
	while (daemon_running) {
		pthread_t thread;
		struct socket_pass *sp;

		sp = xmalloc(sizeof(struct socket_pass));
		sp->rtc = rtc;
		if ((sp->socket = accept_connection(rtc)) < 0) {
			free(sp);
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			faillog(rtc, "could not accept connection");
		}
		if (pthread_create(&thread, NULL, serve_one_request, sp))
			warnlog(sp->rtc, "could not create handle_request thread");
	}
	pthread_exit(NULL);
}

static int open_pid_file(struct runtime_config *restrict rtc)
{
	if (access(rtc->state_dir, F_OK))
		if (mkdir(rtc->state_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
			err(EXIT_FAILURE, "cannot create directory: %s", rtc->state_dir);
	if (!(rtc->pid_filefd = fopen(rtc->pid_file, "we")))
		err(EXIT_FAILURE, "cannot not open file: %s", rtc->pid_file);
	return 0;
}

static void update_pid_file(const struct runtime_config *restrict rtc, const int tmp_s)
{
	if (ftruncate(fileno(rtc->pid_filefd), 0)) {
		warnlog(rtc, "pid_file ftruncate failed");
		return;
	}
	rewind(rtc->pid_filefd);
	fprintf(rtc->pid_filefd, "%u %d %d\n", getpid(), rtc->current[tmp_s].state, STATE_FILE_VERSION);
	fprintf(rtc->pid_filefd, "%ld.%09ld:%s", rtc->previous_change.tv_sec, rtc->previous_change.tv_nsec,
		rtc->current[tmp_s].reason + TIME_STAMP_LEN);
	fflush(rtc->pid_filefd);
}

static int close_pid_file(struct runtime_config *restrict rtc)
{
	char buf[STRERRNO_BUF];

	if (rtc->pid_filefd && close_stream(rtc->pid_filefd)) {
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

static int add_tstamp_to_reason(struct runtime_config *restrict rtc, int tmp_s)
{
	time_t prev_c;
	struct tm prev_tm;
	char zone[TSTAMP_ZONE + TSTAMP_NULL];

	rtc->current[tmp_s].reason[0] = '\n';
	prev_c = rtc->previous_change.tv_sec;
	if (localtime_r(&prev_c, &prev_tm) == NULL) {
		warnlog(rtc, "localtime_r() failed");
		return 1;
	}
	if (strftime
	    (rtc->current[tmp_s].reason + TSTAMP_NL, TSTAMP_ISO8601 + TSTAMP_NULL, "%Y-%m-%dT%H:%M:%S",
	     &prev_tm) == 0) {
		warnlog(rtc, "strftime failed");
		return 1;
	}
	snprintf(rtc->current[tmp_s].reason + TSTAMP_NL + TSTAMP_ISO8601, TSTAMP_NSEC + TSTAMP_NULL + TSTAMP_NULL,
		 ",%09ld", rtc->previous_change.tv_nsec);
	strftime(zone, sizeof(zone), "%z ", &prev_tm);
	/* do not null terminate timestamp */
	memcpy(rtc->current[tmp_s].reason + TSTAMP_NL + TSTAMP_ISO8601 + TSTAMP_NSEC, zone, TSTAMP_ZONE);
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
		if (fscanf(pidfd, "%10ld.%10ld:", &(rtc->previous_change.tv_sec), &(rtc->previous_change.tv_nsec)) != 2
		    || errno != 0)
			goto err;
		len = fread(rtc->current[rtc->s].reason + TIME_STAMP_LEN, sizeof(char), REASON_TEXT, pidfd);
		rtc->current[rtc->s].reason[TIME_STAMP_LEN + len] = '\0';
	}
	if (valid_state(state))
		rtc->current[rtc->s].state = (state_code) state;
	else
 err:
		rtc->current[rtc->s].state = STATE_UNKNOWN;
	if (pidfd)
		fclose(pidfd);
	rtc->current[rtc->s].len = strlen(state_message[rtc->current[rtc->s].state]);
}

static void daemonize(void)
{
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
	if (daemon(0, 0))
		err(EXIT_FAILURE, "daemon");
}

static void wait_state_change(struct runtime_config *rtc)
{
	int msqid, tmp_s;
	struct state_change_msg buf;

	if ((msqid = msgget(rtc->ipc_key, IPC_MODE | IPC_CREAT)) == -1)
		faillog(rtc, "could not create message queue");
	while (daemon_running) {
		if (msgrcv(msqid, &buf, sizeof(buf.info), IPC_MSG_ID, MSG_NOERROR) == -1) {
			if (errno == EINTR)
				continue;
			warnlog(rtc, "receiving ipc message failed");
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
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=state change %s -> %s", state_message[rtc->current[rtc->s].state],
				state_message[buf.info.nstate], "MESSAGE_ID=%s",
				SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE), "PRIORITY=%d", LOG_INFO,
				"SENDER_UID=%ld", buf.info.uid, "SENDER_PID=%ld", buf.info.pid,
				"SENDER_TTY=%s", buf.info.tty, NULL);
#else
		syslog(LOG_INFO, "state change received from uid %d pid %d tty %s, state %s -> %s", buf.info.uid,
		       buf.info.pid, buf.info.tty, state_message[rtc->current[rtc->s].state],
		       state_message[buf.info.nstate]);
#endif
		tmp_s = rtc->s ? 0 : 1;
		rtc->current[tmp_s].state = buf.info.nstate;
		rtc->current[tmp_s].len = strlen(state_message[buf.info.nstate]);
		clock_gettime(CLOCK_REALTIME, &rtc->previous_change);
		gettime_monotonic(&rtc->previous_mono);
		rtc->monotonic = 1;
		if (add_tstamp_to_reason(rtc, tmp_s) != 0)
			goto error;
		memccpy((rtc->current[tmp_s].reason + TIME_STAMP_LEN), buf.info.reason, '\0', REASON_TEXT);
		rtc->current[tmp_s].reason[MAX_MESSAGE - 1] = '\0';
		update_pid_file(rtc, tmp_s);
		/* flip which structure is in use, this allows lockless reads */
		rtc->s = tmp_s;
		continue;
 error:
		warnlog(rtc, "previous state change time cannot be reported");
		memset(rtc->current[rtc->s].reason, 0, MAX_MESSAGE);
	}
}

static void stop_server(struct runtime_config *restrict rtc)
{
#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STOPPING=1");
#endif
	daemon_running = 0;
	if (rtc->worker) {
		pthread_kill(rtc->worker, SIGHUP);
		pthread_join(rtc->worker, NULL);
	}
	if (rtc->ipc_key) {
		int qid;

		qid = msgget(rtc->ipc_key, IPC_MODE);
		msgctl(qid, IPC_RMID, NULL);
	}
	if (rtc->server_socket)
		close(rtc->server_socket);
	if (rtc->res)
		freeaddrinfo(rtc->res);
	close_pid_file(rtc);
	if (rtc->pid_file && access(rtc->pid_file, F_OK)) {
		open_pid_file(rtc);
		update_pid_file(rtc, rtc->s);
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
	pthread_attr_t attr;
	struct sigaction sigact = {
		.sa_handler = catch_stop,
		.sa_flags = 0
	};
#ifdef HAVE_LIBSYSTEMD
	const int ret = sd_listen_fds(0);

	if (ret == 1)
		rtc->server_socket = SD_LISTEN_FDS_START + 0;
	else if (ret < 0)
		faillog(rtc, "sd_listen_fds() failed");
	else if (1 < ret)
		faillog(rtc, "too many file descriptors received");
	else {
#else
	{
#endif
		const int on = 1;
		if (!(rtc->server_socket = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol)))
			err(EXIT_FAILURE, "cannot create socket");
		if (setsockopt(rtc->server_socket, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on)))
			err(EXIT_FAILURE, "cannot set socket options");
		if (bind(rtc->server_socket, rtc->res->ai_addr, rtc->res->ai_addrlen))
			err(EXIT_FAILURE, "unable to bind");
		if (listen(rtc->server_socket, SOMAXCONN))
			err(EXIT_FAILURE, "unable to listen");
	}
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (!rtc->run_foreground) {
		daemonize();
		update_pid_file(rtc, rtc->s);
	}
	daemon_running = 1;
	create_worker(rtc);
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service started", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START), "STATE=%s",
			state_message[rtc->current[rtc->s].state], "PRIORITY=%d", LOG_INFO, NULL);
	sd_notify(0, "READY=1");
#else
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "started in state %s", state_message[rtc->current[rtc->s].state]);
#endif
	/* clean up after receiving signal */
	if (sigemptyset(&sigact.sa_mask))
		faillog(rtc, "sigemptyset failed");
#ifdef SIGHUP
	setup_sigaction(rtc, SIGHUP, &sigact);
#endif
#ifdef SIGINT
	setup_sigaction(rtc, SIGINT, &sigact);
#endif
#ifdef SIGQUIT
	setup_sigaction(rtc, SIGQUIT, &sigact);
#endif
#ifdef SIGTERM
	setup_sigaction(rtc, SIGTERM, &sigact);
#endif
#ifdef SIGUSR1
	setup_sigaction(rtc, SIGUSR1, &sigact);
#endif
#ifdef SIGUSR2
	setup_sigaction(rtc, SIGUSR2, &sigact);
#endif
	while (daemon_running)
		wait_state_change(rtc);
	stop_server(rtc);
}

void start_server(struct runtime_config *restrict rtc)
{
	clock_gettime(CLOCK_REALTIME, &rtc->previous_change);
	memcpy(rtc->current[rtc->s].reason, "<program started>", 18);
	read_status_from_file(rtc);
	if (add_tstamp_to_reason(rtc, rtc->s))
		exit(EXIT_FAILURE);
	open_pid_file(rtc);
	update_pid_file(rtc, rtc->s);
	if (!(rtc->ipc_key = ftok(rtc->pid_file, IPC_MSG_ID)))
		err(EXIT_FAILURE, "ftok failed");
	run_server(rtc);
}
