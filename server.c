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
#include <mqueue.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

enum event_type {
	EVENT_LISTENER = 0,
	EVENT_SIGNAL,
	EVENT_IPC,
	EVENT_CLIENT
};

#define EVENT_DATA(type)	((uint64_t) (type))
#define EVENT_CLIENT_DATA(fd)	(EVENT_DATA(EVENT_CLIENT) | ((uint64_t) (uint32_t) (fd) << 32))
#define EVENT_SOURCE(data)	((enum event_type) ((data) & UINT32_C(3)))
#define EVENT_FD(data)		((int) ((data) >> 32))

static inline void gettime_monotonic(struct timespec *ts)
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
	if (strerror_r(errno, buf, sizeof(buf)) == 0)
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

static int make_socket_none_blocking(struct runtime_config *restrict rtc, int socket)
{
	int flags;

	if ((flags = fcntl(socket, F_GETFL)) < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) {
		warnlog(rtc, "cannot make socket none-blocking");
		return 1;
	}
	if ((flags = fcntl(socket, F_GETFD)) < 0 || fcntl(socket, F_SETFD, flags | FD_CLOEXEC) < 0) {
		warnlog(rtc, "cannot make socket close-on-exec");
		return 1;
	}
	return 0;
}

static int submit_poll_event(struct runtime_config *restrict rtc, int fd, uint64_t data)
{
	struct io_uring_sqe *sqe;
	int ret;

	if (!(sqe = io_uring_get_sqe(&rtc->ring)))
		return -ENOSPC;
	io_uring_prep_poll_add(sqe, fd, POLLIN);
	/* The data64 helpers are not available in liburing 2.0. */
	sqe->user_data = data;
	ret = io_uring_submit(&rtc->ring);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;
	return 0;
}

static int send_message(struct runtime_config *restrict rtc, int socket, const void *data, size_t len)
{
	const char *buf = data;
	ssize_t sent;

	while (len) {
		if ((sent = send(socket, buf, len, MSG_NOSIGNAL)) < 0) {
			if (errno == EINTR)
				continue;
			warnlog(rtc, "send failed");
			return 1;
		}
		if (!sent) {
			errno = EPIPE;
			warnlog(rtc, "send failed");
			return 1;
		}
		buf += (size_t) sent;
		len -= (size_t) sent;
	}
	return 0;
}

static void accept_connection(struct runtime_config *restrict rtc)
{
	int client_socket, ret;

#ifdef HAVE_ACCEPT4
	if ((client_socket =
		 accept4(rtc->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) < 0) {
#else
	if ((client_socket = accept(rtc->listen_fd, NULL, NULL)) < 0) {
#endif
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED)
			warnlog(rtc, "accept failed");
		return;
	}
	if (make_socket_none_blocking(rtc, client_socket)
	    || send_message(rtc, client_socket, state_message[rtc->current[rtc->s].state],
			    rtc->current[rtc->s].len)) {
		close(client_socket);
		return;
	}
	ret = submit_poll_event(rtc, client_socket, EVENT_CLIENT_DATA(client_socket));
	if (ret < 0) {
		errno = -ret;
		warnlog(rtc, "io_uring poll_add failed");
		close(client_socket);
	}
}

static void write_reason(struct runtime_config *restrict rtc, int socket, const char *request)
{
	char time_buf[IGNORE_BYTES];
	struct timespec now, delta;
	int len;
	enum {
		SECONDS_IN_DAY = 86400,
		SECONDS_IN_HOUR = 3600,
		SECONDS_IN_MIN = 60
	};

	if (memcmp(request, WHYWHEN, sizeof(WHYWHEN) - 1))
		return;
	if (send_message(rtc, socket, rtc->current[rtc->s].reason, strlen(rtc->current[rtc->s].reason)))
		return;
	if (rtc->monotonic) {
		gettime_monotonic(&now);
		timespec_subtract(&now, &rtc->previous_mono, &delta);
	} else {
		clock_gettime(CLOCK_REALTIME, &now);
		timespec_subtract(&now, &rtc->previous_change, &delta);
	}
	len = snprintf(time_buf, sizeof(time_buf), "\n%ld days %02ld:%02ld:%02ld,%09ld ago",
		       delta.tv_sec / SECONDS_IN_DAY, delta.tv_sec % SECONDS_IN_DAY / SECONDS_IN_HOUR,
		       delta.tv_sec % SECONDS_IN_HOUR / SECONDS_IN_MIN, delta.tv_sec % SECONDS_IN_MIN,
		       delta.tv_nsec);
	if (len < 0 || (size_t) len >= sizeof(time_buf)) {
		errno = EOVERFLOW;
		warnlog(rtc, "reason output truncated");
		return;
	}
	send_message(rtc, socket, time_buf, (size_t) len);
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
		if (strerror_r(errno, buf, sizeof(buf)) == 0)
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

	if (!(pidfd = fopen(rtc->pid_file, "re")))
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

static void change_state(struct runtime_config *rtc)
{
	int tmp_s;
	struct state_info buf;
	char *msg = (char *)&buf;

	while (mq_receive(rtc->ipc_mq, msg, sizeof(buf), NULL) < 0) {
		if (errno == EINTR)
			continue;
		warnlog(rtc, "receiving ipc message failed");
		return;
	}
	if (!valid_state(buf.nstate)) {
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=unknown state change: %d", buf.nstate,
				"MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR), "PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_INFO, "unknown state change: %d", buf.nstate);
#endif
		return;
	}
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=state change %s -> %s", state_message[rtc->current[rtc->s].state],
			state_message[buf.nstate], "MESSAGE_ID=%s",
			SD_ID128_CONST_STR(MESSAGE_STATE_CHANGE), "PRIORITY=%d", LOG_INFO,
			"SENDER_UID=%ld", buf.uid, "SENDER_PID=%ld", buf.pid, "SENDER_TTY=%s", buf.tty, NULL);
#else
	syslog(LOG_INFO, "state change received from uid %d pid %d tty %s, state %s -> %s", buf.uid,
	       buf.pid, buf.tty, state_message[rtc->current[rtc->s].state], state_message[buf.nstate]);
#endif
	tmp_s = rtc->s ? 0 : 1;
	rtc->current[tmp_s].state = buf.nstate;
	rtc->current[tmp_s].len = strlen(state_message[buf.nstate]);
	clock_gettime(CLOCK_REALTIME, &rtc->previous_change);
	gettime_monotonic(&rtc->previous_mono);
	rtc->monotonic = 1;
	if (add_tstamp_to_reason(rtc, tmp_s) != 0)
		goto error;
	memccpy((rtc->current[tmp_s].reason + TIME_STAMP_LEN), buf.reason, '\0', REASON_TEXT);
	rtc->current[tmp_s].reason[MAX_MESSAGE - 1] = '\0';
	update_pid_file(rtc, tmp_s);
	/* flip which structure is in use, this allows lockless reads */
	rtc->s = tmp_s;
	return;
 error:
	warnlog(rtc, "previous state change time cannot be reported");
	memset(rtc->current[rtc->s].reason, 0, MAX_MESSAGE);
}

static void wait_events(struct runtime_config *rtc)
{
	struct io_uring_cqe *cqe;
	uint64_t data;
	int client_socket, op_ret, poll_ret;
	ssize_t bytes;

	for (;;) {
		if ((op_ret = io_uring_wait_cqe(&rtc->ring, &cqe)) < 0) {
			if (op_ret == -EINTR)
				continue;
			errno = -op_ret;
			faillog(rtc, "io_uring_wait_cqe failed");
		}
		data = cqe->user_data;
		poll_ret = cqe->res;
		io_uring_cqe_seen(&rtc->ring, cqe);

		switch (EVENT_SOURCE(data)) {
		case EVENT_LISTENER:
			if (poll_ret < 0) {
				errno = -poll_ret;
				faillog(rtc, "listener poll operation failed");
			}
			if (poll_ret & (POLLERR | POLLHUP | POLLNVAL)) {
				errno = EIO;
				faillog(rtc, "listener poll returned an error");
			}
			if (poll_ret & POLLIN)
				accept_connection(rtc);
			op_ret = submit_poll_event(rtc, rtc->listen_fd, EVENT_DATA(EVENT_LISTENER));
			if (op_ret < 0) {
				errno = -op_ret;
				faillog(rtc, "cannot rearm listener poll operation");
			}
			break;
		case EVENT_SIGNAL:
			if (poll_ret < 0) {
				errno = -poll_ret;
				faillog(rtc, "signal poll operation failed");
			}
			if (poll_ret & (POLLERR | POLLNVAL)) {
				errno = EIO;
				faillog(rtc, "signal poll returned an error");
			}
			return;
		case EVENT_IPC:
			if (poll_ret < 0) {
				errno = -poll_ret;
				faillog(rtc, "message queue poll operation failed");
			}
			if (poll_ret & (POLLERR | POLLHUP | POLLNVAL)) {
				errno = EIO;
				faillog(rtc, "message queue poll returned an error");
			}
			if (poll_ret & POLLIN)
				change_state(rtc);
			op_ret = submit_poll_event(rtc, rtc->ipc_mq, EVENT_DATA(EVENT_IPC));
			if (op_ret < 0) {
				errno = -op_ret;
				faillog(rtc, "cannot rearm message queue poll operation");
			}
			break;
		case EVENT_CLIENT: {
			char request[sizeof(WHYWHEN)];

			client_socket = EVENT_FD(data);
			if (poll_ret >= 0 && (poll_ret & POLLIN)) {
				bytes = recv(client_socket, request, sizeof(request), 0);
				if (bytes >= (ssize_t)(sizeof(WHYWHEN) - 1))
					write_reason(rtc, client_socket, request);
				else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
					warnlog(rtc, "receive failed");
			}
			if (close(client_socket))
				warnlog(rtc, "socket close");
			break;
		}
		default:
			abort();
		}
	}
	abort();
}

static void stop_server(struct runtime_config *restrict rtc)
{
#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STOPPING=1");
#endif
	if (rtc->ring_initialized) {
		io_uring_queue_exit(&rtc->ring);
		rtc->ring_initialized = 0;
	}
	if (rtc->ipc_mq >= 0) {
		mq_close(rtc->ipc_mq);
		mq_unlink(rtc->mq_name);
		rtc->ipc_mq = -1;
	}
	if (rtc->listen_fd >= 0) {
		close(rtc->listen_fd);
		rtc->listen_fd = -1;
	}
	if (rtc->signal_fd >= 0) {
		close(rtc->signal_fd);
		rtc->signal_fd = -1;
	}
	if (rtc->res)
		freeaddrinfo(rtc->res);
	close_pid_file(rtc);
	if (rtc->pid_file && access(rtc->pid_file, F_OK)) {
		open_pid_file(rtc);
		update_pid_file(rtc, rtc->s);
		close_pid_file(rtc);
	}
	free(rtc->pid_file);
	free(rtc->mq_name);
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service stopped", "MESSAGE_ID=%s",
			SD_ID128_CONST_STR(MESSAGE_STOP_START), "PRIORITY=%d", LOG_INFO, NULL);
#else
	syslog(LOG_INFO, "service stopped");
	closelog();
#endif
}

void start_server(struct runtime_config *restrict rtc)
{
	int queue_ret;
	sigset_t mask;
	struct state_info buf;
	struct mq_attr attr = {.mq_maxmsg = 5,.mq_msgsize = sizeof(buf) };
#ifdef HAVE_LIBSYSTEMD
	const int ret = sd_listen_fds(0);
#endif
	rtc->listen_fd = -1;
	rtc->signal_fd = -1;
	rtc->ipc_mq = -1;
	/* read previous state and reason */
	clock_gettime(CLOCK_REALTIME, &rtc->previous_change);
	memcpy(rtc->current[rtc->s].reason, "<program started>", 18);
	read_status_from_file(rtc);
	if (add_tstamp_to_reason(rtc, rtc->s))
		exit(EXIT_FAILURE);
	open_pid_file(rtc);
	update_pid_file(rtc, rtc->s);

	/* daemonize before creating the event loop and its registered file
	 * descriptors, so the parent process does not retain them */
	if (!rtc->run_foreground) {
		if (daemon(0, 0))
			err(EXIT_FAILURE, "daemon");
		update_pid_file(rtc, rtc->s);
	}

	/* open server listening socket */
#ifdef HAVE_LIBSYSTEMD
	if (ret == 1)
		rtc->listen_fd = SD_LISTEN_FDS_START + 0;
	else if (ret < 0)
		faillog(rtc, "sd_listen_fds() failed");
	else if (1 < ret)
		faillog(rtc, "too many file descriptors received");
	else {
#else
	{
#endif
		const int on = 1;
		if ((rtc->listen_fd = socket(rtc->res->ai_family, SOCK_CLOEXEC | rtc->res->ai_socktype, rtc->res->ai_protocol)) < 0)
			faillog(rtc, "cannot create socket");
		if (setsockopt(rtc->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&on, sizeof(on)))
			faillog(rtc, "cannot set socket options");
		if (bind(rtc->listen_fd, rtc->res->ai_addr, rtc->res->ai_addrlen))
			faillog(rtc, "unable to bind");
		if (listen(rtc->listen_fd, SOMAXCONN))
			faillog(rtc, "unable to listen");
	}
	if (make_socket_none_blocking(rtc, rtc->listen_fd))
		faillog(rtc, "cannot set server socket none-blocking");

	/* setup signalfd */
	sigemptyset(&mask);
#ifdef SIGHUP
	sigaddset(&mask, SIGHUP);
#endif
#ifdef SIGINT
	sigaddset(&mask, SIGINT);
#endif
#ifdef SIGQUIT
	sigaddset(&mask, SIGQUIT);
#endif
#ifdef SIGTERM
	sigaddset(&mask, SIGTERM);
#endif
#ifdef SIGUSR1
	sigaddset(&mask, SIGUSR1);
#endif
#ifdef SIGUSR2
	sigaddset(&mask, SIGUSR2);
#endif
	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
		faillog(rtc, "sigprocmask");
	if ((rtc->signal_fd = signalfd(-1, &mask, SFD_CLOEXEC)) < 0)
		faillog(rtc, "signalfd");

	/* setup IPC used for state changes */
	if ((rtc->ipc_mq = mq_open(rtc->mq_name, O_CREAT | O_RDONLY | O_CLOEXEC, 0600, &attr)) == (mqd_t) - 1)
		faillog(rtc, "could not create message queue");

	/* initialize io_uring after daemonizing and opening permanent sources */
	if ((queue_ret = io_uring_queue_init(IO_URING_QUEUE_DEPTH, &rtc->ring, 0)) < 0) {
		errno = -queue_ret;
		faillog(rtc, "io_uring queue initialization failed");
	}
	rtc->ring_initialized = 1;
	queue_ret = submit_poll_event(rtc, rtc->listen_fd, EVENT_DATA(EVENT_LISTENER));
	if (queue_ret < 0) {
		errno = -queue_ret;
		faillog(rtc, "io_uring poll_add listener failed");
	}
	queue_ret = submit_poll_event(rtc, rtc->signal_fd, EVENT_DATA(EVENT_SIGNAL));
	if (queue_ret < 0) {
		errno = -queue_ret;
		faillog(rtc, "io_uring poll_add signal failed");
	}
	queue_ret = submit_poll_event(rtc, rtc->ipc_mq, EVENT_DATA(EVENT_IPC));
	if (queue_ret < 0) {
		errno = -queue_ret;
		faillog(rtc, "io_uring poll_add message queue failed");
	}

	/* tell systemd the software has started */
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service started", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START), "STATE=%s",
			state_message[rtc->current[rtc->s].state], "PRIORITY=%d", LOG_INFO, NULL);
	sd_notify(0, "READY=1");
#else
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "started in state %s", state_message[rtc->current[rtc->s].state]);
#endif

	/* stay in event loop */
	wait_events(rtc);
	/* until it is time to stop the service */
	stop_server(rtc);
}
