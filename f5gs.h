#ifndef F5GS_HEADER_H
#define F5GS_HEADER_H

#define IGNORE_BYTES		256


enum {
	STATE_DISABLE,
	STATE_MAINTENANCE,
	STATE_ENABLE,
	STATE_UNKNOWN
};

/* Check get_server_status() is valid after changing message text(s). */
static const char *state_messages[] = {
	[STATE_DISABLE] = "disable",
	[STATE_MAINTENANCE] = "maintenance",
	[STATE_ENABLE] = "enable",
	[STATE_UNKNOWN] = "unknown"
};

static const int state_signals[] = {
	[STATE_DISABLE] = SIGUSR1,
	[STATE_MAINTENANCE] = SIGUSR2,
	[STATE_ENABLE] = SIGWINCH
};

struct runtime_config {
	struct addrinfo *res;
	int server_s;
	pthread_rwlock_t lock;
	int msg_type;
	size_t msg_len;
	char *statedir;
	char **argv;
	int send_signal;
};

#endif /* F5GS_HEADER_H */
