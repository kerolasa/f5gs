#ifndef F5GS_HEADER_H
# define F5GS_HEADER_H


# define MESSAGE_ERROR		SD_ID128_MAKE(e7,23,7d,b8,48,ae,40,91,b5,ce,09,b2,eb,b7,59,44)
# define MESSAGE_STATE_CHANGE	SD_ID128_MAKE(74,60,5f,27,15,d3,4b,01,8a,2c,61,c3,7a,99,4c,7b)
# define MESSAGE_STOP_START	SD_ID128_MAKE(f5,eb,95,b2,81,7e,46,69,a8,cc,40,ea,83,94,11,b3)

enum {
	STATE_FILE_VERSION = 0,
	IGNORE_BYTES = 256
};

/* Remember to update manual page if you change --quiet return value(s). */
enum {
	STATE_ENABLE = 0,
	STATE_MAINTENANCE,
	STATE_DISABLE,
	STATE_UNKNOWN
};

/* Remember to update manual page if you change signal(s). */
enum {
	SIG_ENABLE = SIGHUP,
	SIG_MAINTENANCE = SIGUSR1,
	SIG_DISABLE = SIGUSR2
};

static const char *state_message[] = {
	[STATE_ENABLE] = "enable",
	[STATE_MAINTENANCE] = "maintenance",
	[STATE_DISABLE] = "disable",
	[STATE_UNKNOWN] = "unknown"
};

static const int state_signal[] = {
	[STATE_ENABLE] = SIG_ENABLE,
	[STATE_MAINTENANCE] = SIG_MAINTENANCE,
	[STATE_DISABLE] = SIG_DISABLE
};

static const int signal_state[] = {
	[SIG_ENABLE] = STATE_ENABLE,
	[SIG_MAINTENANCE] = STATE_MAINTENANCE,
	[SIG_DISABLE] = STATE_DISABLE
};

struct runtime_config {
	struct addrinfo *res;
	int server_socket;
	pthread_rwlock_t lock;
	int state_code;
	size_t message_lenght;
	char *state_dir;
	char *pid_file;
	char **argv;
	int client_signal;
	unsigned int no_scripts:1,
		     run_foreground:1,
		     quiet:1;
};

static void __attribute__ ((__noreturn__)) usage(FILE *out);
static void __attribute__ ((__noreturn__)) faillog(struct runtime_config *rtc, char *msg, ...);
static void *handle_request(void *voidsocket);
static char *construct_pid_file(struct runtime_config *rtc);
static int update_pid_file(struct runtime_config *rtc);
#ifdef HAVE_SIGNALFD
static void catch_signals(struct signalfd_siginfo *info);
#else
static void catch_signals(int signal);
#endif
static void read_status_from_file(struct runtime_config *rtc);
static void daemonize(void);
static void *signal_handler_thread(void *arg);
static void setup_signal_handling(void);
static void stop_server(int sig __attribute__ ((__unused__)));
static void run_server(struct runtime_config *rtc);
static int run_script(struct runtime_config *rtc, char *script);
static int change_state(struct runtime_config *rtc, pid_t pid);
static char *get_server_status(struct runtime_config *rtc);
static int set_server_status(struct runtime_config *rtc);

#endif				/* F5GS_HEADER_H */
