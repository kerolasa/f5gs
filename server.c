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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
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

static int make_socket_none_blocking(struct runtime_config *restrict rtc, int socket)
{
	int flags;

	if ((flags = fcntl(socket, F_GETFL)) < 0) {
		warnlog(rtc, "fcntl F_GETFL failed");
		return 1;
	}
	flags |= O_NONBLOCK;
	if (fcntl(socket, F_SETFL, flags) < 0) {
		warnlog(rtc, "fcntl F_SETFL failed");
		return 1;
	}
	return 0;
}

static void accept_connection(struct runtime_config *restrict rtc)
{
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof client_addr;
	struct epoll_event event;
	struct f5gs_action *client_socket = malloc(sizeof(struct f5gs_action));

	if (client_socket == NULL) {
		warnlog(rtc, "could not allocate memory");
		return;
	}
	if ((client_socket->fd = accept(rtc->listen_event->fd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
		warnlog(rtc, "accept failed");
		return;
	}
	if (send(client_socket->fd, state_message[rtc->current[rtc->s].state], rtc->current[rtc->s].len, 0) < 0) {
		warnlog(rtc, "send failed");
		return;
	}
	if (make_socket_none_blocking(rtc, client_socket->fd)) {
		warnlog(rtc, "fcntl none-blocking failed");
		return;
	}
	memset(&event, 0, sizeof event);
	event.events = EPOLLIN | EPOLLONESHOT;
	event.data.ptr = client_socket;
	client_socket->type = EV_CLIENT_SOCKET;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, client_socket->fd, &event) < 0) {
		warnlog(rtc, "epoll_ctl failed");
		return;
	}
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

static void change_state(struct runtime_config *rtc)
{
	int tmp_s;
	struct state_info buf;
	char *msg = (char *)&buf;

	while (mq_receive(rtc->ipc_mq_event->fd, msg, sizeof(buf), NULL) < 0) {
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
	struct epoll_event *events;

	events = xmalloc(NUM_EVENTS * sizeof(struct epoll_event));
	while (!rtc->stop_requested) {
		int nevents, i;

		nevents = epoll_wait(rtc->epollfd, events, NUM_EVENTS, -1);
		if (nevents < 0) {
			if (errno == EINTR)
				continue;
			warnlog(rtc, "epoll_wait failed");
			continue;
		}
		for (i = 0; i < nevents; i++) {
			struct f5gs_action *action;
			struct epoll_event event;

			action = (struct f5gs_action *)events[i].data.ptr;
			switch (action->type) {
			case EV_SERVER_SOCKET:
				accept_connection(rtc);
				break;
			case EV_CLIENT_SOCKET:
				if (action->type == EV_CLIENT_SOCKET)
					write_reason(rtc, action->fd);
				memset(&event, 0, sizeof event);
				if (epoll_ctl(rtc->epollfd, EPOLL_CTL_DEL, action->fd, &event))
					warnlog(rtc, "removing socket epoll");
				if (close(action->fd))
					warnlog(rtc, "socket close");
				free(action);
				break;
			case EV_SIGNAL_FD:
				rtc->stop_requested = 1;
				break;
			case EV_MESSAGE_QUEUE:
				change_state(rtc);
				break;
			default:
				abort();
				break;
			}
		}
	}
	free(events);
	return;
}

static void stop_server(struct runtime_config *restrict rtc)
{
	rtc->stop_requested = 1;
#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STOPPING=1");
#endif
	if (rtc->ipc_mq_event->fd) {
		mq_close(rtc->ipc_mq_event->fd);
		mq_unlink(rtc->mq_name);
	}
	if (rtc->listen_event->fd)
		close(rtc->listen_event->fd);
	if (rtc->signal_event->fd)
		close(rtc->signal_event->fd);
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
	free(rtc->listen_event);
	free(rtc->signal_event);
	free(rtc->ipc_mq_event);
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
	struct epoll_event event;
	sigset_t mask;
	struct state_info buf;
	struct mq_attr attr = {.mq_maxmsg = 5,.mq_msgsize = sizeof(buf) };
#ifdef HAVE_LIBSYSTEMD
	const int ret = sd_listen_fds(0);
#endif
	/* read previous state and reason */
	clock_gettime(CLOCK_REALTIME, &rtc->previous_change);
	memcpy(rtc->current[rtc->s].reason, "<program started>", 18);
	read_status_from_file(rtc);
	if (add_tstamp_to_reason(rtc, rtc->s))
		exit(EXIT_FAILURE);
	open_pid_file(rtc);
	update_pid_file(rtc, rtc->s);

	/* allocate space for events, done before daemon() call so that
	 * xmalloc() messages are not lost */
	rtc->listen_event = xmalloc(sizeof(struct f5gs_action));
	rtc->signal_event = xmalloc(sizeof(struct f5gs_action));
	rtc->ipc_mq_event = xmalloc(sizeof(struct f5gs_action));

	/* daemonize if needed.  if this is moved after epoll_ctl() calls
	 * they start to misbehave (possibly because stdin and such are
	 * closed) */
	if (!rtc->run_foreground) {
		if (daemon(0, 0))
			err(EXIT_FAILURE, "daemon");
		update_pid_file(rtc, rtc->s);
	}

	/* open server listen socket and add epoll */
#ifdef HAVE_LIBSYSTEMD
	if (ret == 1)
		rtc->listen_event->fd = SD_LISTEN_FDS_START + 0;
	else if (ret < 0)
		faillog(rtc, "sd_listen_fds() failed");
	else if (1 < ret)
		faillog(rtc, "too many file descriptors received");
	else {
#else
	{
#endif
		const int on = 1;
		if (!(rtc->listen_event->fd = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol)))
			faillog(rtc, "cannot create socket");
		if (setsockopt(rtc->listen_event->fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on)))
			faillog(rtc, "cannot set socket options");
		if (bind(rtc->listen_event->fd, rtc->res->ai_addr, rtc->res->ai_addrlen))
			faillog(rtc, "unable to bind");
		if (make_socket_none_blocking(rtc, rtc->listen_event->fd))
			faillog(rtc, "cannot set server socket none-blocking");
		if (listen(rtc->listen_event->fd, SOMAXCONN))
			faillog(rtc, "unable to listen");
	}
#ifdef HAVE_EPOLL_CREATE1
	if ((rtc->epollfd = epoll_create1(0)) < 0)
#else
	if ((rtc->epollfd = epoll_create(NUM_EVENTS)) < 0)
#endif
		faillog(rtc, "epoll_create failed");
	memset(&event, 0, sizeof event);
	event.events = EPOLLIN;
	event.data.ptr = rtc->listen_event;
	rtc->listen_event->type = EV_SERVER_SOCKET;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, rtc->listen_event->fd, &event) < 0)
		faillog(rtc, "epoll_ctl add socket failed");

	/* setup signalfd epoll */
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
	memset(&event, 0, sizeof event);
	event.events = EPOLLIN;
	event.data.ptr = rtc->signal_event;
	if ((rtc->signal_event->fd = signalfd(-1, &mask, 0)) < 0)
		faillog(rtc, "signalfd");
	rtc->signal_event->type = EV_SIGNAL_FD;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, rtc->signal_event->fd, &event) < 0)
		faillog(rtc, "epoll_ctl add signal failed");

	/* setup IPC epoll that used for state changes */
	if ((rtc->ipc_mq_event->fd = mq_open(rtc->mq_name, O_CREAT | O_RDONLY, 0600, &attr)) == (mqd_t) - 1)
		faillog(rtc, "could not create message queue");
	event.events = EPOLLIN;
	event.data.ptr = rtc->ipc_mq_event;
	rtc->ipc_mq_event->type = EV_MESSAGE_QUEUE;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, rtc->ipc_mq_event->fd, &event) < 0)
		faillog(rtc, "epoll add message queue failed");

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
