#ifndef F5GS_HEADER_H
# define F5GS_HEADER_H

# include <mqueue.h>

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
	/* message size fragments */
	TSTAMP_NULL = 1,
	TSTAMP_NL = 1,
	TSTAMP_ISO8601 = 19,
	TSTAMP_NSEC = 9,
	TSTAMP_ZONE = 6,
	/* all time stamp fragments together */
	TIME_STAMP_LEN = TSTAMP_NL + TSTAMP_ISO8601 + TSTAMP_NSEC + TSTAMP_ZONE,
	REASON_TEXT = 256,
	/* complete message */
	MAX_MESSAGE = TIME_STAMP_LEN + REASON_TEXT + sizeof(state_message),
	/* receive buffer size */
	CLIENT_SOCKET_BUF = MAX_MESSAGE + TIME_STAMP_LEN,
	/* various */
	NUM_EVENTS = 32,			/* epoll events */
	STATE_CHANGE_VERIFY_TRIES = 64,		/* after state change status check */
	IGNORE_BYTES = 256,			/* amount bytes server will ignore */
	STRERRNO_BUF = 256			/* strerror_r() message buffer size */
};

struct socket_pass {
	struct runtime_config *rtc;		/* run time configuration for socket handler thread */
	int socket;				/* request socket */
};

struct state_msg {
	state_code state;			/* state message code, see earlier enum */
	size_t len;				/* length of the state message */
	char reason[MAX_MESSAGE];		/* --reason content to --why requests */
};

struct runtime_config {
	struct addrinfo *res;			/* connection/listen address of --address */
	int server_socket;			/* listen socket */
	int epollfd;				/* socket epoll() file descriptor */
	pthread_t worker;			/* accepted connection request handler */
	struct state_msg current[2];		/* current and next state message */
	const char *state_dir;			/* directory where state files are wrote */
	char *pid_file;				/* path to the state file for this instance */
	FILE *pid_filefd;			/* open file handle to state file */
	char **argv;				/* command line arguments */
	state_code new_state;			/* state the client attemtps to set */
	char *new_reason;			/* message the client will add to the new state */
	struct timespec previous_change;	/* timestamp of the previous change */
	struct timespec previous_mono;		/* monotonic timestamp of the previous change */
	mqd_t mq;				/* IPC message queue */
	char *mq_name;				/* IPC message queue name, based on listen & port */
	unsigned int
			s:1,			/* current state_mesg structure in use */
			monotonic:1,		/* clock_gettime() is using monotonic time */
			why:1,			/* is --why option in use */
			force:1,		/* is --force option in use */
			no_scripts:1,		/* is --no-scripts option in use */
			run_foreground:1,	/* is --foreground option in use */
			quiet:1;		/* is --quiet option in use */
};

# ifndef TTY_NAME_MAX
#  define TTY_NAME_MAX 32			/* fallback if bits/local_lim.h does not have this */
# endif

struct state_info {				/* IPC message payload */
	state_code nstate;			/* state to be set */
	char reason[MAX_MESSAGE];		/* message accompanied with the state */
	uid_t uid;				/* uid of the process that set the state */
	pid_t pid;				/* pid of the process that set the state */
	char tty[TTY_NAME_MAX];			/* tty of the process that set the state */
};

/* functions that are called a cross files  */
void start_server(struct runtime_config *rtc);
char *get_server_status(const struct runtime_config *rtc);
state_code get_quiet_server_status(const struct runtime_config *rtc);
int set_server_status(struct runtime_config *rtc);

#endif				/* F5GS_HEADER_H */
