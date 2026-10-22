/* keystorectl.c - the key store of scubed3 (see keystore.h)
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
/* the views of the key store are unlocked (more than one, each has a name:
 * iNN by default, NN its slot), changed in memory, and written by save
 * (the whole file again, the views that are not unlocked are lost);
 * scubed3ctl only reads the key store; the daemon is only used by import
 * (the volume key of an open LUKS volume) */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#undef NDEBUG /* we need sanity checking */
#include <sys/mman.h>
#include <assert.h>
#include "config.h"
#include "verbose.h"
#include "gcry.h"
#include "cipher.h"
#include "util.h"
#include "keystore.h"
#include "ctl_common.h"

/* the key store in memory differs from the file: save writes it */
static int changed = 0;

static void ks_changed(void) {
	if (!changed) printf("(the key store is changed in memory, save "
			"writes it)\n");
	changed = 1;
}

/* view VIEW: its name (i04, or the name it got at unlock), or its number
 * in the order of unlocking (1, 2, ..., for scripts), or the only unlocked
 * view if there is no argument; -1 (with a message) if there is no such
 * view */
static int ks_view_arg(const char *arg) {
	int view;

	if (!arg) {
		if (ks.no_views == 1) return 0;
		printf(ks.no_views?"%d views are unlocked, give the name of "
				"the view\n":"no view is unlocked (unlock)\n",
				ks.no_views);
		exit_status = EXIT_FAILURE;
		return -1;
	}
	for (view = 0; view < ks.no_views; view++)
		if (!strcmp(arg, ks.views[view]->name)) return view;
	if (strspn(arg, "0123456789") == strlen(arg) &&
			!parse_int(&view, arg) && view >= 1 &&
			view <= ks.no_views) return view - 1;
	printf("there is no view %s (keys shows the unlocked views)\n", arg);
	exit_status = EXIT_FAILURE;

	return -1;
}

/* a name for a view: as the name of a partition, not only digits (a number
 * is a view in the order of unlocking), not the name of another unlocked
 * view; 1 if it is right (a message if not) */
static int ks_valid_view_name(const char *name, int view) {
	int v;

	if (!ks_valid_name(name)) return 0;
	if (strspn(name, "0123456789") == strlen(name)) {
		printf("the name of a view is not only digits\n");
		return 0;
	}
	for (v = 0; v < ks.no_views; v++) if (v != view &&
			!strcmp(name, ks.views[v]->name)) {
		printf("view %s is unlocked already\n", name);
		return 0;
	}

	return 1;
}

/* at the keys> prompt (not with -c: scripts get the default names) a view
 * that is unlocked or added now gets a name (empty: the default, iNN) */
static void ks_name_view(int view) {
	char prompt[96], *name;

	if (!interactive) return;
	for (;;) {
		snprintf(prompt, sizeof(prompt), "name of this view (empty: "
				"%s): ", vname(view));
		if (!(name = ask_line(prompt)) || !*name) break;
		if (ks_valid_view_name(name, view)) {
			snprintf(ks.views[view]->name, KS_NAME, "%s", name);
			break;
		}
		free(name);
	}
	free(name);
}

/* save: the key store is written again completely, the views that are not
 * unlocked are lost: the user must type yes */
static int ks_commit(void) {
	int i;

	if (!quiet || assume_yes < 3) {
		warning();
		printf("the key store is written again, completely (new IVs, new "
				"random data):\nthe views of passphrases that "
				"are NOT UNLOCKED NOW ARE LOST, it\ndoes not "
				"know them; unlocked now:");
		if (!ks.no_views) printf(" no views");
		for (i = 0; i < ks.no_views; i++) printf("%s view %s (%d "
				"entries)", i?",":"", vname(i), ks_count(i));
		printf("\n\n");
	}
	if (!typed_yes("type yes if ALL passphrases of the key store are "
				"unlocked: ")) {
		printf("cancelled, the key store is not written (the "
				"changes are in memory, until you quit)\n");
		return 0;
	}
	if (ks_write(&ks)) {
		if (errno == ESTALE) printf("the key store %s is NOT "
				"written: another keystorectl changed it\nafter it "
				"was read here (this write would lose that "
				"change); quit,\nunlock again and redo the "
				"change\n", ks.path);
		else printf("unable to write the key store %s: %s\n",
				ks.path, strerror(errno));
		exit_status = EXIT_FAILURE;
		return 0;
	}
	printf("the key store is written\n");
	changed = 0;

	return 1;
}

static int ctl_save(ctl_priv_t *priv, char *argv[]) {
	if (!ks.data) {
		printf("the key store is not read (unlock, unlock-all)\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}
	if (!changed) printf("nothing is changed, it is written anyway\n");
	ks_commit();

	return 0;
}

static int ctl_create(ctl_priv_t *priv, char *argv[]) {
	if (ks_create(ks.path)) {
		printf("unable to create the key store %s: %s\n", ks.path,
				strerror(errno));
		exit_status = EXIT_FAILURE;
		return 0;
	}
	printf("the key store %s is created (%d slots, random data, no "
			"views): passphrase-add\nadds a passphrase (and save writes it); keep "
			"a backup of it (and of the configuration file)\n",
			ks.path, KS_SLOTS);

	return 0;
}

/* the partitions of view that are in another unlocked view with other
 * keys (the same name, another partition): they cannot be used while both
 * views are unlocked */
static void ks_warn_conflicts(int view) {
	int i, other;

	for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[view]->e[i];
		if (!(e->flags&KS_USED)) continue;
		if ((other = ks_conflict(&ks, view, e->name)) < 0) continue;
		printf("WARNING: \"%s\" is in view %s and in view %s with other "
				"keys (two partitions\nwith the same name), it "
				"cannot be used while both views are unlocked:\n"
				"lock one of them (lock VIEW)\n", e->name, vname(view),
				vname(other));
	}
}

static int ks_unlock_one(const char *prompt) {
	unsigned char key[32];
	int ret, view, n = ks.no_views;

	if ((ret = ks_ask_key(prompt, 0, key)) != 1) return ret;
	view = ks_unlock(&ks, key);
	wipememory(key, 32);
	if (view < 0) {
		printf("no view for this passphrase\n");
		exit_status = EXIT_FAILURE;
	} else {
		if (view >= n) ks_name_view(view);
		printf("view %s: %d entries\n", vname(view), ks_count(view));
		ks_warn_conflicts(view);
	}

	return 1;
}

static int ctl_unlock(ctl_priv_t *priv, char *argv[]) {
	if (!ks_open()) return 0;
	if (ks_unlock_one("Key store passphrase: ") == 0)
		printf("no passphrase\n");

	return 0;
}

/* passphrases are asked until an empty one */
static int ctl_unlock_all(ctl_priv_t *priv, char *argv[]) {
	if (!ks_open()) return 0;
	while (ks_unlock_one("Key store passphrase (empty when done): ") == 1);

	return 0;
}

/* forget an unlocked view (in memory, the key store is not changed); the
 * views after it get a lower number in the order of unlocking; save loses
 * it, as every view that is not unlocked */
static int ctl_lock(ctl_priv_t *priv, char *argv[]) {
	int view;

	char name[KS_NAME];

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	snprintf(name, sizeof(name), "%s", vname(view));
	ks_remove_view(&ks, view);
	printf("view %s is locked (save now loses it)\n", name);

	return 0;
}

/* name VIEW NAME: another name for an unlocked view (in memory, until it is
 * locked: the name is not in the key store) */
static int ctl_name(ctl_priv_t *priv, char *argv[]) {
	char old[KS_NAME];
	int view;

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	if (!ks_valid_view_name(argv[1], view)) {
		exit_status = EXIT_FAILURE;
		return 0;
	}
	snprintf(old, sizeof(old), "%s", vname(view));
	snprintf(ks.views[view]->name, KS_NAME, "%s", argv[1]);
	printf("view %s is now view %s (slot i%02d; the name is not in the key "
			"store)\n", old, vname(view), ks.views[view]->slot);

	return 0;
}

static int ctl_keys(ctl_priv_t *priv, char *argv[]) {
	int v;

	if (!ks.no_views) printf("no view is unlocked\n");
	for (v = 0; v < ks.no_views; v++) ks_list(v);

	return 0;
}

/* the key of a passphrase of the key store, for -k: the KDF is computed
 * once (it takes seconds), a script gives the key to the next runs of
 * keystorectl and scubed3ctl; the key unlocks the view as the passphrase
 * does, keep it as secret */
static int ctl_derive(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	char *pw;
	size_t len;
	int i;

	if (!(pw = ask_passphrase("Key store passphrase: ", 0, &len)))
		return 0;
	if (!len) {
		free_passphrase(pw, len);
		printf("no passphrase\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}
	derive_key(pw, len, "/keystore", key);
	free_passphrase(pw, len);
	printf("key: ");
	for (i = 0; i < 32; i++) printf("%02x", key[i]);
	printf("\n");
	fflush(stdout);
	wipememory(key, sizeof(key));

	return 0;
}

static int ctl_passphrase_add(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int view;

	if (!ks_open()) return 0;
	if (ks_ask_key("New key store passphrase: ", 1, key) != 1) {
		printf("no passphrase\n");
		return 0;
	}
	if (ks_unlock(&ks, key) >= 0) {
		printf("this passphrase has a view already\n");
	} else if ((view = ks_add_view(&ks, key)) < 0) {
		printf("all %d slots are used by unlocked views\n", KS_SLOTS);
	} else {
		ks_name_view(view);
		printf("view %s is added (empty, in slot i%02d of i00..i%02d, "
				"chosen at random)\n", vname(view),
				ks.views[view]->slot, KS_SLOTS - 1);
		ks_changed();
	}
	wipememory(key, 32);

	return 0;
}

static int ctl_passphrase_change(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int view, other;

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	if (ks_ask_key("New key store passphrase: ", 1, key) != 1) {
		printf("no passphrase\n");
		return 0;
	}
	if ((other = ks_unlock(&ks, key)) >= 0) {
		printf("this passphrase has a view already (view %s)\n",
				vname(other));
	} else {
		memcpy(ks.views[view]->key, key, 32);
		printf("view %s has the new passphrase\n", vname(view));
		ks_changed();
	}
	wipememory(key, 32);

	return 0;
}

static int ctl_passphrase_remove(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	printf("view %s (%d entries) is removed: the keys in it are lost, "
			"unless they are in\nanother view too\n", vname(view),
			ks_count(view));
	ks_remove_view(&ks, view);
	ks_changed();

	return 0;
}

/* a new entry for name in view: the keys of name in another unlocked view
 * (the same partition), otherwise new keys: key (the key of a passphrase of
 * the partition) or a random one if key is NULL, and a random volume key
 * for its LUKS volume (scubed3ctl luksformat uses it); 1 if the view is
 * changed (in memory) */
static int ks_add(int view, const char *name, const unsigned char *key) {
	ks_entry_t *e, *from;
	int other;

	if (!ks_valid_name(name)) return 0;
	if (ks_find(&ks, view, name, NULL)) {
		printf("\"%s\" is in view %s already\n", name, vname(view));
		return 0;
	}
	from = ks_find(&ks, -1, name, &other);
	if (from && ks_conflict(&ks, other, name) >= 0) {
		printf("\"%s\" has other keys in view %s and view %s: two "
				"partitions with the same\nname, it is not clear "
				"which one to copy; lock one of the views first "
				"(lock VIEW)\n", name, vname(other),
				vname(ks_conflict(&ks, other, name)));
		return 0;
	}
	if (!(e = ks_new_entry(&ks, view, name))) {
		printf("view %s is full (%d entries)\n", vname(view), KS_ENTRIES);
		return 0;
	}
	if (from && key) {
		printf("\"%s\" is in view %s: its keys are copied with add "
				"(a partition has\none key)\n", name, vname(other));
		memset(e, 0, sizeof(*e));
		return 0;
	}
	if (from) {
		memcpy(e, from, sizeof(*e));
		printf("the keys of \"%s\" are copied from view %s to view %s\n",
				name, vname(other), vname(view));
	} else if (key) {
		memcpy(e->key, key, KS_KEY);
		ks_random(e->luks, KS_LUKS);
		e->flags |= KS_HAS_LUKS|KS_LUKS_VK;
		printf("\"%s\" has the key of its passphrase and a new random "
				"volume key in view %s:\ncreate it, and "
				"luksformat it (scubed3ctl)\n", name, vname(view));
	} else {
		ks_random(e->key, KS_KEY);
		ks_random(e->luks, KS_LUKS);
		e->flags |= KS_HAS_LUKS|KS_LUKS_VK;
		printf("\"%s\" has new random keys in view %s: create it, and "
				"luksformat it (scubed3ctl)\n", name, vname(view));
	}

	return 1;
}

static int ctl_add(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (ks_add(view, argv[0], NULL)) ks_changed();

	return 0;
}

/* as add, but the partition gets the key of a passphrase (it is asked
 * twice; empty: a random key, as add): it can also be opened without
 * the key store, with the passphrase */
static int ctl_add_passphrase(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[KEY_LENGTH];
	char *pw;
	size_t len;
	int view, ret;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (!ks_valid_name(argv[0])) return 0;
	if (!(pw = ask_passphrase("Passphrase of the partition (empty: a "
					"random key): ", 1, &len))) return 0;
	if (!len) {
		free_passphrase(pw, len);
		ret = ks_add(view, argv[0], NULL);
	} else {
		derive_key(pw, len, "", key);
		free_passphrase(pw, len);
		ret = ks_add(view, argv[0], key);
		wipememory(key, sizeof(key));
	}
	if (ret) ks_changed();

	return 0;
}

/* NAME2 gets the keys of NAME1 (an unlocked view has it): the same
 * partition under another name, for example "root" for the initramfs; in
 * VIEW, or in the view of NAME1 */
static int ctl_copy(ctl_priv_t *priv, char *argv[]) {
	ks_entry_t *from, *e;
	int view, ambiguous, other;

	if (!ks_valid_name(argv[1])) return 0;
	if (!(from = ks_lookup(argv[0], &ambiguous))) {
		if (!ambiguous) printf("\"%s\" is not in an unlocked view\n",
				argv[0]);
		return 0;
	}
	if (argv[2]) {
		if ((view = ks_view_arg(argv[2])) < 0) return 0;
	} else ks_find(&ks, -1, argv[0], &view);
	if (ks_find(&ks, view, argv[1], NULL)) {
		printf("\"%s\" is in view %s already (remove it first)\n",
				argv[1], vname(view));
		return 0;
	}
	if (!(e = ks_new_entry(&ks, view, argv[1]))) {
		printf("view %s is full (%d entries)\n", vname(view), KS_ENTRIES);
		return 0;
	}
	e->flags = from->flags;
	memcpy(e->key, from->key, KS_KEY);
	memcpy(e->luks, from->luks, KS_LUKS);
	printf("\"%s\" has the keys of \"%s\" in view %s: the same partition "
			"(it is opened as\none of them at a time; remove "
			"%s removes the old name)\n", argv[1], argv[0],
			vname(view), argv[0]);
	if ((other = ks_conflict(&ks, view, argv[1])) >= 0)
		printf("NOTE: view %s has another partition \"%s\" (other "
				"keys); while both views\nare unlocked, \"%s\" "
				"cannot be used: lock one of them first (lock "
				"VIEW)\n", vname(other), argv[1], argv[1]);
	ks_changed();

	return 0;
}

/* the volume key of the LUKS volume of a partition, with a passphrase of it
 * (cryptsetup luksDump --dump-volume-key, the passphrase in a memory file,
 * the key comes back on a pipe), 0 if it is in vk (at most KS_LUKS bytes,
 * zero padded) */
static int luks_dump_vk(ctl_priv_t *priv, const char *name, const char *pw,
		size_t len, unsigned char *vk) {
	char *cmd, line[256], *p, *end;
	int fd, bits = 0, n = 0, dump = 0, ret = -1;
	unsigned long b;
	FILE *fp;

	memset(vk, 0, KS_LUKS);
	if ((fd = memfd_create("scubed3ctl-pass", 0)) == -1 ||
			write(fd, pw, len) != (ssize_t)len) {
		WARNING("unable to make a key file in memory: %s",
				strerror(errno));
		if (fd != -1) close(fd);
		return -1;
	}
	if (asprintf(&cmd, "LC_ALL=C cryptsetup luksDump -q --dump-volume-key "
				"--key-file /dev/fd/%d %s/%s", fd,
				priv->mountpoint, name) == -1)
		FATAL("out of memory");
	if (!(fp = popen(cmd, "r"))) FATAL("popen: %s", strerror(errno));
	free(cmd);
	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, "MK bits:", 8)) bits = atoi(line + 8);
		if (!strncmp(line, "MK dump:", 8)) {
			dump = 1;
			p = line + 8;
		} else p = line;
		if (!dump) continue;
		for (;;) {
			b = strtoul(p, &end, 16);
			if (end == p || b > 255) break;
			if (n < KS_LUKS) vk[n] = b;
			n++;
			p = end;
		}
	}
	wipememory(line, sizeof(line));
	if (!pclose(fp) && bits > 0 && bits%8 == 0 && n == bits/8 &&
			n <= KS_LUKS) ret = 0;
	else wipememory(vk, KS_LUKS);
	wipe_fd(fd, len);
	close(fd);

	return ret;
}

/* an existing partition: its key from its passphrase; if it is open (in
 * the daemon) and has LUKS on it, the passphrase of its LUKS volume is
 * asked, its volume key goes into the key store (the volume is not
 * changed); otherwise it gets a random volume key (for luksformat) */
static int ks_import(ctl_priv_t *priv, int view, const char *name) {
	ks_entry_t *e;
	char *pw;
	size_t len, old_len = 0;
	int other;
	char *argv[] = { (char*)name }, *old_pw = NULL;
	unsigned char vk[KS_LUKS];

	if (!ks_valid_name(argv[0])) return 0;
	if (ks_find(&ks, view, argv[0], NULL)) {
		printf("\"%s\" is in view %s already\n", argv[0], vname(view));
		return 0;
	}
	if (!(e = ks_new_entry(&ks, view, argv[0]))) {
		printf("view %s is full (%d entries)\n", vname(view), KS_ENTRIES);
		return 0;
	}
	if (!(pw = ask_passphrase("Passphrase of the partition: ", 0, &len))) {
		memset(e, 0, sizeof(*e));
		return 0;
	}
	derive_key(pw, len, "", e->key);
	free_passphrase(pw, len);

	/* the LUKS volume keeps its keyslots: with its passphrase (asked
	 * here, so that all input comes from the same place, also from a
	 * pipe) its volume key goes into the key store */
	ks_random(e->luks, KS_LUKS);
	e->flags |= KS_HAS_LUKS|KS_LUKS_VK;
	if (ctl_connect(priv, 0)) printf("the scubed3 daemon does not run: "
			"\"%s\" gets a random volume key\n(for luksformat; if "
			"it has a LUKS volume, open it and import it again)\n",
			argv[0]);
	else if (do_server_command(priv->s, 0, "info %s", argv[0]) ||
			result.status || !is_luks(priv, argv[0]))
		printf("\"%s\" is not open, or it has no LUKS volume: it gets "
				"a random volume key\n(for luksformat; if it has "
				"a LUKS volume, open it and import it again)\n",
				argv[0]);
	else {
		if (!(old_pw = ask_passphrase("Passphrase of its LUKS volume: ",
						0, &old_len)) || !old_len ||
				luks_dump_vk(priv, argv[0], old_pw, old_len,
					vk)) {
			printf("the volume key of the LUKS volume on \"%s\" is "
					"not in the key store (a wrong "
					"passphrase?)\n", argv[0]);
			e->flags &= ~(KS_HAS_LUKS|KS_LUKS_VK);
			memset(e->luks, 0, KS_LUKS);
		} else {
			memcpy(e->luks, vk, KS_LUKS);
			e->flags |= KS_HAS_LUKS|KS_LUKS_VK;
			printf("the volume key of the LUKS volume on \"%s\" is "
					"in the key store\n", argv[0]);
		}
		wipememory(vk, sizeof(vk));
		if (old_pw) free_passphrase(old_pw, old_len);
	}
	ctl_disconnect(priv);
	printf("the key of \"%s\" is in view %s (it is not checked: close and "
			"open it to see\nthat it is right)\n", argv[0], vname(view));
	if ((other = ks_conflict(&ks, view, argv[0])) >= 0)
		printf("NOTE: view %s has another partition \"%s\" (other "
				"keys); while both views\nare unlocked, \"%s\" "
				"cannot be used: lock one of them first (lock "
				"VIEW)\n", vname(other), argv[0], argv[0]);

	return 1;
}

static int ctl_import(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (ks_import(priv, view, argv[0])) ks_changed();

	return 0;
}

static int ctl_remove(ctl_priv_t *priv, char *argv[]) {
	ks_entry_t *e;
	int view;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (!(e = ks_find(&ks, view, argv[0], NULL))) {
		printf("\"%s\" is not in view %s\n", argv[0], vname(view));
		return 0;
	}
	wipememory(e, sizeof(*e));
	if (!ks_find(&ks, -1, argv[0], NULL)) printf("the keys of \"%s\" are "
			"not in another unlocked view: without them\nit cannot "
			"be opened (unless with its passphrase, if it has "
			"one)\n", argv[0]);
	ks_changed();

	return 0;
}

/* a guided setup of the key store: it is created if it does not exist, all
 * its passphrases are unlocked, new passphrases are added, the partitions
 * of every view are chosen; it is written once, at the end */
static int ctl_setup(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	char prompt[96], *name;
	int v, changed = 0, created = 0, step = 1;

	if (access(ks.path, F_OK)) {
		if (ks_create(ks.path)) {
			printf("unable to create the key store %s: %s\n",
					ks.path, strerror(errno));
			exit_status = EXIT_FAILURE;
			return 0;
		}
		printf("the key store %s is created (keep a backup of it, and "
				"of the configuration\nfile)\n", ks.path);
		created = 1;
	}
	if (!ks_open()) return 0;

	if (!created) {
		printf("\n%d. unlock ALL passphrases of the key store: the views "
				"of the passphrases\n   that are not unlocked are "
				"lost when it is written\n", step++);
		while (ks_unlock_one("Key store passphrase (empty when "
					"done): ") == 1);
	}

	printf("\n%d. new passphrases: each one gets its own view (for "
			"example one you can\n   reveal, and one for the hidden "
			"partitions)\n", step++);
	while (ask_yn("add a new passphrase? [y/N] ")) {
		if (ks_ask_key("New key store passphrase: ", 1, key) != 1)
			continue;
		if (ks_unlock(&ks, key) >= 0)
			printf("this passphrase has a view already\n");
		else if ((v = ks_add_view(&ks, key)) < 0)
			printf("all %d slots are used\n", KS_SLOTS);
		else {
			ks_name_view(v);
			printf("view %s is added (in slot i%02d of i00..i%02d, "
					"chosen at random)\n", vname(v),
					ks.views[v]->slot, KS_SLOTS - 1);
			changed = 1;
		}
		wipememory(key, 32);
	}

	printf("\n%d. the partitions of every view: a name that is in another "
			"view gets the same\n   keys, an existing partition is "
			"imported (with its passphrase), a new one\n   gets new "
			"keys (then: create, resize, luksformat and mke2fs "
			"it)\n", step++);
	for (v = 0; v < ks.no_views; v++) {
		printf("\n");
		ks_list(v);
		for (;;) {
			snprintf(prompt, sizeof(prompt), "partition to add to "
					"view %s (empty when done): ", vname(v));
			if (!(name = ask_line(prompt)) || !*name) {
				free(name);
				break;
			}
			snprintf(prompt, sizeof(prompt), "is \"%.30s\" an "
					"existing partition with its own "
					"passphrase? [y/N] ", name);
			if (!ks_find(&ks, -1, name, NULL) && ks_valid_name(name)
					&& ask_yn(prompt))
				changed |= ks_import(priv, v, name);
			else changed |= ks_add(v, name, NULL);
			free(name);
		}
	}

	if (!changed) {
		printf("\nnothing is changed\n");
		return 0;
	}
	printf("\n");
	if (ks_commit()) printf("\nwith a view of the key store unlocked "
			"(scubed3ctl: unlock), open, open-all,\ncreate, "
			"luksformat, mke2fs, mount and luksopen use its keys\n");

	return 0;
}

static int ctl_help(ctl_priv_t *priv, char *argv[]) {
	printf("Commands (the key store %s; the changes are in memory until save):\n\n", ks.path);
	printf("setup             (guided: creates, unlocks, adds passphrases and partitions,\n");
	printf("                   saves)\n");
	printf("create            (a new key store: random data, no views)\n");
	printf("unlock\n");
	printf("unlock-all        (passphrases until an empty one)\n");
	printf("name VIEW NAME    (another name for an unlocked view, default iNN: its slot;\n");
	printf("                   at the keys> prompt unlock asks a name)\n");
	printf("lock VIEW         (forget an unlocked view, for example when two views have\n");
	printf("                   a partition with the same name and other keys)\n");
	printf("keys              (the unlocked views and their partitions)\n");
	printf("derive            (the key of a passphrase, for -k)\n");
	printf("passphrase-add\n");
	printf("passphrase-change [VIEW]\n");
	printf("passphrase-remove [VIEW]\n");
	printf("add NAME [VIEW]   (new random keys, or the keys of NAME in another view)\n");
	printf("add-passphrase NAME [VIEW] (the key of a passphrase, not a random key)\n");
	printf("copy NAME1 NAME2 [VIEW] (NAME2 gets the keys of NAME1)\n");
	printf("import NAME [VIEW] (an existing partition, with its passphrase)\n");
	printf("remove NAME [VIEW]\n");
	printf("save              (writes the key store: ALL its views must be unlocked)\n");
	printf("quit\n");
	return 0;
}

static ctl_command_t ctl_commands[] = {
	{
		.head.key = "help",
		.command = ctl_help,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "setup",
		.command = ctl_setup,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "create",
		.command = ctl_create,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "unlock",
		.command = ctl_unlock,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "unlock-all",
		.command = ctl_unlock_all,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "name",
		.command = ctl_name,
		.argc = 2,
		.usage = " VIEW NAME"
	}, {
		.head.key = "lock",
		.command = ctl_lock,
		.argc = 1,
		.usage = " VIEW"
	}, {
		.head.key = "keys",
		.command = ctl_keys,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "derive",
		.command = ctl_derive,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "passphrase-add",
		.command = ctl_passphrase_add,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "passphrase-change",
		.command = ctl_passphrase_change,
		.argc = 0,
		.optargc = 1,
		.usage = " [VIEW]"
	}, {
		.head.key = "passphrase-remove",
		.command = ctl_passphrase_remove,
		.argc = 0,
		.optargc = 1,
		.usage = " [VIEW]"
	}, {
		.head.key = "add",
		.command = ctl_add,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "add-passphrase",
		.command = ctl_add_passphrase,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "copy",
		.command = ctl_copy,
		.argc = 2,
		.optargc = 1,
		.usage = " NAME1 NAME2 [VIEW]"
	}, {
		.head.key = "import",
		.command = ctl_import,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "remove",
		.command = ctl_remove,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "save",
		.command = ctl_save,
		.argc = 0,
		.usage = ""
	}
};

static void show_help(void) {
	printf("%s is a program to change the key store of scubed3 (the keys\n", exec_name);
	printf("of partitions in views, one view per passphrase): the views are\n");
	printf("unlocked and changed in memory, save writes the key store\n");
	printf("\n");
	printf("Usage:\n\n$ %s [-C CONFIG_FILE] [-s KDF_SALT] [-i KDF_ITERATIONS] \\\n", exec_name);
	printf("                [-a SOCKET_ADDRESS] [-K KEY_STORE] [-k] [-YYY] [-c COMMAND]... \\\n");
	printf("                [-v] [-q] [-d]\n");
	printf("\nOptions (defaults shown in parentheses):\n\n");
	printf("-C CONFIG_FILE    file with the KDF parameters (%s, if it\n", DEFAULT_CONFIG_FILE);
	printf("                  exists): salt=TEXT, iterations=NUMBER\n");
	printf("-s KDF_SALT       salt used for KDF, overrides the config file (%s)\n", DEFAULT_KDF_SALT);
	printf("-i KDF_ITERATIONS iterations done by KDF, overrides the config file (%u)\n", DEFAULT_KDF_ITERATIONS);
	printf("-a SOCKET_ADDRESS address of scubed3 socket (%s), for import\n", control_socket);
	printf("-Y                assume Yes to questions, this option is DANGEROUS\n");
	printf("                  and must be specified 3 times to take effect\n");
	printf("-c COMMAND        non interactive mode, run COMMAND and exit,\n");
	printf("                  if the command has spaces, it must be quoted;\n");
	printf("                  more -c options: the commands one after the other\n");
	printf("                  (changes that are not saved are lost: -c save)\n");
	printf("-K KEY_STORE      the key store (%s)\n", DEFAULT_KEYSTORE_FILE);
	printf("-k                the passphrases of the key store are given as their\n");
	printf("                  keys (64 hexadecimal digits, from derive), for\n");
	printf("                  scripts: the KDF is computed once\n");
	printf("-v                show verbose output\n");
	printf("-d                show debug output\n");
	printf("-q                do not show warnings (with -YYY: not the warning of\n");
	printf("                  save either)\n");
	printf("\nType \"help\" at the keys> prompt.\n");
}

#define NO_COMMANDS (sizeof(ctl_commands)/sizeof(ctl_commands[0]))

/* quit with changes that are not saved: at the prompt the user is asked,
 * with -c it is an error */
static int may_quit(void) {
	if (!changed) return 1;
	if (no_commands) {
		printf("the changes are NOT saved (-c save writes them)\n");
		exit_status = EXIT_FAILURE;
		return 1;
	}

	return ask_yn("the changes are NOT saved, quit anyway? [y/N] ");
}

int main(int argc, char **argv) {
	ctl_priv_t priv = { .s = -1, .forward = 0 };
	char *line = NULL;
	int ret = 0, i = 0;

	ctl_init(argc, argv, show_help);
	ctl_add_commands(&priv, ctl_commands, NO_COMMANDS);
	VERBOSE("keystorectl-" VERSION);

	for (;;) {
		if (line) free(line);
		if (no_commands) {
			if (i == no_commands || ret) {
				line = NULL;
				may_quit();
				break;
			}
			line = estrdup(command_options[i++]);
		} else if (!(line = ctl_readline("keys> "))) {
			printf("^D\n");
			if (changed) printf("the changes are NOT saved\n");
			break;
		}

		if (!strcmp(line, "exit") || !strcmp(line, "quit") ||
				!strcmp(line, "q") || !strcmp(line, "x")) {
			if (may_quit()) break;
			continue;
		}

		ret = ctl_call(&priv, line);
	}

	free(line);
	ks_free(&ks);

	exit(no_commands?exit_status:0);
}
