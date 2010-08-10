/* scubed3ctl.c - scubed3 control program
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <limits.h>
#undef NDEBUG /* we need sanity checking */
#include <readline/readline.h>
#include <readline/history.h>
#include <termios.h>

#include <assert.h>
#include "config.h"
#include "verbose.h"
#include "control.h"
#include "gcry.h"

#define BUF_SIZE 1024
#define CONV_SIZE 512

#define DEFAULT_PASSPHRASE_HASH		"SHA256"
#define DEFAULT_CIPHER_STRING		"CBC_ESSIV(AES256)"
#define KEY_LENGTH	32

#define MAX_RESULT_LINES 128

typedef struct result_s {
	char buf[BUF_SIZE];
	char *argv[MAX_RESULT_LINES];
	int argc;
	int status;
} result_t;

result_t result;

int vvar_system(const char *format, va_list ap) {
	char *string;
	ssize_t len;
	int ret;

	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}

	ret = system(string);

	wipememory(string, len);

	free(string);
	return ret;
	
}

int var_system(const char *format, ...) {
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vvar_system(format, ap);
	va_end(ap);

	return ret;
}

int vwrite_line(int s, const char *format, va_list ap) {
	char *string;
	ssize_t sent = 0, len, n;
	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}

	//VERBOSE("send: %s", string);

	string[len++] = '\n';

	do {
		n = send(s, string + sent, len - sent, 0);
		if (n == 0) {
			wipememory(string, len);
			FATAL("send: connection reset by peer");
		}
		if (n < 0) {
			wipememory(string, len);
			FATAL("send: %s", strerror(errno));
		}
		sent += n;
	} while (sent < len);

	wipememory(string, len);

	free(string);
	return 0;
	
}

#if 0
int write_line(int s, const char *format, ...) {
	int ret;
	va_list ap;

	va_start(ap, format);
	ret = vwrite_line(s, format, ap);
	va_end(ap);

	return ret;
}
#endif

int do_server_command(int s, int echo, char *format, ...) {
	int ret = 0, buf_len = 0, start = 0, i, done = 0;
	int status_known = 0;
	ssize_t  n;
	va_list ap;

	va_start(ap, format);
	ret = vwrite_line(s, format, ap);
	va_end(ap);

	if (ret) return ret;

	result.status = 0;
	result.argc = 0;
	do {
		if (buf_len == BUF_SIZE) FATAL("response too long");
		n = recv(s, result.buf + buf_len, BUF_SIZE - buf_len, 0);
		if (n == 0) FATAL("recv: connection reset by peer");
		if (n < 0) FATAL("recv: %s", strerror(errno));

		for (i = buf_len; i < buf_len + n; i++) {
			if (result.buf[i] == '\n') {
				result.buf[i] = '\0';
				if (*(result.buf + start) == '.' &&
						i - start == 1) {
					done = 1;
					if (!status_known)
						WARNING("message terminates "
								"without known "
								"status");
					break;
				}
				if (status_known) {
					if (echo) printf("%s\n",
							result.buf + start);
					if (result.argc == MAX_RESULT_LINES) FATAL("too many lines in response");
					result.argv[result.argc++] = result.buf + start;
				} else if (!strcmp(result.buf + start, "ERR") ||
						!strcmp(result.buf + start, "OK")) {
					status_known = 1;
					if (!strcmp(result.buf + start, "ERR")) {
						ret = -1;
						result.status = -1;
					}
				} else {
					ERROR("malformed response, "
						"expected OK or ERR");
					return -1;
				}

				start = i + 1;
			}
		}

		buf_len += n;
	} while (!done);

	return 0;
}

#define MAX_ARGC 10

typedef struct ctl_priv_s {
	int s;
	hashtbl_t c;
	char *mountpoint;
	int no_macroblocks;
} ctl_priv_t;

typedef struct ctl_command_s {
	hashtbl_elt_t head;
	int (*command)(ctl_priv_t*, char**);
	int argc;
	char *usage;
} ctl_command_t;

int do_local_command(ctl_priv_t *priv, ctl_command_t *cmnd, char *format, ...) {
	int argc = 0, ret;
	char *args;
	char *argv[MAX_ARGC+1];
	va_list ap;
	
	va_start(ap, format);
	ret = vasprintf(&args, format, ap);
	va_end(ap);

	if (ret == -1) return -1;
	
	argv[argc] = args;

	while (argv[argc] != '\0') {
		while (*argv[argc] == ' ') argv[argc]++;

		argc++;

		if (argc > MAX_ARGC) {
			printf("too many arguments, discarding command\n");
			free(args);
			return 0;
		}

		argv[argc] = argv[argc-1];

		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

		if (argv[argc] == argv[argc-1]) {
			argc--; // last arg empty
			break;
		}

		if (*argv[argc] == ' ') *argv[argc]++ = '\0';
	}
	
	if (argc != cmnd->argc) {
		printf("usage: %s%s\n", cmnd->head.key, cmnd->usage);
		free(args);
		return 0;
	}

	ret = (*cmnd->command)(priv, argv);
	free(args);
	return ret;
}

int my_getpass(char **lineptr, size_t *n, FILE *stream) {
	struct termios old, new;

	/* Turn echoing off and fail if we can't. */
	if (tcgetattr (fileno(stream), &old) != 0) return -1;

	new = old;
	new.c_lflag &= ~ECHO;
	if (tcsetattr(fileno(stream), TCSAFLUSH, &new) != 0)
		return -1;

	/* Read the password. */
	*n = getline (lineptr, n, stream);

	/* Restore terminal. */
	(void) tcsetattr (fileno (stream), TCSAFLUSH, &old);

	printf("\n");

	return 0;
}

static int ctl_open_create_common(ctl_priv_t *priv, char *argv[], int create) {
	uint8_t *hash;
	char *pw = NULL, *pw2 = NULL;
	size_t pw_len, pw2_len;
	int i, ret;
	int algo;
	gcry_md_hd_t hd;

	if (do_server_command(priv->s, 1, "check-available %s", argv[0]))
		return -1;
	if (result.status == -1) return 0;

	algo = gcry_md_map_name("SHA256");
	assert(algo && gcry_md_get_algo_dlen(algo));
	printf("Enter passphrase: ");
	if (my_getpass(&pw, &pw_len, stdin) == -1) {
		ERROR("unable to get password");
		return -1;
	}
	if (create) {
		printf("Verify passphrase: ");
		if (my_getpass(&pw2, &pw2_len, stdin) == -1) {
			wipememory(pw, pw_len);
			free(pw);
			ERROR("unable to get password for verification");
			return -1;
		}
		if (pw_len != pw2_len || strcmp(pw, pw2)) {
			wipememory(pw, pw_len);
			wipememory(pw2, pw2_len);
			free(pw);
			free(pw2);
			printf("passphrases do not match\n");
			return 0;
		}
				
		wipememory(pw2, pw2_len);
		free(pw2);
	}
	
	gcry_call(md_open, &hd, algo, GCRY_MD_FLAG_SECURE);
	assert(gcry_md_is_secure(hd));

	gcry_md_write(hd, pw, pw_len - 1);
	wipememory(pw, pw_len);
	free(pw);

	hash = gcry_md_read(hd, algo);
	assert(hash);
	
	char hash_text[2*gcry_md_get_algo_dlen(algo)], *ptr = hash_text;

	for (i = 0; i < gcry_md_get_algo_dlen(algo); i++)
		ptr += snprintf(ptr, 3, "%02x", hash[i]);
			
	gcry_md_close(hd);

	ret = do_server_command(priv->s, 1, "%s-internal %s %s %.*s",
			create?"create":"open", argv[0],
			DEFAULT_CIPHER_STRING, sizeof(hash_text),
			hash_text);

	wipememory(hash_text, sizeof(hash_text));

	return ret;
}

static int ctl_open(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 0);
}

static int ctl_create(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 1);
}

int yesno() {
	//char answer[4];
	char *answer;
label:
	answer = readline("");
	//fgets(answer, sizeof(answer), stdin);
	if (answer[0] == '\0' || !strcmp("No", answer)) {
		free(answer);
		return 0; /* No */
	}
	if (strcmp("Yes", answer)) {
		printf("Please answer Yes or No. [No] ");
		free(answer);
		goto label;
	}
	free(answer);
	return 1; /* Yes */
}

void warning(void) {
	printf("---WARNING---WARNING---WARNING---WARNING"
			"---WARNING---WARNING---WARNING---\n");
}


static int parse_int(int *r, const char *in) {
	char *end;
	int ret = strtol(in, &end, 10);

	if (errno && (ret == LONG_MIN || ret == LONG_MAX)) {
		printf("integer out of range\n");
		return -1;
	}
	if (*end != '\0') {
		printf("unable to parse ->%s<-\n", in);
		return -1;
	}
	*r = ret;

	return 0;
}

static int parse_info(ctl_priv_t *priv, int no, ...) {
	int done[no], *ptrs[no], i, j;
	char *names[no], *is;
	va_list ap;

	va_start(ap, no);
	for (i = 0; i < no; i++) {
		done[i] = 0;
		names[i] = va_arg(ap, char*);
		ptrs[i] = va_arg(ap, int*);
	}
	va_end(ap);

	for (j = 0; j < result.argc; j++) {
		if (!(is = strchr(result.argv[j], '='))) {
			printf("malformed response from server\n");
			return -1;
		}

		*(is++) = '\0';

		for (i = 0; i < no; i++) {
			if (!strcmp(result.argv[j], names[i])) {
				if (done[i]) {
					printf("double response from the server");
					return -1;
				}
				
				if (parse_int(ptrs[i], is)) return -1;

				done[i] = 1;

				continue;
			}
		}
	}

	return 0;
}

#define DEFAULT_KEEP_REVISIONS 3
#define DEFAULT_INCREMENT 4
#define DEFAULT_RESERVED_MACROBLOCKS (DEFAULT_KEEP_REVISIONS + DEFAULT_INCREMENT)

static int ctl_resize(ctl_priv_t *priv, char *argv[]) {
	int no_macroblocks, new, reserved_macroblocks, keep_revisions;

	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		return 0;
	}
	
	if (parse_info(priv, 3, "no_macroblocks",
				&no_macroblocks, "reserved_macroblocks",
				&reserved_macroblocks, "keep_revisions",
				&keep_revisions)) return 0;

	if (parse_int(&new, argv[1])) return 0;

	if (no_macroblocks == 0 && reserved_macroblocks == 0 &&
			keep_revisions == 0) {
		reserved_macroblocks = (new < DEFAULT_RESERVED_MACROBLOCKS)?
			new:DEFAULT_RESERVED_MACROBLOCKS;
		keep_revisions = (reserved_macroblocks - DEFAULT_INCREMENT <
				DEFAULT_KEEP_REVISIONS)?reserved_macroblocks -
		       	DEFAULT_INCREMENT:DEFAULT_KEEP_REVISIONS;
		if (keep_revisions < 0) keep_revisions = 0;
	}

	if (new == no_macroblocks) {
		printf("size already is %d\n", new);
		return 0;
	}

	if (new <= 0) {
		printf("you cannot resize below 1\n");
		return 0;
	}

	if (new > priv->no_macroblocks) {
		printf("there are only %d macroblocks in total\n",
				priv->no_macroblocks);
		return 0;
	}

	if (new < reserved_macroblocks) {
		printf("* we need to decrease the amount of reserved "
				"macroblocks by %d to %d\n",
				reserved_macroblocks - new, new);

		reserved_macroblocks = new;
	}

	if (reserved_macroblocks - DEFAULT_INCREMENT < keep_revisions) {
		int nr = reserved_macroblocks - DEFAULT_INCREMENT;
		if (nr < 0) nr = 0;

		if (keep_revisions != 0) printf("* we need to decrease "
				"the amount of kept revisions to %d\n", nr);

		keep_revisions = nr;
	}

	if (new > no_macroblocks) {
		warning();
		printf("allocating %d blocks for %s from the unclaimed pool, "
				"this is\n", new - no_macroblocks, argv[0]);
		printf("only safe if ALL your scubed3 partitions "
				"are open, continue? [No] ");

		if (!yesno()) return 0;
	} else {
		assert(no_macroblocks > new);
		warning();
		printf("removing %d blocks from the end of %s, those blocks\n",
				no_macroblocks - new, argv[0]);
		printf("will be added to the unclaimed pool, if you have a "
				"filesystem on it");
		printf("you must resize it yourself continue? [No] ");

		if (!yesno()) return 0;
	}

	if (new < no_macroblocks) {
		printf("shrinking a partition is not yet supported\n");
		return 0;
	}

	return do_server_command(priv->s, 1, "resize-internal %s %d %d %d",
			argv[0], new, reserved_macroblocks,
			keep_revisions);
}

static int ctl_mount(ctl_priv_t *priv, char *argv[]) {
	if (*argv[1] != '/') {
		printf("mountpoint must start with a slash");
		return 0;
	}
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		// we try to open the partition
		do_local_command(priv, hashtbl_find_element_bykey(&priv->c, "open"), "%s", argv[0]);
		//if (ctl_open(priv, argv)) return -1;
		if (result.status) return 0;
		if (do_server_command(priv->s, 0, "set-close-on-release %s 1",
				argv[0])) return -1;
		if (result.status) return 0;
	} 

	if (!var_system("mount -o loop %s/%s %s", priv->mountpoint,
				argv[0], argv[1])) { // ok
		if (do_server_command(priv->s, 0, "set-aux %s mountpoint %s",
					argv[0], argv[1])) return -1;
	}

	return 0;
}

static int ctl_umount(ctl_priv_t *priv, char *argv[]) {
	if (*argv[0] != '/') { // figure out mountpoint
		if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
		if (result.status) {
			printf("%s\n", result.argv[0]);
			return 0;
		}
		var_system("umount %s", result.argv[0]);
		

	} else var_system("umount %s", argv[0]);
	return 0;
}

static ctl_command_t ctl_commands[] = {
	{
		.head.key = "create",
		.command = ctl_create,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "open",
		.command = ctl_open,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "resize",
		.command = ctl_resize,
		.argc = 2,
		.usage = " NAME MACROBLOCKS"
	}, {
		.head.key = "mount",
		.command = ctl_mount,
		.argc = 2,
		.usage = " NAME MOUNTPOINT"
	}, {
		.head.key = "umount",
		.command = ctl_umount,
		.argc = 1,
		.usage = " NAME"
	}
};

int ctl_call(ctl_priv_t *priv, char *command) {
	char *tmp;
	ctl_command_t *cmnd;

	/* strip leading spaces */
	while (*command == ' ' ) command++;
	
	/* check if the command is local */
	if ((tmp = strchr(command, ' '))) *tmp = '\0'; // temprary terminator
	cmnd = hashtbl_find_element_bykey(&priv->c, command);
	if (tmp) *tmp = ' '; // replace space

	/* execute */
	if (cmnd) return do_local_command(priv, cmnd, "%s", tmp);
	else return do_server_command(priv->s, 1, "%s", command);
}

#define NO_COMMANDS (sizeof(ctl_commands)/sizeof(ctl_commands[0]))

int main(int argc, char **argv) {
	ctl_priv_t priv;
	socklen_t len;
	int i, connections = 0;
	int ret = -1;//, set = 1;
	char *line = NULL;
	struct sockaddr_un remote;
	assert(!strcmp("SHA256", DEFAULT_PASSPHRASE_HASH));
	assert(!strcmp("CBC_ESSIV(AES256)", DEFAULT_CIPHER_STRING));

	verbose_init(argv[0]);

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)<0)
		WARNING("failed locking process in RAM (not root?): %s",
				strerror(errno));

	gcry_global_init();

	/* load all command descriptors in hash table */
	hashtbl_init_default(&priv.c, -1, 4, 0, 1, NULL);
	for (i = 0; i < NO_COMMANDS; i++) {
		if (hashtbl_add_element(&priv.c, &ctl_commands[i]) == NULL)
			FATAL("duplicate command");
	}

	signal(SIGPIPE, SIG_IGN);

	if ((priv.s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	//setsockopt(priv.s, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, CONTROL_SOCKET);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(priv.s, (struct sockaddr*)&remote, len) == -1)
		FATAL("connect: %s", strerror(errno));


	if (do_server_command(priv.s, 0, "static-info")) FATAL("unable to "
			"request mountpoint");
	if (result.argc != 3 || result.status == -1)
		FATAL("unexpected reply from server");

	priv.mountpoint = strdup(result.argv[0]);
	if (!priv.mountpoint) FATAL("out of memory");

	if (parse_int(&priv.no_macroblocks, result.argv[1])) 
		FATAL("unable to read the number of macroblocks from the server");

	if (!connections) {
		printf("scubed3ctl-" VERSION ", connected to scubed3-%s\n",
				result.argv[2]);
		printf("default cipher is %s, passphrase hash is %s\n",
				DEFAULT_CIPHER_STRING, DEFAULT_PASSPHRASE_HASH);
	} else {
		printf("re-establised connection\n");
	}

	connections++;

	do {
		if (line) free(line);
		line = readline("s3> ");

		if (!line) {
			printf("\nEOF\n");
			exit(1);
		}

		/* exit is a special case */
		if (!strcmp(line, "exit") || !strcmp(line, "quit") ||
				!strcmp(line, "q") || !strcmp(line, "x") ||
				!strcmp(line, "bye") || !strcmp(line, "kthxbye") ||
				!strcmp(line, "thanks")) {
			ret = do_server_command(priv.s, 1, "exit");
			break;
		} 
		
	} while (!ctl_call(&priv, line));

	free(line);

	free(priv.mountpoint);

	close(priv.s);
	
	exit(0);
}
