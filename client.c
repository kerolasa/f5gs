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
#include <netdb.h>
#include <paths.h>
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
	if (ttyname_r(STDIN_FILENO, buf.info.tty, TTY_NAME_MAX) != 0) {
		if (errno != ENOTTY)
			warn("ttyname_r failed");
	}
	if (rtc->new_reason)
		memcpy(buf.info.reason, rtc->new_reason, strlen(rtc->new_reason) + 1);
	else
		buf.info.reason[0] = '\0';
	if (run_script(rtc, F5GS_PRE) && !rtc->force)
		return SCRIPT_PRE_FAILED;
	if ((rtc->ipc_key = ftok(rtc->pid_file, buf.mtype)) < 0)
		errx(EXIT_FAILURE, "ftok: is f5gs server process running?");
	if ((qid = msgget(rtc->ipc_key, IPC_MODE)) < 0)
		errx(EXIT_FAILURE, "msgget failed: server stopped or a version mismatch?");
	if (msgsnd(qid, (void *)&buf, sizeof(buf.info), 0) != 0)
		err(EXIT_FAILURE, "ipc message sending failed");
	if (run_script(rtc, F5GS_POST) && !rtc->force)
		return SCRIPT_POST_FAILED;
	return SCRIPT_OK;
}

char *get_server_status(const struct runtime_config *restrict rtc)
{
	int sfd;
	const struct timeval timeout = {
		.tv_sec = 1L,
		.tv_usec = 0L
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

state_code get_quiet_server_status(const struct runtime_config *restrict rtc)
{
	char *s;
	state_code i;

	s = get_server_status(rtc);
	for (i = STATE_ENABLE; i <= STATE_UNKNOWN; i++)
		if (!strcmp(s, state_message[i]))
			break;
	return i;
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

void set_server_status(struct runtime_config *restrict rtc)
{
	char *username, *sudo_user;

	switch (change_state(rtc)) {
	case SCRIPT_OK:
		break;
	case SCRIPT_PRE_FAILED:
		errx(EXIT_FAILURE, "consider running with --no-scripts or --force");
	case SCRIPT_POST_FAILED:
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
