/* ctl_common.h - what scubed3ctl and keystorectl share
 *
 * Copyright (C) 2019  Rik Snel <rik@snel.it>
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
#ifndef INCLUDE_SCUBED3_CTL_COMMON_H
#define INCLUDE_SCUBED3_CTL_COMMON_H 1

#include <stddef.h>
#include "hashtbl.h"
#include "keystore.h"

/* scubed3ctl talks to the daemon (partitions, LUKS) and reads the key
 * store, one view at a time; keystorectl changes the key store (in memory,
 * save writes it): the options, the input of passphrases, the KDF, the
 * configuration file, the protocol of the daemon and the commands at the
 * prompt (or -c) are the same, they are here */

#define BUF_SIZE 1024

#define DEFAULT_KDF_HASH		"SHA256"
#define	DEFAULT_KDF_SALT		"scubed3_prod"
#define DEFAULT_KDF_ITERATIONS		16777216
#define DEFAULT_KDF_FUNCTION		"PBKDF2"
#define DEFAULT_CIPHER_STRING		"CBC_ESSIV(AES256)"
#define DEFAULT_CONFIG_FILE		"/etc/scubed3.conf"
#define DEFAULT_KEYSTORE_FILE		"/etc/scubed3.keys"
#define KEY_LENGTH	32

#define MAX_RESULT_LINES 128

/* -c COMMAND, more than one: they are done in this order (the key store
 * stays unlocked between them) */
#define MAX_COMMANDS 16

#define MAX_ARGC 10

/* the paranoia level that is required before a partition is written to by
 * mount, mke2fs or luksformat (paranoia=LEVEL in the configuration file) */
#define DEFAULT_PARANOIA		2

/* the name of the device mapper device of a partition with LUKS on it:
 * the prefix (luks_prefix= in the configuration file, the debian-installer
 * makes it HOSTNAME-, as LVM names the volumes of HOSTNAME-vg) and the name
 * of the partition */
#define LUKS_NAME	"%s%s"

extern const char *command_options[MAX_COMMANDS];
extern int no_commands;
extern const char *kdf_salt;
extern const char *control_socket;
extern unsigned long kdf_iterations;
extern int recommended_paranoia;
extern char luks_prefix[65];
extern keystore_t ks;
extern int assume_yes;
extern int key_input;
extern int exit_status;
/* at the prompt (not with -c) */
extern int interactive;

typedef struct result_s {
	char buf[BUF_SIZE];
	char *argv[MAX_RESULT_LINES];
	int argc;
	int status;
} result_t;

/* the answer of the daemon to the last command */
extern result_t result;

typedef struct ctl_priv_s {
	int s;			/* the control socket, -1 if not connected */
	hashtbl_t c;		/* the commands */
	char *mountpoint;	/* of the daemon, NULL if not connected */
	int no_macroblocks;
	int forward;		/* other commands go to the daemon */
} ctl_priv_t;

typedef struct ctl_command_s {
	hashtbl_elt_t head;
	int (*command)(ctl_priv_t*, char**);
	int argc;
	int optargc; /* optional arguments after argc, argv ends with NULL */
	char *usage;
} ctl_command_t;

/* the options (-C -s -i -a -h -Y -c -v -d -q -K -k), the configuration
 * file, mlockall, libgcrypt, the signals; help shows the usage */
void ctl_init(int argc, char **argv, void (*help)(void));

/* the commands of the program (and the daemon, if priv->forward) */
void ctl_add_commands(ctl_priv_t*, ctl_command_t*, int);
int ctl_call(ctl_priv_t*, char*);

/* connect to the daemon (static-info gives its mount point), 0 on success;
 * with fatal an error ends the program, otherwise -1 (no message) */
int ctl_connect(ctl_priv_t*, int fatal);
void ctl_disconnect(ctl_priv_t*);

int do_server_command(int s, int echo, char *format, ...);
int var_system(const char *format, ...);

/* a line from the user (readline at a terminal), NULL at the end */
char *ctl_readline(const char *prompt);
char *ask_line(const char *prompt);
int ask_yn(const char *prompt);
int yesno(const char *prompt);
int typed_yes(const char *prompt);
void warning(void);
int parse_int(int *r, const char *in);

/* a passphrase (twice if verify), NULL if it could not be read or the two
 * do not match; *len is its length; free_passphrase wipes it */
char *ask_passphrase(const char *prompt, int verify, size_t *len);
void free_passphrase(char *pw, size_t len);
/* PBKDF2 with the salt (and suffix) and the iterations of the
 * configuration */
void derive_key(const char *pw, size_t len, const char *suffix,
		unsigned char *key);

/* cryptsetup with data (a key) on its standard input, the key of the LUKS
 * volume of e (see KS_LUKS_VK) */
int cryptsetup_with_data(const void *data, size_t len, const char *format,
		...);
int cryptsetup_with_key(ks_entry_t *e, const char *format, ...);
/* overwrite the memory file fd (len bytes) with zeroes */
void wipe_fd(int fd, size_t len);
/* has partition name a LUKS header? */
int is_luks(ctl_priv_t*, const char *name);

/* the key store: read it (1, or 0 with a message), the key of a passphrase
 * (-k: of 64 hexadecimal digits; 1, 0 if it is empty, -1 if it could not
 * be read), the number of entries of a view, its name, a valid name for a
 * partition (or a view), the entry of a partition in the unlocked views
 * (NULL if it is in none, or in two with other keys: *ambiguous), the
 * entries of a view on stdout */
int ks_open(void);
int ks_ask_key(const char *prompt, int verify, unsigned char *key);
int ks_count(int view);
const char *vname(int view);
int ks_valid_name(const char *name);
ks_entry_t *ks_lookup(const char *name, int *ambiguous);
void ks_list(int view);

#endif /* INCLUDE_SCUBED3_CTL_COMMON_H */
