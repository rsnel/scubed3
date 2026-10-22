/* scubed3ctl.c - scubed3 control program
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
/* the partitions of the daemon (open, create, resize, LUKS, mount, the
 * paranoia level); the key store is only read, one view is unlocked at a
 * time (its keys are used instead of passphrases), keystorectl changes it */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#undef NDEBUG /* we need sanity checking */
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/loop.h>
#include <assert.h>
#include "config.h"
#include "verbose.h"
#include "gcry.h"
#include "cipher.h"
#include "util.h"
#include "keystore.h"
#include "ctl_common.h"

/* open or create partition name with key */
static int send_key(ctl_priv_t *priv, const char *name,
		const unsigned char *key, int create) {
	char hash_text[2*KEY_LENGTH + 1], *ptr = hash_text;
	int i, ret;

	for (i = 0; i < KEY_LENGTH; i++)
		ptr += snprintf(ptr, 3, "%02x", key[i]);

	ret = do_server_command(priv->s, 1, "%s-internal %s %s %.*s",
			create?"create":"open", name,
			DEFAULT_CIPHER_STRING, 2*KEY_LENGTH,
			hash_text);

	// HERE BE DRAGONS! if sizeof(hash_text) is NOT cast to int, then
	// the compiler 'optimizes' it to 0, that will lead to wipememory
	// attempting to wipe all memory, which will lead to a segfault
	wipememory(hash_text, (int)sizeof(hash_text));

	return ret;
}

/* the key of a partition comes from the key store, if it is in an unlocked
 * view, otherwise from its passphrase */
static int ctl_open_create_common(ctl_priv_t *priv, char *argv[], int create) {
	unsigned char key[KEY_LENGTH];
	ks_entry_t *e;
	char *pw;
	size_t len;
	int ret, ambiguous;

	if (do_server_command(priv->s, 1, "check-available %s", argv[0]))
		return -1;
	if (result.status == -1) return 0;

	assert(!strcmp(DEFAULT_KDF_FUNCTION, "PBKDF2"));

	e = ks_lookup(argv[0], &ambiguous);
	if (ambiguous) return 0;
	if (e) {
		VERBOSE("the key of \"%s\" is in the key store", argv[0]);
		return send_key(priv, argv[0], e->key, create);
	}

	if (!(pw = ask_passphrase("Enter passphrase: ", create, &len))) {
		exit_status = EXIT_FAILURE;
		return 0;
	}
	if (!len) WARNING("empty passphrase used, "
			"this is not very secure");

	derive_key(pw, len, "", key);
	free_passphrase(pw, len);

	ret = send_key(priv, argv[0], key, create);

	// WARNING: sizeof(key) must be cast to int... see below
	wipememory(key, (int)sizeof(key));

	return ret;
}

static int ctl_open(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 0);
}

static int ctl_help(ctl_priv_t *priv, char *argv[]) {
	printf("Internal commands:\n\n");
	printf("create NAME\n");
	printf("open NAME\n");
	printf("close NAME\n");
	printf("resize NAME MACROBLOCKS\n");
	printf("luksformat NAME\n");
	printf("luksopen NAME\n");
	printf("luksstatus [NAME] (LUKS header, key store, loop device, /dev/mapper, mount)\n");
	printf("mke2fs NAME\n");
	printf("mount NAME MOUNTPOINT\n");
	printf("umount NAME|MOUNTPOINT\n");
	printf("chown OWNER.GROUP NAME|MOUNTPOINT\n");
	printf("paranoia LEVEL [KIBPS]\n");
	printf("\nKey store (%s, read only: keystorectl changes it), one view at a time:\n\n", ks.path);
	printf("unlock            (a passphrase; the view that is unlocked is locked)\n");
	printf("unlock-until NAME (passphrases until a view has NAME, for a boot: the\n");
	printf("                   partitions of the other views are opened)\n");
	printf("lock\n");
	printf("keys              (the partitions of the unlocked view)\n");
	printf("open-all          (the partitions of the unlocked view)\n");
	printf("luksopen-all      (their LUKS volumes)\n");
	return 0;
}

static int ctl_create(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 1);
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
			exit_status = EXIT_FAILURE;
			return -1;
		}

		*(is++) = '\0';

		for (i = 0; i < no; i++) {
			if (!strcmp(result.argv[j], names[i])) {
				if (done[i]) {
					printf("double response "
							"from the server");
					exit_status = EXIT_FAILURE;
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
#define DEFAULT_RESERVED_MACROBLOCKS \
	(DEFAULT_KEEP_REVISIONS + DEFAULT_INCREMENT)

static int ctl_resize(ctl_priv_t *priv, char *argv[]) {
	int no_macroblocks, new, reserved_macroblocks;

	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		return 0;
	}

	if (parse_info(priv, 1, "no_macroblocks", &no_macroblocks)) return 0;

	if (parse_int(&new, argv[1])) return 0;

	if (new > 0) reserved_macroblocks = (new - 1)/4 + 1; 
	else reserved_macroblocks = 0;

	if (new == no_macroblocks) {
		printf("size already is %d\n", new);
		return 0;
	}

	if (new < 0) {
		printf("you cannot resize below 0\n");
		return 0;
	}

	if (new > priv->no_macroblocks) {
		printf("can't resize to %d macroblocks because there are only "
				"%d macroblocks in total\n",
				new, priv->no_macroblocks);
		return 0;
	}

	/* only show this warning if quiet is not active or assume_yes
	 * is not active */
	if (!quiet || assume_yes < 3) {
		if (new > no_macroblocks) {
			char *prompt = "\
only safe if ALL your scubed3 partitions are open, continue? [No] ";
			warning();
			printf("allocating %d blocks for %s from the unclaimed pool, "
					"this is\n", new - no_macroblocks, argv[0]);

			if (!yesno(prompt)) return 0;
		} else {
			assert(no_macroblocks > new);
			warning();
			if (!new) {
				printf("\
removing %s: all its data is lost.\n\
\n\
Removing a partition is a sensitive operation: its blocks are wiped, in\n\
paranoia level 2 and 3 in random order, that takes long (%s cannot be\n\
closed until it is done). To protect its data, and the existence of its\n\
passphrase: if scubed3 stops before \"p\" shows %s as [removed, wiped],\n\
OPEN %s FIRST every time you start scubed3, before you switch to level 2\n\
or 3 and before you write to the other partitions, until \"p\" shows\n\
[removed, wiped]. Do not use its passphrase again before that.\n\
\n", argv[0], argv[0], argv[0], argv[0]);
				if (!typed_yes("type yes if you understand this "
							"and want to remove it: ")) {
					printf("%s is not removed\n", argv[0]);
					return 0;
				}
				return do_server_command(priv->s, 1,
						"resize-internal %s 0 0", argv[0]);
			}
			printf("removing %d blocks from the end of %s, those blocks\n",
					no_macroblocks - new, argv[0]);
			printf("will be added to the unclaimed pool, if you have a "
					"filesystem on it\n");

			if (!yesno("you must resize it before typing Yes, "
						"continue? [No] ")) return 0;
		}
	}

	return do_server_command(priv->s, 1, "resize-internal %s %d %d",
			argv[0], new, reserved_macroblocks);
}

/* writing to a partition (mount, mke2fs, luksformat) below the recommended
 * paranoia level shows which blocks belong to which partition (see
 * Paranoia levels in the README), the user must type yes; returns 1 if it
 * may go on, 0 if not, -1 if the connection failed */
static int check_paranoia(ctl_priv_t *priv, const char *what) {
	int i, current = -1;
	char prompt[128];

	if (do_server_command(priv->s, 0, "p")) return -1;
	for (i = 0; i < result.argc; i++)
		sscanf(result.argv[i], "paranoia level %d", &current);
	if (current >= recommended_paranoia) return 1;

	warning();
	printf("the paranoia level is %d, the recommended level is %d (see "
			"the configuration\nfile): what is written to a "
			"partition now shows which blocks belong to\nwhich "
			"partition. Open ALL your partitions first, then "
			"switch with\n\"paranoia %d\", then %s.\n\n", current,
			recommended_paranoia, recommended_paranoia, what);
	snprintf(prompt, sizeof(prompt), "type yes to %s anyway: ", what);
	if (typed_yes(prompt)) return 1;
	printf("cancelled\n");

	return 0;
}

/* the partition must exist and must not be mounted, returns 1 if so (a
 * message is printed if not), -1 if the connection failed */
static int check_unmounted(ctl_priv_t *priv, const char *name) {
	if (do_server_command(priv->s, 0, "info %s", name)) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		return 0;
	}
	if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					name)) return -1;
	if (!result.status) {
		printf("partition \"%s\" is mounted on %s\n",
				name, result.argv[0]);
		return 0;
	}

	return 1;
}

/* the key of the LUKS volume on partition name in the key store, NULL if
 * it is not there (cryptsetup asks the passphrase then); *ambiguous: see
 * ks_lookup */
static ks_entry_t *luks_key(const char *name, int *ambiguous) {
	ks_entry_t *e = ks_lookup(name, ambiguous);

	return (e && (e->flags&KS_HAS_LUKS))?e:NULL;
}

/* open the LUKS volume on partition name as /dev/mapper/PREFIXNAME */
static int luks_open(ctl_priv_t *priv, const char *name) {
	int ambiguous;
	ks_entry_t *e = luks_key(name, &ambiguous);

	if (ambiguous) return 1;
	/* with the volume key no KDF runs (faster, also at boot) */
	if (e && (e->flags&KS_LUKS_VK)) return cryptsetup_with_key(e,
			"cryptsetup open --volume-key-file /dev/stdin %s/%s "
			LUKS_NAME, priv->mountpoint, name, luks_prefix, name);
	if (e) return cryptsetup_with_key(e, "cryptsetup open --key-file=- "
			"%s/%s " LUKS_NAME, priv->mountpoint, name,
			luks_prefix, name);

	return var_system("cryptsetup open %s/%s " LUKS_NAME,
			priv->mountpoint, name, luks_prefix, name);
}

/* the block device whose major:minor is in /sys/block/NAME/dev (or the
 * sysfs file at path), 0 if there is none */
static dev_t sysfs_dev(const char *path) {
	unsigned int ma, mi;
	FILE *f;
	int ok;

	if (!(f = fopen(path, "r"))) return 0;
	ok = fscanf(f, "%u:%u", &ma, &mi) == 2;
	fclose(f);

	return ok?makedev(ma, mi):0;
}

/* where the block device dev is mounted (the first mount point in mnt),
 * 1 if it is */
static int mounted_at(dev_t dev, char *mnt, size_t size) {
	char src[PATH_MAX], dir[PATH_MAX];
	struct stat st;
	FILE *f;
	int ret = 0;

	if (!(f = fopen("/proc/self/mounts", "r"))) return 0;
	while (fscanf(f, "%4095s %4095s %*[^\n]", src, dir) == 2) {
		if (src[0] != '/' || stat(src, &st) || !S_ISBLK(st.st_mode) ||
				st.st_rdev != dev) continue;
		snprintf(mnt, size, "%s", dir);
		ret = 1;
		break;
	}
	fclose(f);

	return ret;
}

/* the device mapper device on loop device dev (a device in /dev/mapper with
 * it as slave; or a node of the loop device itself, as fake-cryptsetup of
 * the tests makes it), its name in name, its device in *mdev */
static int mapper_on(const char *loop, dev_t dev, char *name, size_t size,
		dev_t *mdev) {
	char path[PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int ret = 0;

	if (!(d = opendir("/dev/mapper"))) return 0;
	while (!ret && (de = readdir(d))) {
		if (de->d_name[0] == '.' || !strcmp(de->d_name, "control"))
			continue;
		snprintf(path, sizeof(path), "/dev/mapper/%s", de->d_name);
		if (stat(path, &st) || !S_ISBLK(st.st_mode)) continue;
		snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/slaves/%s",
				major(st.st_rdev), minor(st.st_rdev), loop);
		if (st.st_rdev != dev && access(path, F_OK)) continue;
		snprintf(name, size, "%s", de->d_name);
		*mdev = st.st_rdev;
		ret = 1;
	}
	closedir(d);

	return ret;
}

/* the LUKS status of one partition: its LUKS header, the key store, the
 * loop devices on its file, the device mapper device on them (LUKS open),
 * where it is mounted */
static void luks_status(ctl_priv_t *priv, const char *name) {
	char path[PATH_MAX], loop[NAME_MAX + 1], mapper[NAME_MAX + 1],
	     mnt[PATH_MAX];
	unsigned char hdr[8];
	struct loop_info64 li;
	struct dirent *de;
	struct stat st;
	ks_entry_t *e;
	dev_t ldev, mdev;
	int fd, luks = 0, version = 0, v, loops = 0;
	DIR *d;

	snprintf(path, sizeof(path), "%s/%s", priv->mountpoint, name);
	if (stat(path, &st)) {
		printf("%s: not open\n", name);
		exit_status = EXIT_FAILURE;
		return;
	}
	if ((fd = open(path, O_RDONLY|O_CLOEXEC)) != -1) {
		if (pread(fd, hdr, sizeof(hdr), 0) == sizeof(hdr) &&
				!memcmp(hdr, "LUKS\272\276", 6)) {
			luks = 1;
			version = hdr[6]<<8|hdr[7];
		}
		close(fd);
	}
	printf("%s: ", name);
	if (luks && (version == 1 || version == 2))
		printf("LUKS%d header", version);
	else if (luks) printf("LUKS header (version %d)", version);
	else printf("no LUKS header");
	if (ks.data && (e = ks_find(&ks, -1, name, &v))) {
		if (e->flags&KS_LUKS_VK) printf(", its LUKS volume key is in "
				"view %s of the key store", vname(v));
		else if (e->flags&KS_HAS_LUKS) printf(", its LUKS key is in view "
				"%s of the key store", vname(v));
		else printf(", in view %s of the key store (no LUKS key)",
				vname(v));
	}

	/* the loop devices on the file of the partition */
	if ((d = opendir("/sys/block"))) {
		while ((de = readdir(d))) {
			if (strncmp(de->d_name, "loop", 4)) continue;
			snprintf(path, sizeof(path), "/dev/%s", de->d_name);
			if ((fd = open(path, O_RDONLY|O_CLOEXEC)) == -1) continue;
			if (ioctl(fd, LOOP_GET_STATUS64, &li) ||
					li.lo_device != st.st_dev ||
					li.lo_inode != st.st_ino) {
				close(fd);
				continue;
			}
			close(fd);
			snprintf(loop, sizeof(loop), "%s", de->d_name);
			snprintf(path, sizeof(path), "/sys/block/%s/dev", loop);
			ldev = sysfs_dev(path);
			loops++;
			printf(", /dev/%s", loop);
			if (mapper_on(loop, ldev, mapper, sizeof(mapper), &mdev)) {
				printf(" open as /dev/mapper/%s", mapper);
				if (mounted_at(mdev, mnt, sizeof(mnt)))
					printf(" (mounted on %s)", mnt);
			} else if (mounted_at(ldev, mnt, sizeof(mnt)))
				printf(" mounted on %s", mnt);
			else if (luks) printf(" (not opened)");
		}
		closedir(d);
	}
	if (!loops) printf(luks?", not open (no loop device)":
			", no loop device");
	printf("\n");
}

static int ctl_luksstatus(ctl_priv_t *priv, char *argv[]) {
	struct dirent *de;
	DIR *d;
	int n = 0;

	if (argv[0]) {
		luks_status(priv, argv[0]);
		return 0;
	}
	if (!(d = opendir(priv->mountpoint))) {
		printf("unable to read %s: %s\n", priv->mountpoint,
				strerror(errno));
		exit_status = EXIT_FAILURE;
		return 0;
	}
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		luks_status(priv, de->d_name);
		n++;
	}
	closedir(d);
	if (!n) printf("no partitions are open\n");

	return 0;
}

/* LUKS on a partition: with its key in the key store the volume key comes
 * from there (random, keystorectl made it; cryptsetup gets it in a memory
 * file, never in a file on disk), the volume has one keyslot: the passphrase
 * that is asked (any length), or a random key that is not kept (then only
 * the key store opens it); a key store from before the volume keys has the
 * key file of a keyslot; without the key store cryptsetup asks */
static int ctl_luksformat(ctl_priv_t *priv, char *argv[]) {
	int ret, ambiguous, fd = -1;
	ks_entry_t *e = luks_key(argv[0], &ambiguous);
	char prompt[128], *pw;
	size_t len;

	if (ambiguous) return 0;

	if ((ret = check_unmounted(priv, argv[0])) != 1) return ret;
	if ((ret = check_paranoia(priv, "format it")) != 1) return ret;
	if (is_luks(priv, argv[0])) printf("partition \"%s\" already has a "
			"LUKS header, it is formatted again\n", argv[0]);
	if (!e) {
		var_system("cryptsetup luksFormat --type luks2 %s/%s",
				priv->mountpoint, argv[0]);
		return 0;
	}

	snprintf(prompt, sizeof(prompt), "the data on \"%s\" is lost, type "
			"yes to format it (with its key from the key store): ",
			argv[0]);
	if (!typed_yes(prompt)) {
		printf("cancelled\n");
		return 0;
	}
	if (!(e->flags&KS_LUKS_VK)) {
		if (!cryptsetup_with_key(e, "cryptsetup -q luksFormat --type "
					"luks2 --key-file=- %s/%s",
					priv->mountpoint, argv[0]))
			printf("\"%s\" is formatted, its LUKS key is in the key "
					"store\n", argv[0]);
		return 0;
	}
	if (!(pw = ask_passphrase("Passphrase of the LUKS volume (empty: a "
					"random key, only the key store opens "
					"it): ", 1, &len))) return 0;
	if ((fd = memfd_create("scubed3ctl-vk", 0)) == -1 ||
			write(fd, e->luks, KS_LUKS) != KS_LUKS) {
		WARNING("unable to make a key file in memory: %s",
				strerror(errno));
		exit_status = EXIT_FAILURE;
	} else {
		if (!len) {
			free_passphrase(pw, len);
			pw = ecalloc(1, KS_LUKS);
			len = KS_LUKS;
			ks_random(pw, len);
		}
		if (!cryptsetup_with_data(pw, len, "cryptsetup -q luksFormat "
					"--type luks2 --key-size %d "
					"--volume-key-file /dev/fd/%d "
					"--key-file=- %s/%s", KS_LUKS*8, fd,
					priv->mountpoint, argv[0]))
			printf("\"%s\" is formatted, its volume key is in the "
					"key store\n", argv[0]);
	}
	if (fd != -1) {
		wipe_fd(fd, KS_LUKS);
		close(fd);
	}
	free_passphrase(pw, len);

	return 0;
}

/* open the LUKS volume of a partition (for a boot: no filesystem is
 * mounted, nothing is written) */
static int ctl_luksopen(ctl_priv_t *priv, char *argv[]) {
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		exit_status = EXIT_FAILURE;
		return 0;
	}
	if (!is_luks(priv, argv[0])) {
		printf("partition \"%s\" has no LUKS header\n", argv[0]);
		exit_status = EXIT_FAILURE;
		return 0;
	}
	luks_open(priv, argv[0]);

	return 0;
}

static int ctl_mke2fs(ctl_priv_t *priv, char *argv[]) {
	int ret;

	if ((ret = check_unmounted(priv, argv[0])) != 1) return ret;
	if ((ret = check_paranoia(priv, "make the filesystem")) != 1)
		return ret;
	if (is_luks(priv, argv[0])) {
		if (luks_open(priv, argv[0])) return 0;
		var_system("mke2fs -F /dev/mapper/" LUKS_NAME, luks_prefix,
				argv[0]);
		var_system("cryptsetup close " LUKS_NAME, luks_prefix, argv[0]);
	} else var_system("mke2fs -F %s/%s", priv->mountpoint, argv[0]);
	return 0;
}

static int ctl_mount(ctl_priv_t *priv, char *argv[]) {
	int ret;

	if (*argv[1] != '/') {
		printf("mountpoint must start with a slash\n");
		return 0;
	}
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		if ((ret = check_paranoia(priv, "mount it")) != 1) return ret;
		// we try to open the partition
		{
			char *oargv[] = { argv[0], NULL };
			ctl_open(priv, oargv);
		}

		if (result.status) return 0;
		if (do_server_command(priv->s, 0, "set-close-on-release %s 1",
				argv[0])) return -1;
		if (result.status) return 0;
	} else {
		/* the partition can be opened more than once, but it
		 * should not be mounted twice */
		if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
		if (!result.status) {
			printf("partition \"%s\" is already mounted on %s\n",
					argv[0], result.argv[0]);
			return 0;
		}
		if ((ret = check_paranoia(priv, "mount it")) != 1) return ret;
	}

	if (is_luks(priv, argv[0])) {
		if (luks_open(priv, argv[0])) return 0;
		if (var_system("mount /dev/mapper/" LUKS_NAME " %s",
					luks_prefix, argv[0], argv[1])) {
			var_system("cryptsetup close " LUKS_NAME, luks_prefix,
					argv[0]);
			return 0;
		}
	} else if (var_system("mount -o loop %s/%s %s", priv->mountpoint,
				argv[0], argv[1])) return 0;

	if (do_server_command(priv->s, 0, "set-aux %s mountpoint %s",
				argv[0], argv[1])) return -1;

	return 0;
}

static int ctl_chown(ctl_priv_t *priv, char *argv[]) {
	if (*argv[1] == '/') {
		printf("only partition name is allowed\n");
		return 0;
	}
	if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
				argv[1])) return -1;
	if (result.status) {
		printf("no mountpount known for \"%s\"\n", argv[1]);
		return 0;
	}
	var_system("chown %s %s", argv[0], result.argv[0]);

	return 0;
}

/* paranoia level 2 and 3 (see README) overwrite every block that does not
 * belong to an open partition, so all partitions must be open (we ask, but
 * not when switching between level 2 and 3: closing a partition switches
 * back to level 1, so they are still open); level 3 limits the speed of
 * every partition, it is shown */
static int ctl_paranoia(ctl_priv_t *priv, char *argv[]) {
	int i, level, current = -1;
	unsigned long total = 0, kibps = 0, blocks;
	char name[256];

	if (strcmp(argv[0], "0") && strcmp(argv[0], "1") &&
			strcmp(argv[0], "2") && strcmp(argv[0], "3")) {
		printf("paranoia level must be 0, 1, 2 or 3\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}
	level = atoi(argv[0]);
	if (argv[1] && (sscanf(argv[1], "%lu", &kibps) != 1 || !kibps)) {
		printf("the rate must be a positive number of KiB/s\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}

	if (level >= 2 && (!quiet || assume_yes < 3)) {
		if (do_server_command(priv->s, 0, "p")) return -1;
		for (i = 0; i < result.argc; i++)
			sscanf(result.argv[i], "paranoia level %d", &current);
		if (current < 2) {
			warning();
			printf("paranoia level %d writes random data to every "
					"block that does not belong\nto an open "
					"partition: partitions that are NOT OPEN NOW "
					"WILL BE DESTROYED\n(and closing a partition "
					"switches back to level 1), the open "
					"partitions are:\n", level);
			for (i = 0; i < result.argc; i++)
				if (strstr(result.argv[i], " blocks in "))
					printf("%s\n", result.argv[i]);
		}

		if (level == 3) {
			for (i = 0; i < result.argc; i++) {
				sscanf(result.argv[i], "%lu blocks total",
						&total);
				if (!argv[1]) sscanf(result.argv[i],
						"level 3 rate %lu KiB/s",
						&kibps);
			}
			printf("\nparanoia level 3 writes on average %lu KiB/s "
					"(%.1f GB per day), also if\nnothing "
					"is written; a partition gets a share of "
					"that proportional to its\nsize, so it "
					"can write at most (the garbage collector "
					"takes a part of it):\n", kibps,
					kibps*1024.*86400/1e9);
			for (i = 0; total && i < result.argc; i++)
				if (sscanf(result.argv[i], "%lu blocks in "
						"%255s", &blocks, name) == 2)
					printf("%s: %.1f KiB/s\n", name,
						(double)kibps*blocks/total);
		}
		if (current < 2 && !yesno("are ALL your scubed3 partitions "
					"open, continue? [No] ")) return 0;
	}

	if (argv[1]) return do_server_command(priv->s, 1, "paranoia %s %s",
			argv[0], argv[1]);
	return do_server_command(priv->s, 1, "paranoia %s", argv[0]);
}

/* unmount, and close the LUKS volume if it was mounted from one */
static void umount_close(const char *mountpoint) {
	var_system("src=$(findmnt -n -o SOURCE %s); umount %s && "
			"case \"$src\" in /dev/mapper/" LUKS_NAME ") "
			"cryptsetup close \"$src\";; esac", mountpoint,
			mountpoint, luks_prefix, "*");
}

static int ctl_umount(ctl_priv_t *priv, char *argv[]) {
	if (*argv[0] != '/') { // figure out mountpoint
		if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
		if (result.status) {
			printf("%s\n", result.argv[0]);
			return 0;
		}
		umount_close(result.argv[0]);
	} else umount_close(argv[0]);
	return 0;
}

/* one view at a time (no two partitions with the same name): unlock locks
 * the view that is unlocked */
static void lock_view(int quiet_lock) {
	if (!ks.no_views) return;
	if (!quiet_lock) printf("view %s is locked\n", vname(0));
	ks_remove_view(&ks, 0);
}

static int ctl_unlock(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int ret, view;

	if (!ks_open()) return 0;
	if ((ret = ks_ask_key("Key store passphrase: ", 0, key)) != 1) {
		if (!ret) printf("no passphrase\n");
		return 0;
	}
	lock_view(0);
	view = ks_unlock(&ks, key);
	wipememory(key, 32);
	if (view < 0) {
		printf("no view for this passphrase\n");
		exit_status = EXIT_FAILURE;
	} else printf("view %s: %d entries\n", vname(view), ks_count(view));

	return 0;
}

static int ctl_lock(ctl_priv_t *priv, char *argv[]) {
	if (!ks.no_views) printf("no view is unlocked\n");
	else lock_view(0);

	return 0;
}

static int ctl_keys(ctl_priv_t *priv, char *argv[]) {
	if (!ks.no_views) printf("no view is unlocked\n");
	else ks_list(0);

	return 0;
}

/* open every partition of the unlocked view that is not open */
static int open_view(ctl_priv_t *priv) {
	int i, n = 0;

	for (i = 0; ks.no_views && i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[0]->e[i];
		if (!(e->flags&KS_USED)) continue;
		if (do_server_command(priv->s, 0, "info %s", e->name))
			return -1;
		if (!result.status) continue; /* open already */
		printf("opening \"%s\"\n", e->name);
		if (send_key(priv, e->name, e->key, 0)) return -1;
		if (!result.status) n++;
	}
	printf("%d partitions are opened\n", n);

	return 0;
}

static int ctl_open_all(ctl_priv_t *priv, char *argv[]) {
	if (!ks.no_views) {
		printf("no view is unlocked\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}

	return open_view(priv);
}

/* for a boot (see contrib/initramfs-tools): passphrases are read until a
 * view with partition NAME is unlocked; the partitions of the views of the
 * other passphrases are opened (in paranoia level 2 and 3 all partitions
 * must be open), then the view is locked; after every passphrase one line,
 * "NAME" in quotes:
 *
 *	"NAME" is in view N		found, the next command is done
 *	view N: M entries, without "NAME"
 *	no view for this passphrase	(also an empty one)
 *
 * (opening the partitions of a view without NAME writes more lines after
 * its line); at the end of the input the commands after it are not done;
 * without a terminal no prompt is shown */
static int ctl_unlock_until(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int ret, view;
	const char *prompt = isatty(fileno(stdin))?"Key store passphrase: ":"";

	if (!ks_open()) return 1;
	lock_view(1);
	while (1) {
		if ((ret = ks_ask_key(prompt, 0, key)) == -1) {
			printf("no view with \"%s\" is unlocked\n", argv[0]);
			exit_status = EXIT_FAILURE;
			fflush(stdout);
			return 1;
		}
		view = -1;
		if (ret == 1) {
			view = ks_unlock(&ks, key);
			wipememory(key, 32);
		}
		if (view < 0) printf("no view for this passphrase\n");
		else if (!ks_find(&ks, view, argv[0], NULL)) {
			printf("view %s: %d entries, without \"%s\"\n",
					vname(view), ks_count(view), argv[0]);
			/* the answer must arrive now (the caller waits for
			 * it), the partitions are opened after it */
			fflush(stdout);
			if (open_view(priv)) return -1;
			lock_view(1);
		} else {
			printf("\"%s\" is in view %s\n", argv[0], vname(view));
			fflush(stdout);
			return 0;
		}
		fflush(stdout);
	}
}

/* open the LUKS volumes (with their keys from the key store) of the open
 * partitions of the unlocked view (NAME, if it is given, must be in it: at
 * boot the view of the root partition) */
static int ctl_luksopen_all(ctl_priv_t *priv, char *argv[]) {
	int i, n = 0;
	char dev[128];

	if (!ks.no_views || (argv[0] && !ks_find(&ks, 0, argv[0], NULL))) {
		printf(argv[0]?"\"%s\" is not in the unlocked view\n":
				"no view is unlocked\n", argv[0]);
		exit_status = EXIT_FAILURE;
		return 0;
	}
	for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[0]->e[i];
		if (!(e->flags&KS_USED) || !(e->flags&KS_HAS_LUKS)) continue;
		snprintf(dev, sizeof(dev), "/dev/mapper/" LUKS_NAME,
				luks_prefix, e->name);
		if (!access(dev, F_OK)) continue; /* open already */
		if (do_server_command(priv->s, 0, "info %s", e->name))
			return -1;
		if (result.status) continue; /* the partition is not open */
		if (!is_luks(priv, e->name)) continue; /* no LUKS (yet) */
		if (!luks_open(priv, e->name)) n++;
	}
	printf("%d LUKS volumes are opened\n", n);

	return 0;
}

static ctl_command_t ctl_commands[] = {
	{
		.head.key = "help",
		.command = ctl_help,
		.argc = 0,
		.usage = ""
	}, {
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
		.head.key = "luksformat",
		.command = ctl_luksformat,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "luksopen",
		.command = ctl_luksopen,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "luksstatus",
		.command = ctl_luksstatus,
		.argc = 0,
		.optargc = 1,
		.usage = " [NAME]"
	}, {
		.head.key = "unlock",
		.command = ctl_unlock,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "unlock-until",
		.command = ctl_unlock_until,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "lock",
		.command = ctl_lock,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "keys",
		.command = ctl_keys,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "open-all",
		.command = ctl_open_all,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "luksopen-all",
		.command = ctl_luksopen_all,
		.argc = 0,
		.optargc = 1,
		.usage = " [NAME]"
	}, {
		.head.key = "mke2fs",
		.command = ctl_mke2fs,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "mount",
		.command = ctl_mount,
		.argc = 2,
		.usage = " NAME MOUNTPOINT"
	}, {
		.head.key = "chown",
		.command = ctl_chown,
		.argc = 2,
		.usage = " OWNER.GROUP NAME"
	}, {
		.head.key = "paranoia",
		.command = ctl_paranoia,
		.argc = 1,
		.optargc = 1,
		.usage = " LEVEL [KIBPS]"
	}, {
		.head.key = "umount",
		.command = ctl_umount,
		.argc = 1,
		.usage = " NAME|MOUNTPOINT"
	}
};

static void show_help(void) {
	printf("%s is a program to connect with the scubed3 process to\n", exec_name);
	printf("manage scubed3 partitions (also known as hidden volumes);\n");
	printf("it only reads the key store, keystorectl changes it\n");
	printf("\n");
	printf("Usage:\n\n$ %s [-C CONFIG_FILE] [-s KDF_SALT] [-i KDF_ITERATIONS] \\\n", exec_name);
	printf("                [-a SOCKET_ADDRESS] [-K KEY_STORE] [-k] [-YYY] [-c COMMAND]... \\\n");
	printf("                [-v] [-q] [-d]\n");
	printf("\nOptions (defaults shown in parentheses):\n\n");
	printf("-C CONFIG_FILE    file with the KDF parameters (%s, if it\n", DEFAULT_CONFIG_FILE);
	printf("                  exists), lines kdf=%s-%s, salt=TEXT and\n", DEFAULT_KDF_FUNCTION, DEFAULT_KDF_HASH);
	printf("                  iterations=NUMBER, keep a backup of it! (and\n");
	printf("                  paranoia=LEVEL, the level that mount, mke2fs and\n");
	printf("                  luksformat require, default %d)\n", DEFAULT_PARANOIA);
	printf("-s KDF_SALT       salt used for KDF, overrides the config file (%s)\n", DEFAULT_KDF_SALT);
	printf("-i KDF_ITERATIONS iterations done by KDF, overrides the config file (%u)\n", DEFAULT_KDF_ITERATIONS);
	printf("-a SOCKET_ADDRESS address of scubed3 socket (%s)\n", control_socket);
	printf("-Y                assume Yes to questions, this option is DANGEROUS\n");
	printf("                  and must be specified 3 times to take effect\n");
	printf("-c COMMAND        non interactive mode, run COMMAND and exit,\n");
	printf("                  if the command has spaces, it must be quoted;\n");
	printf("                  more -c options: the commands one after the other\n");
	printf("-K KEY_STORE      the key store (%s)\n", DEFAULT_KEYSTORE_FILE);
	printf("-k                the passphrases of the key store are given as their\n");
	printf("                  keys (64 hexadecimal digits, from keystorectl derive),\n");
	printf("                  for scripts: the KDF is computed once\n");
	printf("-v                show verbose output\n");
	printf("-d                show debug output\n");
	printf("-q                do not show warnings, if \"assume Yes\" is active\n");
	printf("                  the warnings when shrinking/enlarging scubed3\n");
	printf("                  partitions is also not shown\n");
	printf("\nMore information:\n\nType \"help\" at the s3> prompt. You only get the prompt\nif the connection to the scubed3 process succeeds\n");
}

#define NO_COMMANDS (sizeof(ctl_commands)/sizeof(ctl_commands[0]))

int main(int argc, char **argv) {
	ctl_priv_t priv = { .s = -1, .forward = 1 };
	char *line = NULL;
	int ret, i = 0;

	ctl_init(argc, argv, show_help);
	ctl_add_commands(&priv, ctl_commands, NO_COMMANDS);
	ctl_connect(&priv, 1);
	VERBOSE("scubed3ctl-" VERSION);

	do {
		if (line) free(line);
		if (no_commands) {
			line = estrdup(command_options[i++]);
		} else line = ctl_readline("s3> ");

		// do not store history!
		//if (line && *line) add_history(line);

		/* exit is a special case */
		if (!line || !strcmp(line, "exit") || !strcmp(line, "quit") ||
				!strcmp(line, "q") || !strcmp(line, "x") ||
				!strcmp(line, "bye") ||
				!strcmp(line, "kthxbye") ||
				!strcmp(line, "thanks")) {
			if (!line) printf("^D\n");
			break;
		}

		ret = ctl_call(&priv, line);

		if (no_commands && (i == no_commands || ret)) break;
	} while (!ret);

	free(line);
	ks_free(&ks);
	ctl_disconnect(&priv);

	exit(no_commands?exit_status:0);
}
