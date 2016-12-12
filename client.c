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
#include <mqueue.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <paths.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBSYSTEMD
# include <systemd/sd-daemon.h>
# include <systemd/sd-journal.h>
#else
# include <syslog.h>
#endif

#include "xalloc.h"

#include "f5gs.h"

static int run_script(const struct runtime_config *restrict rtc, const char *restrict script)
{
	pid_t child;
	int status;

	if (rtc->no_scripts || access(script, X_OK))
		return 0;
	child = fork();
	if (child < 0)
		err(EXIT_FAILURE, "cannot fork %s", script);
	if (child == 0) {
		char *p, *copy = NULL;

		if (setuid(geteuid()))
			err(EXIT_FAILURE, "setuid() failed");
		p = getenv("F5GS");
		if (p)
			copy = xstrdup(p);
#ifdef HAVE_CLEARENV
		clearenv();
#else
		environ = NULL;
#endif
		if (setenv("PATH", _PATH_STDPATH, 1) < 0)
			err(EXIT_FAILURE, "cannot setenv");
		if (copy) {
			if (setenv("F5GS", copy, 1) < 0)
				err(EXIT_FAILURE, "cannot setenv");
			free(copy);
		}
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
	struct state_info buf = { 0 };
	const char *msg = (const char *)&buf;
	mqd_t mq_fd;

	mq_fd = mq_open(rtc->mq_name, O_WRONLY | O_CLOEXEC);
	buf.nstate = rtc->new_state;
	buf.uid = getuid();
	buf.pid = getpid();
	switch (ttyname_r(STDIN_FILENO, buf.tty, TTY_NAME_MAX)) {
	case 0:
	case ENOTTY:
		/* nothing */
		break;
	default:
		err(EXIT_FAILURE, "ttyname_r failed");
	}
	if (rtc->new_reason)
		memcpy(buf.reason, rtc->new_reason, strlen(rtc->new_reason) + 1);
	else
		buf.reason[0] = '\0';
	if (run_script(rtc, F5GS_PRE) && !rtc->force)
		return SCRIPT_PRE_FAILED;
	if (mq_send(mq_fd, msg, sizeof(buf), 0) < 0)
		errx(EXIT_FAILURE, "ipc message sending failed");
	mq_close(mq_fd);
	if (run_script(rtc, F5GS_POST) && !rtc->force)
		return SCRIPT_POST_FAILED;
	return SCRIPT_OK;
}

char *get_server_status(const struct runtime_config *restrict rtc)
{
	const struct timeval timeout = {
		.tv_sec = 1L,
		.tv_usec = 0L
	};
	static char buf[CLIENT_SOCKET_BUF];
	ssize_t buflen, msg_len = 0;
	struct pollfd sfd[1];
	int ms = 2;

	if (!(sfd[0].fd = socket(rtc->res->ai_family, rtc->res->ai_socktype, rtc->res->ai_protocol))) {
		if (rtc->quiet)
			exit(STATE_UNKNOWN);
		else
			err(EXIT_FAILURE, "cannot create socket");
	}
	if (setsockopt(sfd[0].fd, SOL_SOCKET, SO_SNDTIMEO | TCP_NODELAY, (void *)&timeout, sizeof(timeout)))
		err(EXIT_FAILURE, "setsockopt failed");
	if (connect(sfd[0].fd, rtc->res->ai_addr, rtc->res->ai_addrlen)) {
		if (rtc->quiet)
			exit(STATE_UNKNOWN);
		else
			err(EXIT_FAILURE, "cannot connect");
	}
	if (rtc->why) {
		if (send(sfd[0].fd, WHYWHEN, sizeof(WHYWHEN), 0) < 0)
			err(EXIT_FAILURE, "sending why request failed");
	}
	sfd[0].events = POLLIN;
	while ((0 < poll(sfd, 1, ms)) && ms < 1025) {
		buflen = recv(sfd[0].fd, buf + msg_len, sizeof(buf) - msg_len, 0);
		if (buflen < 0)
			err(EXIT_FAILURE, "reading socket failed");
		msg_len += buflen;
		ms *= 2;
	}
	buf[msg_len] = '\0';
	close(sfd[0].fd);
	return buf;
}

state_code get_quiet_server_status(const struct runtime_config *restrict rtc)
{
	char *s;

	s = get_server_status(rtc);
	switch (s[0]) {
	case 'e':
		return STATE_ENABLE;
	case 'm':
		return STATE_MAINTENANCE;
	case 'd':
		return STATE_DISABLE;
	case 'u':
		return STATE_UNKNOWN;
	default:
		errx(EXIT_FAILURE, "unexpected server response: %s", s);
	}
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

int verify_server_status(const struct runtime_config *restrict rtc)
{
	int verify_tries = STATE_CHANGE_VERIFY_TRIES;

	while (verify_tries--) {
		const struct timespec waittime = {
			.tv_sec = 0L,
			.tv_nsec = 10000000L
		};

		if (get_quiet_server_status(rtc) == rtc->new_state)
			return EXIT_SUCCESS;
		nanosleep(&waittime, NULL);
	}
	warnx("state change verification failed");
	return EXIT_FAILURE;
}

int set_server_status(struct runtime_config *restrict rtc)
{
	char *username, *sudo_user;
	int retval = EXIT_SUCCESS;

	switch (change_state(rtc)) {
	case SCRIPT_OK:
		break;
	case SCRIPT_PRE_FAILED:
		errx(EXIT_FAILURE, "consider running with --no-scripts or --force");
	case SCRIPT_POST_FAILED:
		warnx("post script failed");
		retval = EXIT_FAILURE;
		break;
	default:		/* should be impossible */
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
	retval |= verify_server_status(rtc);
	return retval;
}
