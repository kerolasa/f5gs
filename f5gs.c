/* This is F5 Graceful Scaling helper daemon.
 *
 * Sami Kerola <sami.kerola@sabre.com>
 */

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE             1024
#define PORT_NUM            32546	/* f5 == in octal */
#define PEND_CONNECTIONS      128	/* pending connections to hold */

unsigned int client_s;

/* child thread */
void *response_thread(void *arg)
{
	char in_buf[BUF_SIZE];
	char out_buf[BUF_SIZE];
	unsigned int fh;
	unsigned int buf_len;
	ssize_t retcode;

	/* let the client send, and ignore */
	retcode = recv(client_s, in_buf, BUF_SIZE, 0);

	if (retcode < 0)
		printf("recv error\n");
	else {
		fh = open("/etc/f5gs.conf", O_RDONLY);

		if (fh == -1) {
			strcpy(out_buf, "UNKNOWN\n");
			send(client_s, out_buf, strlen(out_buf), 0);
		} else {
			buf_len = 1;
			while (0 < buf_len) {
				buf_len = read(fh, out_buf, BUF_SIZE);
				if (0 < buf_len)
					send(client_s, out_buf, buf_len, 0);
			}

			close(fh);
		}
	}
        close(client_s);
        pthread_exit(NULL);
        /* should be impossible to reach */
	return 0;
}

int main(int argc, char **argv)
{
	socklen_t server_s;
	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	socklen_t addr_len;
	unsigned int ids;
	pthread_attr_t attr;
	pthread_t threads;

	server_s = socket(AF_INET, SOCK_STREAM, 0);
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT_NUM);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(server_s, (struct sockaddr *)&server_addr, sizeof(server_addr)))
	        err(EXIT_FAILURE, "unable to bind");
	if (listen(server_s, PEND_CONNECTIONS))
	        err(EXIT_FAILURE, "unable to listen");

	pthread_attr_init(&attr);
	while (1) {
		addr_len = sizeof(client_addr);
		client_s =
		    accept(server_s, (struct sockaddr *)&client_addr,
			   &addr_len);

		if (client_s < 0)
			err(EXIT_FAILURE, "unable to create socket");
		else {
			ids = client_s;
			pthread_create(&threads, &attr, response_thread, &ids);

		}
	}

	close(server_s);
	return EXIT_SUCCESS;
}
