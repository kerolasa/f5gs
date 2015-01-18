#ifndef F5GS_HEADER_H
# define F5GS_HEADER_H

# define MESSAGE_ERROR		SD_ID128_MAKE(e7,23,7d,b8,48,ae,40,91,b5,ce,09,b2,eb,b7,59,44)
# define MESSAGE_STATE_CHANGE	SD_ID128_MAKE(74,60,5f,27,15,d3,4b,01,8a,2c,61,c3,7a,99,4c,7b)
# define MESSAGE_STOP_START	SD_ID128_MAKE(f5,eb,95,b2,81,7e,46,69,a8,cc,40,ea,83,94,11,b3)

# define WHYWHEN "info:0"

/* Remember to update manual page if you change --quiet return value(s). */
typedef enum {
	STATE_ENABLE = 0,	/* must have value 0, and first entry */
	STATE_MAINTENANCE,
	STATE_DISABLE,
	STATE_UNKNOWN		/* must be the last entry */
} state_code;

/* Identifiers, versions, and so such. */
enum {
	STATE_FILE_VERSION = 1,
	IPC_MSG_ID = 2,
};

/* Function return values. */
enum {
	SCRIPT_OK = 0,
	SCRIPT_PRE_FAILED,
	SCRIPT_POST_FAILED
};

# define IPC_MODE 0600

static const char *state_message[] = {
	[STATE_ENABLE] = "enable",
	[STATE_MAINTENANCE] = "maintenance",
	[STATE_DISABLE] = "disable",
	[STATE_UNKNOWN] = "unknown"
};

/* Buffer sizes, string lengths, and such.  */
enum {
	TSTAMP_NULL = 1,
	TSTAMP_NL = 1,
	TSTAMP_ISO8601 = 19,
	TSTAMP_USEC = 6,
	TSTAMP_ZONE = 6,
	TIME_STAMP_LEN = TSTAMP_NL + TSTAMP_ISO8601 + TSTAMP_USEC + TSTAMP_ZONE,
	REASON_TEXT = 256,
	MAX_MESSAGE = TIME_STAMP_LEN + REASON_TEXT,

	CLIENT_SOCKET_BUF = sizeof(state_message) + MAX_MESSAGE,

	TTY_NAME_LEN = 32,
	NUM_EVENTS = 32,
	STATE_CHANGE_VERIFY_TRIES = 64,
	IGNORE_BYTES = 256,
	STRERRNO_BUF = 256
};

struct f5gs_action {
	int fd;
	int is_socket;
	struct f5gs_action *p;
};

struct runtime_config {
	struct addrinfo *res;
	int server_socket;
	int epollfd;
	pthread_t worker;
	pthread_rwlock_t lock;
	state_code current_state;
	size_t message_length;
	const char *state_dir;
	char *pid_file;
	FILE *pid_filefd;
	char **argv;
	state_code new_state;
	char *new_reason;
	char current_reason[MAX_MESSAGE];
	struct timeval previous_change;
	key_t ipc_key;
	unsigned int
			why:1,
			force:1,
			no_scripts:1,
			run_foreground:1,
			quiet:1;
};

struct state_info {
	state_code nstate;
	char reason[MAX_MESSAGE];
	uid_t uid;
	pid_t pid;
	char tty[TTY_NAME_LEN];
};

struct state_change_msg {
	long mtype;
	struct state_info info;
};

void start_server(struct runtime_config *rtc);
char *get_server_status(const struct runtime_config *rtc);
state_code get_quiet_server_status(const struct runtime_config *rtc);
void set_server_status(struct runtime_config *rtc);

#endif				/* F5GS_HEADER_H */
