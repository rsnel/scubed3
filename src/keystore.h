/* keystore.h - a deniable store of the keys of partitions (for scubed3ctl)
 *
 * Copyright (C) 2026 Rik Snel <rik@snel.it>
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
#ifndef INCLUDE_SCUBED3_KEYSTORE_H
#define INCLUDE_SCUBED3_KEYSTORE_H 1

#include <stdint.h>

/* the key store is a file of KS_SLOTS slots; a slot is either random data
 * (unused) or a view: the entries of one passphrase, encrypted with
 * AES-256-GCM (the key is derived from the passphrase, see scubed3ctl.c),
 * with a random IV at the start and the tag at the end; a view has
 * KS_ENTRIES entries (unused entries are zero, the number of entries does
 * not show), an entry has the name of a partition, its key, and the key of
 * the LUKS volume on it; the whole file is written again for every change,
 * with new IVs and new random data, so it changes completely; nothing in
 * the file shows how many views there are */
#define KS_SLOTS	64
#define KS_ENTRIES	64
#define KS_NAME		31	/* with the terminating 0 */
#define KS_KEY		32	/* the key of the partition */
#define KS_LUKS		64	/* the key file of the LUKS volume */
#define KS_LUKS_PASS_MAX (KS_LUKS - 1)	/* a passphrase in luks: its length,
					 * then the passphrase */
#define KS_IV		12
#define KS_TAG		16

typedef struct ks_entry_s {
	uint8_t flags;
	char name[KS_NAME];
	unsigned char key[KS_KEY];
	unsigned char luks[KS_LUKS];
} ks_entry_t;

#define KS_USED		0x01	/* the entry is used */
#define KS_HAS_LUKS	0x02	/* luks is the key of the LUKS volume */
#define KS_LUKS_PASS	0x04	/* the key of the LUKS volume is a passphrase:
				 * luks[0] is its length, then the passphrase
				 * (the rest is zero), otherwise the key is all
				 * of luks (random) */

#define KS_PLAIN	(KS_ENTRIES*sizeof(ks_entry_t))
#define KS_SLOT		(KS_IV + KS_PLAIN + KS_TAG)
#define KS_SIZE		(KS_SLOTS*KS_SLOT)

typedef struct ks_view_s {
	int slot;
	unsigned char key[32]; /* AES-256-GCM, derived from the passphrase */
	ks_entry_t e[KS_ENTRIES];
} ks_view_t;

typedef struct keystore_s {
	const char *path;
	unsigned char *data; /* the file, NULL if it is not read yet */
	int no_views; /* the unlocked views, in the order of unlocking */
	ks_view_t *views[KS_SLOTS];
} keystore_t;

/* create a new key store (random data, no views), it must not exist */
int ks_create(const char *path);

/* read the key store, 0 or -1 (errno) */
int ks_read(keystore_t*);

/* the view of a key (derived from a passphrase): its number (0..) if it is
 * unlocked now or was unlocked before, -1 if no slot has a view of it */
int ks_unlock(keystore_t*, const unsigned char*);

/* add a new (empty) view with this key in a slot that is not used by an
 * unlocked view; -1 if there is none */
int ks_add_view(keystore_t*, const unsigned char*);

void ks_remove_view(keystore_t*, int);

/* the entry of a partition in a view, NULL if it is not there; with view
 * -1 the first unlocked view that has it (its number in *found) */
ks_entry_t *ks_find(keystore_t*, int view, const char*, int *found);

/* NAME is in view and in another unlocked view with other keys (two
 * partitions with the same name, for example a root partition you can
 * admit and a hidden one): the first such other view, -1 if there is none
 * (the same keys in more views are the same partition, no conflict) */
int ks_conflict(keystore_t*, int view, const char*);

/* a new entry in a view, NULL if the view is full */
ks_entry_t *ks_new_entry(keystore_t*, int view, const char*);

/* write the key store again: the unlocked views with new IVs, all other
 * slots new random data (the views that are not unlocked are lost); it is
 * locked (flock) while it is checked and written, -1 with errno ESTALE if
 * another process wrote it after it was read here (nothing is written: our
 * views do not have its change) */
int ks_write(keystore_t*);

void ks_random(void*, size_t);

/* for a signal handler: the temporary file of a write that is not done is
 * removed (the old key store stays) */
void ks_signal_cleanup(void);

/* wipe and free everything */
void ks_free(keystore_t*);

#endif /* INCLUDE_SCUBED3_KEYSTORE_H */
