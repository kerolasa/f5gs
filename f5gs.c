/* This is F5 Graceful Scaling helper daemon.
 *
 * Sami Kerola <sami.kerola@sabre.com>
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

#define PORT_NUM	    "32546"	/* f5 == in octal */
#define PEND_CONNECTIONS      128	/* pending connections to hold */
#define IGNORE_BYTES	      256

enum {
	STATE_UNKNOWN,
	STATE_DISABLE,
	STATE_MAINTENANCE,
	STATE_ENABLE
};
static const char *state_messages[] = {
	[STATE_UNKNOWN] = "unknown",
	[STATE_DISABLE] = "disable",
	[STATE_MAINTENANCE] = "maintenance",
	[STATE_ENABLE] = "enable"
};

struct runtime_config {
	struct addrinfo *res;
	int server_s;
	pthread_rwlock_t lock;
	int msg_type;
	size_t msg_len;
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

/* child thread */
static void *response_thread(void *arg)
{
	char in_buf[IGNORE_BYTES];
	ssize_t retcode;
	int client_s = (*(int *)arg);

	if (pthread_rwlock_rdlock(&(rtc.lock))) {
		warn("could not get lock");
		return NULL;
	}
	send(client_s, state_messages[rtc.msg_type], rtc.msg_len, 0);
	pthread_rwlock_unlock(&(rtc.lock));
	/* let the client send, and ignore */
	retcode = recv(client_s, in_buf, IGNORE_BYTES, 0);
	if (retcode < 0)
		printf("recv error\n");
	close(client_s);
	pthread_exit(NULL);
	/* should be impossible to reach */
	return NULL;
}

static void catch_signals(int signal)
{
	if (pthread_rwlock_wrlock(&rtc.lock)) {
		warn("could not get lock");
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
}

static void __attribute__((__noreturn__))
faillog(char *msg)
{
	syslog(LOG_ERR, "%s: %s", msg, strerror(errno));
	exit(EXIT_FAILURE);
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
	if (listen(rtc->server_s, PEND_CONNECTIONS))
		err(EXIT_FAILURE, "unable to listen");
	if (pthread_attr_init(&attr))
		err(EXIT_FAILURE, "cannot init thread attribute");

	if (pthread_rwlock_init(&(rtc->lock), NULL))
		err(EXIT_FAILURE, "cannot init read-write lock");
	rtc->msg_type = STATE_UNKNOWN;
	rtc->msg_len = strlen(state_messages[STATE_UNKNOWN]);

	if (signal(SIGUSR1, catch_signals) == SIG_ERR ||
	    signal(SIGUSR2, catch_signals) == SIG_ERR ||
	    signal(SIGWINCH, catch_signals) == SIG_ERR)
		err(EXIT_FAILURE, "cannot set signal handler");

	daemonize();
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
			syslog(LOG_WARNING, "unable to create socket");
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

int main(int argc, char **argv)
{
	int c, server = 0;
	char *listen = NULL, *port = PORT_NUM;
	struct addrinfo hints;
	int e;
	enum {
		STATEDIR_OPT = CHAR_MAX + 1,
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

	while ((c = getopt_long(argc, argv, "dmesl:p:Vh", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			printf("FIXME: disable");
			break;
		case 'm':
			printf("FIXME: maintenance");
			break;
		case 'e':
			printf("FIXME: enable");
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
			printf("FIXME: state file");
			break;
		case 'V':
			printf("%s version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
			break;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	memset(&rtc, 0, sizeof(struct runtime_config));
	memset(&hints, 0, sizeof(struct addrinfo));

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	e = getaddrinfo(listen, port, &hints, &(rtc.res));
	if (e) {
		warnx("getaddrinfo: %s port %s: %s", listen, port, gai_strerror(e));
		exit(EXIT_FAILURE);
	}
	if (server)
		run_server(&rtc);

	return EXIT_SUCCESS;
}
