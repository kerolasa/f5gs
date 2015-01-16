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
#include <sys/epoll.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
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
	struct epoll_event event;
	struct f5gs_action *socket_action = malloc(sizeof(struct f5gs_action));

#ifdef HAVE_TIMERFD_CREATE
	/* FIXME: on older system that do not have timerfd_create()
	 * there is no timeout, that makes this software to be easy to
	 * to DoS.  For example RHEL4 is this sort of system.
	 * Unfortunately I do not have time right now (2015-01-15)
	 * correct this issue; getting bug fix release to correct pid
	 * file handling has greater priority. */
	int tfd;
	struct timespec now;
	struct itimerspec timeout;
	struct f5gs_action *timer_action = malloc(sizeof(struct f5gs_action));
#endif
	if (socket_action == NULL
#ifdef HAVE_TIMERFD_CREATE
		|| timer_action == NULL
#endif
	) {
		warnlog(rtc, "could not allocate memory");
		return;
	}
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
#ifdef HAVE_TIMERFD_CREATE
	if ((tfd = timerfd_create(CLOCK_REALTIME, 0)) < 0) {
		warnlog(rtc, "timerfd_create failed");
		return;
	}
#endif
	event.events = EPOLLIN | EPOLLET;
	event.data.ptr = socket_action;
	socket_action->fd = client_socket;
#ifdef HAVE_TIMERFD_CREATE
	socket_action->p = timer_action;
#endif
	socket_action->is_socket = 1;
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, client_socket, &event) < 0) {
		warnlog(rtc, "epoll_ctl failed");
		return;
	}
#ifdef HAVE_TIMERFD_CREATE
	event.events = EPOLLIN;
	event.data.ptr = timer_action;
	timer_action->fd = tfd;
	timer_action->p = socket_action;
	timer_action->is_socket = 0;
	if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
		warnlog(rtc, "clock_gettime failed");
		return;
	}
	timeout.it_interval.tv_sec = 0;
	timeout.it_interval.tv_nsec = 0;
	timeout.it_value.tv_sec = now.tv_sec + 1;
	timeout.it_value.tv_nsec = now.tv_nsec;
	if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &timeout, NULL) < 0) {
		warnlog(rtc, "timerfd_settime failed");
		return;
	}
	if (epoll_ctl(rtc->epollfd, EPOLL_CTL_ADD, tfd, &event) < 0) {
		warnlog(rtc, "epoll_ctl timeout failed");
		return;
	}
#endif
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

#ifdef HAVE_PTHREAD_SETNAME_NP
	pthread_setname_np(pthread_self(), "handle_requests");
#endif
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
			if (events[i].data.fd == rtc->server_socket) {
				accept_connection(rtc);
				continue;
			}
			struct f5gs_action *action = (struct f5gs_action *) events[i].data.ptr;
			if (action->is_socket)
				write_reason(rtc, action->fd);
#ifdef HAVE_TIMERFD_CREATE
			close(action->p->fd);
			free(action->p);
#endif
			close(action->fd);
			free(action);
		}
	}
	free(events);
	pthread_exit(NULL);
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
		if (pthread_rwlock_wrlock(&rtc->lock)) {
			warnlog(rtc, "could not get state change lock");
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

	if (!rtc->run_foreground) {
		daemonize();
		update_pid_file(rtc);
	}
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

void start_server(struct runtime_config *restrict rtc)
{
	gettimeofday(&rtc->previous_change, NULL);
	memcpy(rtc->current_reason, "<program started>", 18);
	read_status_from_file(rtc);
	if (add_tstamp_to_reason(rtc))
		exit(EXIT_FAILURE);
	open_pid_file(rtc);
	update_pid_file(rtc);
	if (!(rtc->ipc_key = ftok(rtc->pid_file, IPC_MSG_ID)))
		err(EXIT_FAILURE, "ftok failed");
	run_server(rtc);
}
