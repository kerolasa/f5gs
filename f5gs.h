#ifndef F5GS_HEADER_H
#define F5GS_HEADER_H

#define IGNORE_BYTES		256

#define MESSAGE_ERROR		SD_ID128_MAKE(e7,23,7d,b8,48,ae,40,91,b5,ce,09,b2,eb,b7,59,44)
#define MESSAGE_STATE_CHANGE	SD_ID128_MAKE(74,60,5f,27,15,d3,4b,01,8a,2c,61,c3,7a,99,4c,7b)
#define MESSAGE_STOP_START	SD_ID128_MAKE(f5,eb,95,b2,81,7e,46,69,a8,cc,40,ea,83,94,11,b3)

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
