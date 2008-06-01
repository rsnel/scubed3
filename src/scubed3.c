/* license GPLv3 or any later */
/* 'Log Structured Block Device' */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/user.h>
#undef NDEBUG /* we need sanity checking */
#include <assert.h>
#include <fuse/fuse_opt.h>

#include "config.h"
#include "scubed3.h"
#include "gcry.h"
#include "bit.h"
#include "blockio.h"
#include "verbose.h"
#include "dllist.h"
#include "util.h"
#include "cipher.h"
#include "hashtbl.h"
#include "fuse_io.h"

#define DM_SECTOR_LOG 9

#define ID	(index>>l->mesobits)
#define NO	(index&l->mesomask)

void obsolete_mesoblk(scubed3_t *l, blockio_info_t *bi, uint32_t no) {
	assert(bi);
	assert(bi->no_nonobsolete);
	bi->no_nonobsolete--;

	/* if the whole block is obsolete, remove it from the active list */
	if (!bi->no_nonobsolete) dllist_remove(&bi->elt);
}

void obsolete_mesoblk_byidx(scubed3_t *l, uint32_t index) {
	if (index != 0xFFFFFFFF)
		obsolete_mesoblk(l, &l->dev->b->blockio_infos[ID], NO);
}

static inline void update_block_indices(scubed3_t *l, uint32_t offset,
		uint32_t id, uint16_t  no) {
	l->block_indices[offset] = (id<<l->mesobits) + no;
}

struct hash_seqnos_s {
	gcry_md_hd_t hd;
	blockio_info_t *last;
};

char *hash_seqnos(scubed3_t *l, char *md5sum_res, blockio_info_t *last) {
	struct hash_seqnos_s priv = {
		.last = last
	};
	int add_seqno(blockio_info_t *bi, struct hash_seqnos_s *priv) {
		//printf(" %llu", bi->seqno);
		gcry_md_write(priv->hd, &bi->seqno, sizeof(uint64_t));
		return !(priv->last == bi);
	}

	//printf("sns:");
	gcry_call(md_open, &priv.hd, GCRY_MD_MD5, 0);
	dllist_iterate(&l->dev->used_blocks,
			(int (*)(dllist_elt_t*, void*))add_seqno, &priv);
	//printf("\n");
	memcpy(md5sum_res, gcry_md_read(priv.hd, 0), 16);
	gcry_md_close(priv.hd);

	return md5sum_res;
}

static void add_blockref(scubed3_t *l, uint32_t offset) {
	l->cur->indices[l->cur->no_indices] = offset;
	update_block_indices(l, offset, id(l->cur), l->cur->no_indices);
	l->cur->no_indices++;
}

static inline char *mesoblk(scubed3_t *l, uint16_t no) {
	assert(no < l->cur->max_indices);
	return l->data + (no<<l->dev->mesoblk_log);
}

void select_new_macroblock(scubed3_t *l) {
	blockio_info_t *head;
	uint32_t new, k, index;

	l->cur = blockio_dev_get_new_macroblock(l->dev);
	l->cur->seqno = ++l->next_seqno;
	l->cur->no_indices = 0;
	l->updated = 0;

	while (l->dev->next_free_macroblock == (new = id(l->dev->macroblocks[gcry_fastranduint32(l->dev->no_macroblocks)]))) {
		blockio_dev_write_macroblock(l->dev, l->data, l->cur);
		l->cur->seqno = l->next_seqno++;
	}

	l->dev->next_free_macroblock = new;

	head = blockio_dev_gc_which_macroblock(l->dev,
			l->dev->next_free_macroblock);

	/* copy contents of selected new empty block to RAM */
	for (k = 0; k < head->no_indices; k++) {
		index = l->block_indices[head->indices[k]];
		if (index != 0xFFFFFFFF &&
				&l->dev->b->blockio_infos[ID] == head) {
			blockio_dev_read_mesoblk(l->dev, mesoblk(l,
					l->cur->no_indices), id(head), k);

			add_blockref(l, head->indices[k]);

			obsolete_mesoblk(l, head, k);
		}
	}

	DEBUG("new block %u (seqno=%llu) has %u mesoblocks due to GC of "
			"block %u", id(l->cur), l->cur->seqno,
			l->cur->no_indices, id(head));
}

int replay(blockio_info_t *bi, scubed3_t *l) {
	uint32_t k, index;
	char md5_calc[16];

	for (k = 0; k < bi->no_indices; k++) {
		index = l->block_indices[bi->indices[k]];
		assert(id(bi) != ID);
		obsolete_mesoblk_byidx(l, index);
		update_block_indices(l, bi->indices[k], id(bi), k);
	}

	hash_seqnos(l, md5_calc, bi);
	if (!memcmp(bi->seqnos_hash, md5_calc, 16)) {
		VERBOSE("revision %llu OK!", bi->seqno) ;
		l->next_seqno = bi->seqno + 1;
	}
#if 0
	else {
		VERBOSE("revision %llu not OK", b->seqno);
	}
#endif

	l->cur = bi;
	return 1;
}

void debug_stuff(scubed3_t *l) {
	int que(blockio_info_t *b) {
		VERBOSE("block %u owned by \"%s\" (seqno %llu) has %u used "
				"mesoblocks", id(b), b->dev->name,
				b->seqno, b->no_nonobsolete);
		return 1;
	}
	dllist_iterate(&l->dev->used_blocks,
			(int (*)(dllist_elt_t*, void*))que, NULL);
}

void commit_current_macroblock(scubed3_t *l) {
	l->cur->no_nonobsolete = l->cur->no_indices;
	dllist_append(&l->dev->used_blocks, &l->cur->elt);

	hash_seqnos(l, l->cur->seqnos_hash, NULL);

	blockio_dev_write_macroblock(l->dev, l->data, l->cur);
}

void scubed3_free(scubed3_t *l) {
	if (l->updated) {
		DEBUG("committing current macroblock to disk and exit");
		commit_current_macroblock(l);
	} else {
		DEBUG("current macroblock doesn't contain new data, exiting");
	}

	free(l->block_indices);
	free(l->data);
}

void scubed3_init(scubed3_t *l, blockio_dev_t *dev) {
	int i;
	uint32_t no_block_indices;
	assert(l && dev);

	assert(dev->no_macroblocks); /* do not call this in this case */

	l->dev = dev;
	l->cur = NULL;

	l->mesobits = (dev->b->macroblock_log - dev->mesoblk_log);
	l->mesomask = 0xFFFFFFFF>>(32 - l->mesobits);
	DEBUG("mesobits=%u, mesomask=%08x", l->mesobits, l->mesomask);

	no_block_indices = (dev->no_macroblocks - dev->reserved)*dev->mmpm;
	l->block_indices = ecalloc(no_block_indices, sizeof(uint32_t));

	for (i = 0; i < no_block_indices; i++) l->block_indices[i] = 0xFFFFFFFF;

	l->data = ecalloc(dev->mmpm, dev->mesoblk_size);

	l->next_seqno = 0;
	assert(!l->cur); /* l->cur should still be NULL */
	dllist_iterate(&dev->used_blocks,
			(int (*)(dllist_elt_t*, void*))replay, l);

	if (l->cur) {
		if (l->next_seqno != l->cur->seqno + 1)
			FATAL("last revision (%llu) borked",
					l->cur->seqno);

		if(!blockio_check_data_hash(l->cur, l->data)) {
			FATAL("newest block has invalid data");
		} else DEBUG("data in newest block is OK");
	}

	debug_stuff(l);

	l->next_seqno = dev->highest_seqno_seen;

	if (!l->next_seqno) {
		dev->next_free_macroblock =
			id(dev->macroblocks[gcry_fastranduint32(
						dev->no_macroblocks)]);
	}

	DEBUG("next free macroblock is %d", dev->next_free_macroblock);
	select_new_macroblock(l);

	if (l->next_seqno == 1/*dllist_is_empty(&l->used_blocks)*/) {
		l->updated = 1;
	}
	//DEBUG("next block is %u (seqno=%llu)", id(l->cur), l->cur->seqno);
}

void blockio_dev_fake_mesoblk_part(blockio_dev_t *dev, void *addr,
		uint32_t id, uint32_t no, uint32_t offset, uint32_t size) {
	memset(addr, 0, size);
}

void do_cow(scubed3_t *l, uint32_t index, uint32_t muoff,
		uint32_t size, void *addr) {
	void (*readorfake)(blockio_dev_t*, void*, uint32_t, uint32_t,
			uint32_t, uint32_t) = blockio_dev_fake_mesoblk_part;

	if (index != 0xFFFFFFFF) readorfake = blockio_dev_read_mesoblk_part;

	/* read the parts of the  mesoblock we don't modify from disk
	 * otherwise, set it to zero */
	if (muoff) readorfake(l->dev, addr, ID, NO, 0, muoff);
	if (l->dev->mesoblk_size > size + muoff)
		readorfake(l->dev, addr + muoff + size, ID, NO, muoff + size,
				l->dev->mesoblk_size - size - muoff);
}

int do_write(scubed3_t *l, uint32_t mesoff, uint32_t muoff, uint32_t size,
		char *in) {
	uint32_t index = l->block_indices[mesoff];
	assert(muoff + size <= l->dev->mesoblk_size);
	void *addr;
	/* three possibilities:
	 * 1. the block is currently in RAM, we update it
	 * 2. the block was never written, we add it to RAM
	 * 3. the block is on disk, we obsolete it and add it to RAM */

	l->updated = 1;

	if (ID != id(l->cur)) {
		/* could be that the new block is full after
		 * garbage collecting (depends on the way the
		 * to-be-freed block is selected) */
		while (l->cur->no_indices == l->cur->max_indices) {
			commit_current_macroblock(l);
			select_new_macroblock(l);
		}
		index = l->block_indices[mesoff];
	}

	addr = mesoblk(l, (ID == id(l->cur))?NO:l->cur->no_indices);
	memcpy(addr + muoff, in, size);

	/* if not, the mesoblock is in RAM */
	if (ID != id(l->cur)) { /* we are on disk */
		/* if we do not write the complete mesoblock, read the
		 * other parts from disk (if possible) or zero them */
		do_cow(l, index, muoff, size, addr);

		/* mark old mesoblock obsolete, if there is one */
		obsolete_mesoblk_byidx(l, index);

		/* add new reference */
		add_blockref(l, mesoff);
	}

	return 0;
}

int do_read(scubed3_t *l, uint32_t mesoff, uint32_t muoff, uint32_t size,
		char *out) {
	uint32_t index = l->block_indices[mesoff];
	/* three possibilities:
	 * 1. the block is currently in RAM
	 * 2. the block was never written, we return zeroes
	 * 3. the block is on disk */

	if (ID == id(l->cur)) /* in RAM */
		memcpy(out, mesoblk(l, NO) + muoff, size);
	else if (index == 0xFFFFFFFF) /* never written */
		memset(out, 0, size);
	else /* we are on disk */
		blockio_dev_read_mesoblk_part(l->dev, out, ID, NO, muoff, size);
	return 0;
}

int do_req(scubed3_t *l, scubed3_io_t cmd, uint64_t r_offset, size_t size,
		char *buf) {
	assert(cmd == SCUBED3_READ || cmd == SCUBED3_WRITE);
	uint32_t meso = r_offset>>l->dev->mesoblk_log;
	uint32_t inmeso = r_offset%l->dev->mesoblk_size;
	uint32_t ooff = 0, reqsz;
	int (*action)(scubed3_t*, uint32_t, uint32_t, uint32_t, char*) =
		(cmd == SCUBED3_READ) ? do_read : do_write;

	if (inmeso) {
		if (inmeso + size <= l->dev->mesoblk_size) reqsz = size;
		else reqsz = l->dev->mesoblk_size - inmeso;

		if (action(l, meso, inmeso, reqsz, buf + ooff)) return 0;
		meso++;
		size -= reqsz;
		ooff += reqsz;
	}

	while (size >= l->dev->mesoblk_size) {
		if (action(l, meso, 0, l->dev->mesoblk_size, buf + ooff))
			return 0;
		meso++;
		size -= l->dev->mesoblk_size;
		ooff += l->dev->mesoblk_size;
	}

	if (size > 0 && action(l, meso, 0, size, buf + ooff)) return 0;

	return 1;
}

#define SCUBED3_OPT_KEY(a,b,c) { a, offsetof(struct options, b), c }

int main(int argc, char **argv) {
	struct options {
		char *base;
		uint8_t mesoblock_log;
		uint8_t macroblock_log;
		uint32_t reserved;
	} options = {
		.base = NULL,
		.mesoblock_log = 14,
		.reserved = 2,
		.macroblock_log = 22
	};
	struct fuse_opt scubed3_opts[] = {
		SCUBED3_OPT_KEY("-b %s", base, 0),
		SCUBED3_OPT_KEY("-r %d", reserved, 0),
		SCUBED3_OPT_KEY("-m %d", mesoblock_log, 0),
		SCUBED3_OPT_KEY("-M %d", macroblock_log, 0),
		FUSE_OPT_END
	};
	int ret, i, j = 0;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	blockio_t b;
	scubed3_t l;

	verbose_init(argv[0]);

	if (fuse_opt_parse(&args, &options, scubed3_opts, NULL) == -1)
		FATAL("error parsing options");

	VERBOSE("version %s Copyright (C) 2008, Rik Snel <rik@snel.it>",
			PACKAGE_VERSION);
	if (!options.base) FATAL("argument -b FILE is required");

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)<0)
		WARNING("failed locking process in RAM (not root?): %s",
				strerror(errno));

	gcry_global_init();

	blockio_init(&b, options.base, options.macroblock_log);

	uint8_t key[32] = { 0, };
	cipher_t c;
	blockio_dev_t dev;
	cipher_init(&c, "ABL4(SERPENT256)", 32, key, 32);
	blockio_dev_init(&dev, &b, &c, options.mesoblock_log, "test");
	if (dev.no_macroblocks == 0) {
		WARNING("device \"%s\" is empty, %u/%u, making it full", dev.name, dev.used.no_set, dev.used.no_bits);
		bitmap_setbits(&dev.used, dev.b->no_macroblocks);
		dev.reserved = options.reserved;
		for (i = 0; i < dev.b->no_macroblocks; i++) {
			dev.b->blockio_infos[i].dev = &dev;
			dev.b->blockio_infos[i].indices = ecalloc(dev.mmpm,
					sizeof(uint32_t));
		}
		dev.no_macroblocks = dev.b->no_macroblocks;
		dev.macroblocks = ecalloc(dev.no_macroblocks,
				sizeof(blockio_info_t*));
		for (i = 0; i < dev.b->no_macroblocks; i++) {
			if (dev.b->blockio_infos[i].dev == &dev) {
				assert(j < dev.no_macroblocks);
				dev.macroblocks[j++] = &dev.b->blockio_infos[i];
			}
		}
	}

	scubed3_init(&l, &dev);
	ret = fuse_io_start(args.argc, args.argv, &l);

	scubed3_free(&l);
	blockio_dev_free(&dev);
	cipher_free(&c);
	blockio_free(&b);

	free(options.base);
	fuse_opt_free_args(&args);

	exit(ret);
}

#if 0
	uint8_t key[32] = { 0, };
	uint8_t Z[16] = { 0, };
	uint8_t data[512] = { 0, };
	cipher_t w;
	Z[15] = 1;

	cipher_init(&w, "ABL4(SERPENT256)", 32, key, 32);
	cipher_enc(&w, data, data, Z);
	cipher_dec(&w, data, data, Z);
	cipher_free(&w);
	FATAL("stop");
#endif
