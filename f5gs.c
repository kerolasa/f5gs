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
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "close-stream.h"
#include "closeout.h"
#include "progname.h"

#include "f5gs.h"

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
			if (REASON_TEXT <= strlen(optarg)) {
				warnx("too long reason, truncating to %d characters", REASON_TEXT - 1);
				optarg[REASON_TEXT - 1] = '\0';
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
		start_server(&rtc);
		return EXIT_SUCCESS;
	}
	/* change server state */
	if (rtc.new_state != STATE_UNKNOWN) {
		int verify_tries = STATE_CHANGE_VERIFY_TRIES;

		set_server_status(&rtc);
		retval = EXIT_FAILURE;
		while (verify_tries--) {
			const struct timespec waittime = {
				.tv_sec = 0L,
				.tv_nsec = 10000000L
			};

			if (get_quiet_server_status(&rtc) == rtc.new_state) {
				retval = EXIT_SUCCESS;
				break;
			}
			nanosleep(&waittime, NULL);
		}
		if (retval != EXIT_SUCCESS)
			warnx("state change verification failed");
	}
	/* request server state */
	if (rtc.quiet)
		retval = get_quiet_server_status(&rtc);
	else {
		if (address)
			printf("%s: ", address);
		printf("current status is: %s\n", get_server_status(&rtc));
	}
	/* clean up and exit */
	freeaddrinfo(rtc.res);
	free(rtc.pid_file);
	return retval;
}
