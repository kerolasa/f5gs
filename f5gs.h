#ifndef F5GS_HEADER_H
# define F5GS_HEADER_H

# define MESSAGE_ERROR		SD_ID128_MAKE(e7,23,7d,b8,48,ae,40,91,b5,ce,09,b2,eb,b7,59,44)
# define MESSAGE_STATE_CHANGE	SD_ID128_MAKE(74,60,5f,27,15,d3,4b,01,8a,2c,61,c3,7a,99,4c,7b)
# define MESSAGE_STOP_START	SD_ID128_MAKE(f5,eb,95,b2,81,7e,46,69,a8,cc,40,ea,83,94,11,b3)

# define WHYWHEN "info:0"

enum {
	STATE_FILE_VERSION = 1,
	IPC_MSG_ID = 1,
	TIME_STAMP_LEN = 33,
	MAX_REASON = TIME_STAMP_LEN + 255,
	IGNORE_BYTES = 256
};

/* Remember to update manual page if you change --quiet return value(s). */
typedef enum {
	STATE_ENABLE = 0,	/* must have value 0, and first entry */
	STATE_MAINTENANCE,
	STATE_DISABLE,
	STATE_UNKNOWN		/* must be the last entry */
} state_code;

static const char *state_message[] = {
	[STATE_ENABLE] = "enable",
	[STATE_MAINTENANCE] = "maintenance",
	[STATE_DISABLE] = "disable",
	[STATE_UNKNOWN] = "unknown"
};

struct runtime_config {
	struct addrinfo *res;
	int server_socket;
	pthread_rwlock_t lock;
	state_code current_state;
	size_t message_length;
	const char *state_dir;
	char *pid_file;
	char **argv;
	state_code new_state;
	char *new_reason;
	char current_reason[MAX_REASON];
	struct timeval previous_change;
	key_t ipc_key;
	unsigned int
			why:1,
			no_scripts:1,
			run_foreground:1,
			quiet:1;
};

struct state_info {
	state_code nstate;
	char reason[MAX_REASON];
};

struct state_change_msg {
	long mtype;
	struct state_info info;
};

static void __attribute__((__noreturn__)) usage(FILE *out);
static void __attribute__((__noreturn__)) faillog(const struct runtime_config *rtc, const char *msg, ...);
static void *handle_request(void *voidsocket);
static char *construct_pid_file(const struct runtime_config *rtc);
static int update_pid_file(const struct runtime_config *rtc);
static void read_status_from_file(struct runtime_config *rtc);
static void daemonize(void);
static void stop_server(int sig);
static void run_server(struct runtime_config *rtc);
static int run_script(const struct runtime_config *rtc, const char *script);
static int change_state(struct runtime_config *rtc);
static char *get_server_status(const struct runtime_config *rtc);
static int set_server_status(struct runtime_config *rtc);

#endif				/* F5GS_HEADER_H */
