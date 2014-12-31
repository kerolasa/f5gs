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
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void __attribute__((__noreturn__))
    faillog(struct runtime_config *restrict rtc, const char *restrict msg, ...)
{
	va_list args;
	char *s;
	char buf[256];

	va_start(args, msg);
	if (vasprintf(&s, msg, args) < 0)
		goto fail;
	if (rtc->run_foreground)
		err(EXIT_FAILURE, "%s", s);
	if (strerror_r(errno, buf, sizeof(buf)))
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=%s", s, "STRERROR=%s", buf, "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "%s: %s", s, buf);
#endif
 fail:
	free(s);
	va_end(args);
	stop_server(rtc);
	exit(EXIT_FAILURE);
}

static void warnlog(struct runtime_config *restrict rtc, const char *restrict msg, ...)
{
	va_list args;
	char *s;
	char buf[256];

	va_start(args, msg);
	if (vasprintf(&s, msg, args) < 0)
		goto fail;
	if (rtc->run_foreground)
		warn("%s", s);
	if (strerror_r(errno, buf, sizeof(buf)))
#ifdef HAVE_LIBSYSTEMD
		sd_journal_send("MESSAGE=%s", s, "STRERROR=%s", buf, "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_ERROR),
				"PRIORITY=%d", LOG_ERR, NULL);
#else
		syslog(LOG_ERR, "%s: %s", s, buf);
#endif
 fail:
	free(s);
	va_end(args);
}

static void __attribute__((__noreturn__)) *handle_request(void *voidpt)
{
	struct socket_pass *sp = voidpt;
	char io_buf[IGNORE_BYTES];
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
	pthread_rwlock_rdlock(&(sp->rtc->lock));
	if (send(sp->socket, state_message[sp->rtc->current_state], sp->rtc->message_length, 0) < 0) {
		pthread_rwlock_unlock(&(sp->rtc->lock));
		warnlog(sp->rtc, "send failed");
		goto alldone;
	}
	pthread_rwlock_unlock(&(sp->rtc->lock));
	/* wait a second if client wants more info */
	io_buf[0] = '\0';
	if (setsockopt(sp->socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout))) {
		warnlog(sp->rtc, "setsockopt failed");
		goto alldone;
	}
	if (recv(sp->socket, io_buf, sizeof(io_buf), 0) < 0) {
		if (errno != EAGAIN)
			warnlog(sp->rtc, "receive failed");
		goto alldone;
	}
	if (!strcmp(io_buf, WHYWHEN)) {
		struct timeval now, delta;

		if (send(sp->socket, sp->rtc->current_reason, strlen(sp->rtc->current_reason), 0) < 0) {
			warnlog(sp->rtc, "sending reason failed");
			goto alldone;
		}
		gettimeofday(&now, NULL);
		if (timercmp(&now, &sp->rtc->previous_change, <))
			/* time went backwards, ignore result */ ;
		else {
			timersub(&now, &sp->rtc->previous_change, &delta);
			int len = sprintf(io_buf, "\n%ld days %02ld:%02ld:%02ld ago",
					  delta.tv_sec / SECONDS_IN_DAY,
					  delta.tv_sec % SECONDS_IN_DAY / SECONDS_IN_HOUR,
					  delta.tv_sec % SECONDS_IN_HOUR / SECONDS_IN_MIN,
					  delta.tv_sec % SECONDS_IN_MIN);
			if (send(sp->socket, io_buf, len, 0) < 0)
				warnlog(sp->rtc, "send failed");
		}
	}
alldone:
	close(sp->socket);
	free(sp);
	pthread_exit(NULL);
}

static char *construct_pid_file(struct runtime_config *restrict rtc)
{
	char *path;
	void *p;
	const char *last_slash = strrchr(rtc->state_dir, '/');
	char s[INET6_ADDRSTRLEN];
	int separator = 0, ret;

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
		faillog(rtc, "cannot allocate memory");
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
	rewind(rtc->pid_filefd);
	fprintf(rtc->pid_filefd, "%u %d %d\n", getpid(), rtc->current_state, STATE_FILE_VERSION);
	fprintf(rtc->pid_filefd, "%ld.%ld:%s", rtc->previous_change.tv_sec, rtc->previous_change.tv_usec,
		rtc->current_reason + TIME_STAMP_LEN - 1);
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

static void add_tstamp_to_reason(struct runtime_config *restrict rtc)
{
	char explanation[MAX_REASON];
	char *p = explanation;
	time_t prev_c;
	struct tm prev_tm;

	prev_c = rtc->previous_change.tv_sec;
	*p = '\n';
	p++;
	localtime_r(&prev_c, &prev_tm);
	strftime(p, 20, "%Y-%m-%dT%H:%M:%S", &prev_tm);
	p += strlen(p);
	snprintf(p, 7, ",%06d", (int)rtc->previous_change.tv_usec);
	p += strlen(p);
	strftime(p, 7, "%z ", &prev_tm);
	p += strlen(p);
	strcpy(p, rtc->current_reason);
	strcpy(rtc->current_reason, explanation);
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
		len = fread(rtc->current_reason, sizeof(char), sizeof(rtc->current_reason), pidfd);
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
		err(EXIT_FAILURE, "cannot chdir");
	if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}
}

static void *state_change_thread(void *arg)
{
	struct runtime_config *rtc = arg;
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
		add_tstamp_to_reason(rtc);
		update_pid_file(rtc);
		pthread_rwlock_unlock(&rtc->lock);
	}
	pthread_exit(0);
}

static void stop_server(struct runtime_config *restrict rtc)
{
	int qid;

#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STOPPING=1");
#endif
	daemon_running = 0;
	pthread_kill(rtc->chstate_thread, SIGHUP);
	pthread_join(rtc->chstate_thread, NULL);
	pthread_rwlock_destroy(&rtc->lock);
	qid = msgget(rtc->ipc_key, 0600);
	msgctl(qid, IPC_RMID, NULL);
	close(rtc->server_socket);
	freeaddrinfo(rtc->res);
	close_pid_file(rtc);
	open_pid_file(rtc);
	update_pid_file(rtc);
	close_pid_file(rtc);
	free(rtc->pid_file);
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service stopped", "MESSAGE_ID=%s",
			SD_ID128_CONST_STR(MESSAGE_STOP_START), "PRIORITY=%d", LOG_INFO, NULL);
#else
	syslog(LOG_INFO, "service stopped, signal %d", sig);
	closelog();
#endif
}

static void catch_stop(const int sig __attribute__((unused)))
{
	daemon_running = 0;
}

static void run_server(struct runtime_config *restrict rtc)
{
	struct sockaddr_in client_addr;
	socklen_t addr_len;
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
		if (listen(rtc->server_socket, SOMAXCONN))
			err(EXIT_FAILURE, "unable to listen");
	}
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (pthread_rwlock_init(&rtc->lock, NULL))
		err(EXIT_FAILURE, "cannot init read-write lock");

	if (!rtc->run_foreground)
		daemonize();
#ifdef HAVE_LIBSYSTEMD
	sd_journal_send("MESSAGE=service started", "MESSAGE_ID=%s", SD_ID128_CONST_STR(MESSAGE_STOP_START), "STATE=%s",
			state_message[rtc->current_state], "PRIORITY=%d", LOG_INFO, NULL);
	sd_notify(0, "READY=1");
#else
	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "started in state %s", state_message[rtc->current_state]);
#endif
	daemon_running = 1;
	if (pthread_create(&rtc->chstate_thread, NULL, &state_change_thread, (void *)rtc))
		err(EXIT_FAILURE, "could not start state changer thread");
	/* clean up after receiving signal */
	sigemptyset(&sigact.sa_mask);
	sigaction(SIGHUP, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGUSR1, &sigact, NULL);
	sigaction(SIGUSR2, &sigact, NULL);
	while (daemon_running) {
		pthread_t thread;
		struct socket_pass *sp;

		sp = xmalloc(sizeof(struct socket_pass));
		sp->rtc = rtc;
		addr_len = sizeof(client_addr);
		sp->socket = accept(rtc->server_socket, (struct sockaddr *)&client_addr, &addr_len);
		if (sp->socket < 0) {
			free(sp);
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			faillog(rtc, "could not accept connection");
		}
		pthread_create(&thread, NULL, handle_request, sp);
	}
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

static int change_state(struct runtime_config *restrict rtc)
{
	struct state_change_msg buf = {
		.mtype = IPC_MSG_ID,
	};
	int qid;

	buf.info.nstate = rtc->new_state;
	buf.info.uid = getuid();
	buf.info.pid = getpid();
	ttyname_r(STDIN_FILENO, buf.info.tty, TTY_NAME_LEN);
	if (rtc->new_reason)
		memcpy(buf.info.reason, rtc->new_reason, strlen(rtc->new_reason) + 1);
	else
		buf.info.reason[0] = '\0';
	if (run_script(rtc, F5GS_PRE) && !rtc->force)
		return 1;
	if ((rtc->ipc_key = ftok(rtc->pid_file, buf.mtype)) < 0)
		errx(EXIT_FAILURE, "ftok: is f5gs server process running?");
	if ((qid = msgget(rtc->ipc_key, 0600)) < 0)
		err(EXIT_FAILURE, "ipc msgget");
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
	static char buf[sizeof(state_message) + MAX_REASON];
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
		/* this gives handle_request() time to write both status
		 * and reason to socket */
		nanosleep(&waittime, NULL);
	}
	buflen = recv(sfd, buf, sizeof(state_message) + MAX_REASON, 0);
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
	int e, c, server = 0, retval = EXIT_SUCCESS;
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
			if ((MAX_REASON - TIME_STAMP_LEN - 1) < strlen(optarg)) {
				warnx("too long reason, truncating to %d characters", MAX_REASON - TIME_STAMP_LEN - 1);
				optarg[MAX_REASON - TIME_STAMP_LEN - 1] = '\0';
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
	e = getaddrinfo(address, port, &hints, &rtc.res);
	if (e) {
		if (rtc.quiet)
			exit(STATE_UNKNOWN);
		else
			errx(EXIT_FAILURE, "getaddrinfo: %s port %s: %s", address, port, gai_strerror(e));
	}
	rtc.pid_file = construct_pid_file(&rtc);

	if (server) {
		gettimeofday(&rtc.previous_change, NULL);
		memcpy(rtc.current_reason, "<program started>", 18);
		read_status_from_file(&rtc);
		add_tstamp_to_reason(&rtc);
		open_pid_file(&rtc);
		if (!(rtc.ipc_key = ftok(rtc.pid_file, IPC_MSG_ID)))
			err(EXIT_FAILURE, "ftok failed");
		run_server(&rtc);
		return EXIT_SUCCESS;
	} else if (rtc.new_state != STATE_UNKNOWN) {
		struct timespec waittime = {
			.tv_sec = 0L,
			.tv_nsec = 1000000L
		};
		retval = set_server_status(&rtc);
		/* sleep a bit before get_server_status(), else sometimes
		 * server replies using old information */
		nanosleep(&waittime, NULL);
	}

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
	freeaddrinfo(rtc.res);
	free(rtc.pid_file);

	return retval;
}
