#define _POSIX_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define IRCDIR      "irc"
#define INFILE      "in"
#define OUTFILE     "out"
#define PING_TMOUT  300

#ifdef PATH_MAX
#define BUF_PATH_LEN PATH_MAX
#else
#define BUF_PATH_LEN 256
#endif

#define BUF_HOST_LEN 64
#define BUF_NICK_LEN 32
#define BUF_CHAN_LEN 50
#define BUF_MESG_LEN 512

#define SERVER_NICK "-!-"
#define SERVER_PORT "6667"
#define SERVER_HOST "irc.freenode.net"

struct channel {
	int fd;
	char name[BUF_CHAN_LEN];
	struct channel *next;
};

static struct channel *channels;

__attribute__((noreturn)) static void err(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static bool read_line(int fd, char *buffer, size_t buffer_len) {
	size_t i = 0;
	for (char c = buffer[i]; i < buffer_len && c != '\n' && c != EOF; buffer[i++] = c)
		if (read(fd, &c, 1) != 1) return false;
	if (buffer[i - 1] == '\n') buffer[i - 1] = '\0';
	if (buffer[i - 2] == '\r') buffer[i - 2] = '\0';
	return true;
}

static int connect_to_irc(const char *host, const char *port) {
	int sockfd = 0;

	struct addrinfo *res;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(host, port, &hints, &res) != 0)
		return 0;

	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
			continue;
		if (connect(sockfd, res->ai_addr, res->ai_addrlen) == 0)
			break;
		close(sockfd);
	}

	if (res)
		freeaddrinfo(res);

	return sockfd;
}

static bool identify(int ircfd, const char *pass, const char *nick, const char *name) {
	char mesg[BUF_MESG_LEN * 3] = "";
	int len = 0;

	if (pass) len += snprintf(mesg, sizeof(mesg), "PASS %s\r\n", pass);

	len += snprintf(mesg + len, sizeof(mesg) - len, "NICK %s\r\n", nick);
	len += snprintf(mesg + len, sizeof(mesg) - len, "USER %s 0 * :%s\r\n", nick, name);

	return len == write(ircfd, mesg, len);
}

static bool create_dirtree(const char *path) {
	struct stat st;
	char p[BUF_PATH_LEN];

	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;

	for (int i = 1, len = snprintf(p, sizeof(p), "%s", path); i < len; ++i)
		if (p[i] == '/') {
			p[i] = '\0';
			if (stat(p, &st) == -1 && mkdir(p, S_IRWXU) == -1)
				err("cannot create directory '%s'\n", p);
			else if (!S_ISDIR(st.st_mode))
				err("file blocking directory creation '%s'\n", p);
			p[i] = '/';
		}

	return mkdir(path, S_IRWXU) == 0;
}

static int open_channel(const char *channel) {
	struct stat st;
	char infile[BUF_PATH_LEN] = INFILE;

	if (channel && *channel != '\0') {
		if (!create_dirtree(channel))
			err("cannot create channel directory '%s'\n", channel);
		snprintf(infile, sizeof(infile), "%s/%s", channel, INFILE);
	}

	if (stat(infile, &st) == -1 && mkfifo(infile, S_IRWXU) == -1)
		err("cannot create channel fifo '%s'\n", infile);
	else if (access(infile, F_OK) == -1)
		err("cannot access channel fifo '%s'\n", infile);

	return open(infile, O_RDONLY|O_NONBLOCK, 0);
}

static bool is_channel(const char *channel) {
	return *channel == '#' || *channel == '+'
		|| *channel == '!' || *channel == '&';
}

static bool to_irc_lower(const char *src, char *dst, size_t dst_len) {
	for (size_t i = 0, len = strlen(src); i < len && i < dst_len - 1; dst[++i] = '\0')
		switch (src[i]) {
			case '\0': /* NUL */
			case '': /* BEL */
			case '\r': /* CR  */
			case '\n': /* LF  */
			case ' ' : /* SP  */
				return false;
			case ',' : return true;
			case '[' : dst[i] = '{'; break;
			case ']' : dst[i] = '}'; break;
			case '\\': dst[i] = '|'; break;
			case '~' : dst[i] = '^'; break;
			default  : dst[i] = tolower(src[i]); break;
		}
	return true;
}

static bool add_channel(const char *channel) {
	struct channel *chan = NULL;
	char channame[BUF_CHAN_LEN] = "";

	if (!to_irc_lower(channel, channame, sizeof(channame)))
		return false;

	for (chan = channels; chan; chan = chan->next)
		if (strcmp(chan->name, channame) == 0)
			return true;

	if (!(chan = calloc(1, sizeof(struct channel))))
		err("cannot allocate for channel '%s'\n", channel);

	if ((chan->fd = open_channel(channel)) == -1)
		err("cannot open channel fifo '%s'\n", channel);

	snprintf(chan->name, sizeof(chan->name), "%s", channame);
	chan->next = channels;
	channels = chan;
	return true;
}

static void remove_channel(const char *channel) {
	struct channel **c = &channels, *r = NULL;
	char channame[BUF_CHAN_LEN] = "";

	to_irc_lower(channel, channame, sizeof(channame));
	while ((r = *c) && strcmp(r->name, channame) != 0) c = &(*c)->next;
	if (!r) return;

	close(r->fd);
	*c = r->next;
	free(r);
}

static void write_out(const char *channel, const char *nick, const char *mesg) {
	char timebuf[strlen("YYYY-MM-DD HH:MM") + 1];
	const time_t t = time(NULL);
	strftime(timebuf, sizeof(timebuf), "%F %R", localtime(&t));

	char outpath[BUF_PATH_LEN] = OUTFILE;
	if (*channel != '\0') snprintf(outpath, sizeof(outpath), "%s/%s", channel, OUTFILE);

	FILE *outfile = fopen(outpath, "a");
	if (!outfile) add_channel(channel);
	if (!outfile && !(outfile = fopen(outpath, "a"))) return;

	fprintf(outfile, "%s <%s> %s\n", timebuf, nick, mesg);
	fclose(outfile);
}

static int handle_raw(__attribute__((unused)) const char *channel, const char *input, char *mesg, const int mesg_len) {
	return snprintf(mesg, mesg_len, "%s\r\n", input + 1);
}

static int handle_priv(const char *channel, const char *input, char *mesg, const int mesg_len) {
	return snprintf(mesg, mesg_len, "PRIVMSG %s :%s\r\n", channel, input);
}

static int handle_away(__attribute__((unused)) const char *channel, const char *params, char *mesg, const int mesg_len) {
	return (*params == '\0') ? snprintf(mesg, mesg_len, "AWAY\r\n") : snprintf(mesg, mesg_len, "AWAY :%s\r\n", params + 1);
}

static int handle_nick(__attribute__((unused)) const char *channel, const char *params, char *mesg, const int mesg_len) {
	return (*params == '\0') ? 0 : snprintf(mesg, mesg_len, "NICK %s\r\n", params + 1);
}

static int handle_join(__attribute__((unused)) const char *channel, const char *params, char *mesg, const int mesg_len) {
	if (*params == '\0') return 0;
	char *msgkey = strchr(params + 1, ' ');
	if (msgkey) *(msgkey++) = '\0'; else msgkey = "";

	if (is_channel(params + 1)) return snprintf(mesg, mesg_len, "JOIN %s %s\r\n", params + 1, msgkey);

	add_channel(params + 1);
	return (msgkey) ? handle_priv(params + 1, msgkey, mesg, mesg_len) : 0;
}

static int handle_leave(const char *channel, const char *params, char *mesg, const int mesg_len) {
	return strcmp(channel, "") == 0 ? 0
		: (*params == '\0') ? snprintf(mesg, mesg_len, "PART %s\r\n", channel)
		: snprintf(mesg, mesg_len, "PART %s :%s\r\n", channel, params + 1);
}

static int handle_topic(const char *channel, const char *params, char *mesg, const int mesg_len) {
	return (*params == '\0') ? snprintf(mesg, mesg_len, "TOPIC %s\r\n", channel)
	                         : snprintf(mesg, mesg_len, "TOPIC %s :%s\r\n", channel, params + 1);
}

static int handle_names(const char *channel, __attribute__((unused)) const char *params, char *mesg, const int mesg_len) {
	return snprintf(mesg, mesg_len, "NAMES %s\r\n", channel);
}

static int handle_mode(const char *channel, const char *params, char *mesg, const int mesg_len) {
	return (*params == '\0') ? 0 : snprintf(mesg, mesg_len, "MODE %s %s\r\n", channel, params + 1);
}

static int handle_invit(const char *channel, const char *params, char *mesg, const int mesg_len) {
	char nick[BUF_NICK_LEN];
	if (*params == '\0' || !snprintf(nick, sizeof(nick), "%s", params + 1)) return 0;
	return snprintf(mesg, mesg_len, "INVITE %s %s\r\n", params + 1, channel);
}

static int handle_kick(const char *channel, const char *params, char *mesg, const int mesg_len) {
	return (*params == '\0') ? 0 : snprintf(mesg, mesg_len, "KICK %s %s\r\n", channel, params + 1);
}

static int handle_quit(__attribute__((unused)) const char *channel, const char *params, char *mesg, const int mesg_len) {
	return (*params == '\0') ? snprintf(mesg, mesg_len, "QUIT\r\n") : snprintf(mesg, mesg_len, "QUIT :%s\r\n", params + 1);
}

static int (* const cmd_handle[])(const char *channel, const char *params, char *mesg, const int mesg_len) = {
	['a'] = handle_away,  ['t'] = handle_topic,  ['k'] = handle_kick,
	['n'] = handle_nick,  ['u'] = handle_names,  ['q'] = handle_quit,
	['j'] = handle_join,  ['l'] = handle_leave,  ['p'] = handle_priv,
	['m'] = handle_mode,  ['i'] = handle_invit,  ['r'] = handle_raw,
};

int main(int argc, char *argv[]) {
	char host[BUF_HOST_LEN] = SERVER_HOST;
	char nick[BUF_NICK_LEN] = "";
	char pref[BUF_PATH_LEN] = "";
	char path[BUF_PATH_LEN] = "";
	char *port = SERVER_PORT, *pass = NULL, *name = NULL;

	/* parse args */
	if (argc % 2 == 0) err("missing argument for option '%s'\n", argv[argc-1]);

	for (int i = 1; i < argc && argv[i][0] == '-'; ++i) switch (argv[i][1]) {
		case 's': snprintf(host, sizeof(host), "%s", argv[++i]); break;
		case 'n': snprintf(nick, sizeof(nick), "%s", argv[++i]); break;
		case 'i': snprintf(pref, sizeof(pref), "%s", argv[++i]); break;
		case 'k': pass = getenv(argv[++i]); break;
		case 'f': name = argv[++i]; break;
		case 'p': port = argv[++i]; break;
		default : err("usage: ii [-i <irc-dir>] [-s <server>] [-p <port>] [-n <nick>] [-k <passwd-env-var>] [-f <fullname>]\n");
	}

	/* sanitize args */
	{
		const bool p = *pref != '\0';
		const bool n = *nick != '\0';

		if (!p || !n) {
			struct passwd *pw = getpwuid(getuid());
			if (!pw) err("failed to get passwd file\n");
			if (!p)  snprintf(pref, sizeof(pref), "%s/%s", pw->pw_dir, IRCDIR);
			if (!n)  snprintf(nick, sizeof(nick), "%s", pw->pw_name);
		}

		if (!name) name = nick;

		size_t len = strlen(pref);
		while (pref[len - 1] == '/')
			pref[--len] = '\0';
	}

	/* merge prefix and host in a filesystem path and follow it */
	snprintf(path, sizeof(path), "%s/%s", pref, host);
	if (!create_dirtree(path)) err("cannot create main directory '%s'\n", path);
	if (chdir(path) == -1) err("cannot change working directory to '%s'\n", path);

	/*
	 * connect to host
	 * create main/master/server channel
	 * identify and auth to services
	 */
	const int ircfd = connect_to_irc(host, port);
	if (ircfd <= 0) err("cannot connect to '%s:%s'\n", host, port);
	if (!add_channel("")) err("cannot create main channel\n");
	if (!identify(ircfd, pass, nick, name)) err("cannot identify - message cropped.\n");

	/*
	 * listen on the descriptors
	 * handle server messages and channel input
	 * keep connection alive
	 */
	time_t last_response = 0;
	for (bool running = true; running;) {
		struct timeval tv = { .tv_sec = PING_TMOUT / 3, .tv_usec = 0 };
		int maxfd = ircfd;
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(ircfd, &fds);

		for (struct channel *c = channels; c; FD_SET(c->fd, &fds), c = c->next)
			if (maxfd < c->fd) maxfd = c->fd;

		switch (select(maxfd + 1, &fds, 0, 0, &tv)) {
			case -1:
				if (errno != EINTR) err("cannot multiplex selected descriptors (max '%d')\n", maxfd);
				break;
			case 0:
				if (time(NULL) - last_response >= PING_TMOUT) err("ping timeout\n");
				char ping_mesg[BUF_MESG_LEN] = "";
				const int ping_mesg_len = snprintf(ping_mesg, sizeof(ping_mesg), "PING %s\r\n", host);
				write(ircfd, ping_mesg, ping_mesg_len);
				break;
			default:
				if (FD_ISSET(ircfd, &fds)) {
					last_response = time(NULL);

					char input[BUFSIZ] = "";
					if (!read_line(ircfd, input, sizeof(input))) {
						if (errno != EBADF) close(ircfd);
						err("remote host closed connection\n");
					} else {
						// FIXME debug
						printf("\nircfd input '%s'\n", input);

						/* ** parse irc grammar ** */
						char *prefix = NULL, *prefix_user = NULL, *command = NULL;
						char *params = NULL, *prefix_host = NULL, *trailing = NULL;
						char *middle = NULL, mesg[BUF_MESG_LEN] = "";

						/* prefix always starts with ':' else the first word is a command */
						if (*input == ':') prefix = input + 1; else command = input;
						/* if prefix was there then command is the second word */
						if (prefix && (command = strchr(input, ' '))) *(command++) = '\0';
						/* prefix may contain the [!user]@host */
						if (prefix      && (prefix_host = strchr(prefix, '@'))) *(prefix_host++) = '\0';
						if (prefix_host && (prefix_user = strchr(prefix, '!'))) *(prefix_user++) = '\0';
						/* params is the optional string following the command */
						if (command && (params = strchr(command, ' '))) *(params++) = '\0';
						/* get only the first word from params - rest is middle */
						if ((middle = params) && *params != ':' && (middle = strchr(params, ' '))) *(middle++) = '\0';
						/* params/middle may containg the trailing string which always starts with ':' */
						if (middle && (trailing = strchr(middle, ':'))) *(trailing++) = '\0';

						/* ** handle command ** */
						if (!command || strcmp("PONG", command) == 0) {
							; /* empty - do nothing - skip */
						} else if (strcmp("ERROR", command) == 0) {
							snprintf(mesg, sizeof(mesg), "error: %s", trailing);
						} else if (strcmp("TOPIC", command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s changed topic to: %s", prefix, trailing);
						} else if (strcmp("MODE",  command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s changed mode to: %s", prefix, trailing ? trailing : middle);
						} else if (strcmp("KICK",  command) == 0) {
							*(trailing - 2) = '\0'; /* remove trailing space from nickname */
							snprintf(mesg, sizeof(mesg), "%s has kicked %s from %s (%s)", prefix, middle, params, trailing);
							if (strcmp(nick, middle) == 0) remove_channel(params);
						} else if (strcmp("PART",  command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s has parted %s (%s)", prefix, params, trailing ? trailing : "");
							if (strcmp(nick, prefix) == 0) remove_channel(params);
						} else if (strcmp("JOIN",  command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s has joined %s", prefix, params);
							add_channel(params);
						} else if (strcmp("QUIT",  command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s has quit (%s)", prefix, trailing);
							running = strcmp(nick, prefix) != 0;
						} else if (strcmp("NICK",  command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s changed nick to: %s", prefix, trailing);
							if (strcmp(nick, prefix) == 0) snprintf(nick, sizeof(nick), "%s", trailing);
						} else if (strcmp("PRIVMSG", command) == 0) {
							snprintf(mesg, sizeof(mesg), "%s", trailing);
						} else if (strcmp("PING", command) == 0) {
							const int mesg_len = snprintf(mesg, sizeof(mesg), "PONG %s\r\n", trailing);
							write(ircfd, mesg, mesg_len);
							*mesg = '\0'; /* do not write pong messages to out file */
						} else if (trailing) snprintf(mesg, sizeof(mesg), "%s%s", middle ? middle : "", trailing);

						// FIXME debug
						printf("ircfd pref '%s'\n", prefix);
						printf("ircfd host '%s'\n", prefix_host);
						printf("ircfd user '%s'\n", prefix_user);
						printf("ircfd comm '%s'\n", command);
						printf("ircfd para '%s'\n", params);
						printf("ircfd midd '%s'\n", middle);
						printf("ircfd tail '%s'\n", trailing);
						printf("ircfd mesg '%s'\n", mesg);

						if (*mesg != '\0') {
							/* it is a message from/to a server */
							if (!prefix_host) write_out("", SERVER_NICK, mesg);
							/* it is a public message from/to a channel */
							else if (is_channel(params)) write_out(params, prefix, mesg);
							/* it is a private message from/to a user */
							else write_out(prefix, prefix, mesg);
						}
					}
				}

				for (struct channel *c = channels; c; c = c->next) {
					if(!FD_ISSET(c->fd, &fds)) continue;

					char input[BUFSIZ] = "", mesg[BUF_MESG_LEN] = "";
					const bool r = read_line(c->fd, input, sizeof(input));
					unsigned mesg_len = 0, cmd = input[1];

					// FIXME debug
					printf("\ncfd input '%s'\n", input);

					if (!r) {
						if (errno != EBADF) close(c->fd);
						if ((c->fd = open_channel(c->name)) == -1)
							remove_channel(c->name);
					} else if (input[0] != '/') {
						mesg_len = handle_priv(c->name, input, mesg, sizeof(mesg));
						if (mesg_len) write_out(c->name, nick, input);
					} else if ((input[2] == ' ' || input[2] == '\0') && cmd < sizeof(cmd_handle) && cmd_handle[cmd] != NULL) {
						mesg_len = cmd_handle[cmd](c->name, input + 2, mesg, sizeof(mesg));
					} else {
						mesg_len = handle_raw(c->name, input, mesg, sizeof(mesg));
					}

					// FIXME debug
					printf("cfd mesg '%s'\n", mesg);

					if (sizeof(mesg) <= mesg_len) {
						mesg[sizeof(mesg) - 2] = '\r';
						mesg[sizeof(mesg) - 1] = '\n';
					}

					if (mesg_len > 0) write(ircfd, mesg, mesg_len);
				}
		}
	}

	for (struct channel *next = channels->next; channels; next = (channels = next) ? next->next : NULL) free(channels);
	if (ircfd) close(ircfd);

	return EXIT_SUCCESS;
}

/* vim: set noexpandtab : */
