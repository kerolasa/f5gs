/* This is F5 Graceful Scaling helper daemon.
 *
 * Sami Kerola <sami.kerola@sabre.com>
 */

#include "config.h"

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "close-stream.h"
#include "closeout.h"
#include "progname.h"

#define PORT_NUM	    32546	/* f5 == in octal */
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

static int msg_type;
static size_t msg_len;

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
	fprintf(out, " -p, --port <port>    health check tcp port (default: %d)\n", PORT_NUM);
	fprintf(out, " -f, --state <file>   path of the state file (default: %s)\n", "/FIXME/f5gs");
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

	send(client_s, state_messages[msg_type], msg_len, 0);
	/* let the client send, and ignore */
	retcode = recv(client_s, in_buf, IGNORE_BYTES, 0);
	if (retcode < 0)
		printf("recv error\n");
	close(client_s);
	pthread_exit(NULL);
	/* should be impossible to reach */
	return 0;
}

static void catch_signals(int signal)
{
	switch (signal) {
	case SIGUSR1:
		msg_type = STATE_DISABLE;
		break;
	case SIGUSR2:
		msg_type = STATE_MAINTENANCE;
		break;
	case SIGWINCH:
		msg_type = STATE_ENABLE;
		break;
	default:
		/* should be impossible to reach */
		abort();
	}
	msg_len = strlen(state_messages[msg_type]);
}


static void run_server(void)
{
	socklen_t server_s;
	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	socklen_t addr_len;
	unsigned int ids;
	pthread_attr_t attr;
	pthread_t threads;

	msg_type = STATE_UNKNOWN;
	msg_len = strlen(state_messages[STATE_UNKNOWN]);

	if (signal(SIGUSR1, catch_signals) == SIG_ERR ||
	    signal(SIGUSR2, catch_signals) == SIG_ERR ||
	    signal(SIGWINCH, catch_signals) == SIG_ERR)
		err(EXIT_FAILURE, "cannot set signal handler");

	if (!(server_s = socket(AF_INET, SOCK_STREAM, 0)))
		err(EXIT_FAILURE, "cannot create socket");
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT_NUM);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(server_s, (struct sockaddr *)&server_addr, sizeof(server_addr)))
		err(EXIT_FAILURE, "unable to bind");
	if (listen(server_s, PEND_CONNECTIONS))
		err(EXIT_FAILURE, "unable to listen");

	pthread_attr_init(&attr);
	while (1) {
		int client_s;
		addr_len = sizeof(client_addr);
		client_s = accept(server_s, (struct sockaddr *)&client_addr, &addr_len);

		if (client_s < 0)
			err(EXIT_FAILURE, "unable to create socket");
		else {
			ids = client_s;
			pthread_create(&threads, &attr, response_thread, &ids);

		}
	}

	close(server_s);

}

int main(int argc, char **argv)
{
	int c, server = 0;

	static const struct option longopts[] = {
		{"disable", no_argument, NULL, 'd'},
		{"maintenance", no_argument, NULL, 'm'},
		{"enable", no_argument, NULL, 'e'},
		{"server", no_argument, NULL, 's'},
		{"listen", required_argument, NULL, 'l'},
		{"port", required_argument, NULL, 'p'},
		{"state", required_argument, NULL, 'f'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	set_program_name(argv[0]);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "dmesl:p:f:Vh", longopts, NULL)) != -1) {
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
			printf("FIXME: listen");
			break;
		case 'p':
			printf("FIXME: port");
			break;
		case 'f':
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

	if (server)
		run_server();

	return EXIT_SUCCESS;
}
