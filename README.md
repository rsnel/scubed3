# Scubed3

## Introduction

Scubed3 stands for Simple Steganographic Store 3. It is the successor of
[scubed](https://penta.snel.it/~rsnel/scubed/) and the humiliating failure that
is [scubed2](https://snel.it/svn/scubed2/trunk/).

The original scubed is vulnerable to surface anaylsis; carefully watching the
surface of a harddisk (or examining the internal log of an SSD), you can
determine which parts of the disk are used. When an adversary sees the disk is
used in a spot, you lose plausible deniability of the existence of a key to
decrypt that part of the disk. TrueCrypt/VeraCrypt hidden volumes are also
vulnerable to this attack.

Scubed3 aims to protect against surface analysis. To do this we divide the disk
in macroblocks with a size of, for example, `4MiB`. If the disk is in use, and
data must be written, randomly selected macroblocks will be written one by one
until all data is written. If a macroblock is written, all bits of that macroblock
change with probability 0.5.

VAPORWARE WARNING: completely random block selection is not implemented yet,
it will only work on a per-device basis.


## Tutorial

Create a partition, file or logical volume of, say, `4GiB`. You will also need
at least two mountpoints, one for `scubed3` (/mnt/scubed3) itself and any number for the
`scubed3` devices you create (/mnt/test).

Then run

    # scubed3 -f -b /dev/PARTITION /mnt/scubed3

The `-f` option keeps `scubed3` in the foreground, so you can see the verbose output. It responds with

    scubed3:version UNKNOWN Copyright (C) 2019, Rik Snel <rik@snel.it>
    scubed3:mesoblock size 16384 bytes, macroblock size 4194304 bytes
    scubed3:256 mesoblocks per macroblock, including index
    scubed3:required minimumsize of indexblock excluding macroblock index is 1280 bytes
    scubed3:maximum amount of macroblocks supported 120832
    scubed3:listening for connection with scubed3ctl on /tmp/scubed3

Internally `scubed3` uses `FUSE`, [Filesystem in
Userspace](https://github.com/libfuse/libfuse), to make hidden devices
available to the OS. It also uses a unix domain socket to communicate with the
outside world. The program `scubed3ctl` is a client to connect to a running
`scubed3` process. If you run it as follows

    # scubed3ctl -v -d

it outputs some information and gives you a `s3>` prompt

    scubed3ctl:debug:using version 1.8.4 of libgcrypt
    scubed3ctl:scubed3ctl-UNKNOWN, connected to scubed3-UNKNOWN
    scubed3ctl:cipher: CBC_ESSIV(AES256), KDF: PBKDF2(SHA256/16777216)
    s3>

The command `p` shows information about the disk

    s3> p
    0001024 blocks unclaimed
    0001024 blocks total
    s3>

Let's create a new device. The command to do that is `create` and it will ask
you for a passphrase associated to the new device. Since the device is new,
`scubed3` can't know if you typed the passphrase correctly, so you have to type
it twice

    s3> create foo
    Enter passphrase:
    Verify passphrase:
    scubed3ctl:computing 16777216 iterations of PBKDF2(SHA256), please wait...
    s3> p
    0000000 blocks in foo (0.0MiB)
    0001024 blocks unclaimed
    0001024 blocks total
    s3>

The device is created, but it has no blocks yet. Nothing has been written to
disk. To allocate some blocks to this device use the command `resize`, we also
create a filesystem on it, mount it at `/mnt/test`, give permissions to
`USERNAME` on the mounted filesystem and query the status of the disk with `p`.

    s3> resize foo 32
    ---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---
    allocating 32 blocks for foo from the unclaimed pool, this is
    only safe if ALL your scubed3 partitions are open, continue? [No] Yes
    s3> mke2fs foo
    mke2fs 1.44.5 (15-Dec-2018)
    Creating filesystem with 97920 1k blocks and 24480 inodes
    Filesystem UUID: 704b5a90-6897-452b-95d9-f0c0d585330b
    Superblock backups stored on blocks:
            8193, 24577, 40961, 57345, 73729

    Allocating group tables: done
    Writing inode tables: done
    Writing superblocks and filesystem accounting information: done


    s3> mount foo /mnt/test
    s3> chown USERNAME foo
    s3> p
    0000032 blocks in foo (95.6MiB) [U] [MNT /mnt/test]
    0000992 blocks unclaimed
    0001024 blocks total
    s3>

Now the command `p` shows that `foo` is in use `[U]` and mounted at `/mnt/test`.
You can put some files in `/mnt/test` and then `umount` and `close` it.

    s3> umount foo
    s3> close foo
    s3> p
    0001024 blocks unclaimed
    0001024 blocks total
    s3>

It is as if nothing happened. If you want access to `foo` again, you can use the `mount` command
directly.

    s3> mount foo /mnt/test
    Enter passphrase:
    scubed3ctl:computing 16777216 iterations of PBKDF2(SHA256), please wait...
    s3> p
    0000032 blocks in foo (95.6MiB) [U] [C] [MNT /mnt/test]
    0000992 blocks unclaimed
    0001024 blocks total
    s3>

In addition to `[U]` and `[MNT /mnt/test]` we also see the `[C]` flag. This means that
if the device is umounted, it will also be closed.

    s> umount foo
    s> p
    0001024 blocks unclaimed
    0001024 blocks total
    s3>

## License

GPL v3 or (at your option) any later version


## Terminoloy

base device: this file/device/database holds all macroblocks, our worst case
threat-model assumes an adversary can make snapshots of it at will, each block
has a number

scubed3 partition: a block device whose contents are managed by a scubed3
deamon, writing to a scubed3 partition causes changes to the macroblocks in
the base device

macroblocks: the smallest bit of information to someone who doesn't have the
cryptographic key corresponding to it, a macroblock will either change randomly
(each bit changes with a probability of .5) or not at all

mesoblock: in a macroblock a series of mesoblocks are stored, along with
information on where the mesoblock belongs on the scubed3 partition.

indexblock: the first mesoblock in each macroblock contains information
about the other mesoblocks in said macroblock


## Encryption

ESSIV is a function that turns an `uint64_t` and two `uint32_t`'s in a 128 bit
block for use as IV.

indexblocks are encrypted with `IV = ESSIV(0, macroblock_number, 0)`

mesoblocks are encrypted with `IV = ESSIV(seqno, macroblock_number, index)`

the indexblock IV will repeat when the same disk block is written, this is not a problem, because the first 32 bytes of an indexblock are equal to the hash of the rest of the indexblock and the indexblock contains a unique number (the sequence number)

the mesoblock IV will never repeat (as long as the seqno is not reset)

## Threat model

The adversary has full knowledge about the macroblocks (including history) and
may even have write access to them. This adversary may have keys to some
scubed3 partitions. The goal is to be able to plausibly deny the existance of
partitions to which the adversary has no keys.  Only paranoia level 3 protects
completely against this. We assume the adversary cannot detect read access.


## Paranoia levels

When a new macroblock needs to be selected:

Level 0: not paranoid (NOT IMPLEMENTED YET)

it is selected from all allocated blocks based on it's emptyness, the most
empty block gets selected.

Level 1: moderately paranoid

it is randomly selected from all allocated blocks. This hurts write
performance.

Level 2: just paranoid (NOT IMPLEMENTED YET)

a random block of the device is selected. If it happens to be allocated to the
current device, it is used. If it happens to be allocated to another device, it
is updated with a new seqno (therefore every bit will change with a probability
of .5). If the block is unallocated, it will be added to the device, another
block will be deleted. (this level of paranoia is required for flash media,
this kind of media keeps an internal record about the order in whichs blocks
are written to it) It is only safe if all scubed3 devices are known to and
(if active) managed by the scubed3 deamon managing the current device.

[can be implemented as: select device (weighted by size), and write a random block]

Level 3: extremely paranoid (NOT IMPLEMENTED YET)

while active, the daemon writes blocks to random locations at regular intervals
to hide any real activity, this severely hurts performance. Considerations
of level 2 apply. It will wear out your flash very efficiently.


## Control protocol

/MOUNTPOINT/.control is a file to which commands can be written, and
scubed3's response can be read. A command is a line of text terminated
with a linefeed '\n'.

A response starts with "OK\n", or "ERR\n". One or more lines follow.
The last line must be ".\n". This terminates the message.

The command scubed3ctl uses this interface to communicate with scubed3.

Commands:

- status

  shows the status

- open NAME MODE KEY

opens a scubed partition

  * NAME is the name, like root or swap, whatever, the name has no real meaning,
    you can open any partition under any name

  * MODE is the ciphermode, eg `CBC_LARGE(AES)`

   * KEY is the cipher key bas16 encoded (hex).

- resize NAME MACROBLOCKS

  resizes a scubed partition

  * NAME ....

  * MACROBLOCKS, the amount of macroblocks owned by the partition

## Example calculation of indexblock size

For this example we assume the following. Macroblock size 4MB (m). Mesoblock
size 16kB (s). So, there are 256 mesoblocks per macroblock. Mesoblock
0 is the indexblock, the other ones are data blocks.

Whatever values are used, the indexblock MUST fit in one mesoblock! Required
size of the indexblock depends on the maximum amount of macroblocks scubed3 can
support (the size of a backing device can change, so scubed3 should be able to
deal with it, up to a maximum size).

The useful size of the device is

    (1<<(macroblock_log - mesoblock_log) - 1)(no_macroblocks - reserved_blocks)

Note that the usable size is limited by `2^32*(1<<mesoblock_log) - 1`, because the
device is partitioned in mesoblocks, and we have 32 bits available to index
the mesoblocks.

Layout of macroblock:

    0x000000 SHA256_HASH_INDEXBLOCK hash of 0x000020 - 0x003FFF
    0x000020 SHA256_HASH_DATA (hash of ciphertext)
    0x000040 SHA256_HASH_SEQNOS  /* seqnos of used macroblocks */
    0x000060 uint64_t seqno
    0x000068 uint64_t next_seqno
    0x000070 8byte literal "SSS3v0.1"
    0x000078 uint32_t no_macroblocks
    0x00007C uint32_t reserved_blocks
    0x000080 uint8_t[128] reserved space, MUST be 0
    0x000100 uint32_t no_indices
    0x000104 uint32_t idx0x01
    0x000108 uint32_t idx0x02
    ......
    0x0004FC uint32_t idx0xff
    0x000500 bitmap: each bit represents a macroblock of the base device
		 0 FREE
		   this block may be ours: if so, it has our signature
		   on disk (otherwise we wouldn't be able to know that
		   it is ours) and we don't care about the contents of
		   this block
		 1 USED
		   this block is ours: it may not have our signature yet
		   because it has never been written and we care about
		   the data
		(notice that it only makes sense not to care about the contents
 		of a block if there are no older revisions of that data available
		that we DO care about...)
    0x003FFC last 32 bits of bitmap (the bitmap is stored in units of 32 bits)
    0x004000 mesoblock1
    0x008000 mesoblock2
    0x00C000 mesoblock3
    ........
    0x3FC000 mesoblock255
    0x400000 end

There is space to index `0x4000 - 0x0500 = 0x3B00 = 15104` bytes,
which corresponds to a base device size of `15104*8 = 120832` macroblocks,
which correspond to `4MiB * 120832 = 472GiB`

Maximum usable size of the device is `2^32*(1<<mesoblock_log) = 64TiB`, which is
sufficient in the light of the maximum size of the backing device.

In general the indexblock requires:

- fixed size: 260 bytes (including 128 bytes of reserved space)

- 4 bytes for every mesoblock in a macroblock

- 1 bit for every macroblock

## Random block selection

How to randomly select blocks and assign roles to them?

Let's assume we have a device that consists of six macroblocks; 0 to 5
(inclusive). The block that is selected first has `sequence_number = 1`, then
second `sequence_number = 2` and so forth. We select blocks randomly by using
random number generator. In this example, the random number generator generated:

    341435513205214342043533

Since we generate a bunch of random numbers at once; we know when a block will
be reselected in the future. This knowledge is encoded in `nseq` (`next_sequence_number`).

The algorithm is as follows, in the nth step a block is selected:

- if the block will be immediately reselected (`nseq = seq + 1`), do a dummy write `!`

- if the block that will be selected next has data, copy that data to this block (mark old block with `^` and copy letter to this block)

- if the block that will be selected next has no data, give this block a new letter (`a`, `b`, `c` etc)

Here is an example. The square brackets denote the number that is chosen by the random number generator.

    seq   nseq   0  1  2  3  4  5
      1->    5:  -  -  - [a] -  - 
      2->    4:  -  -  -  a [b] - 
      3->    8:  - [b] -  a  ^  - 
      4->   15:  -  b  -  ^ [a] - 
      5->    9:  -  b  - [c] a  - 
      6->    7:  -  b  -  c  a [!]
      7->   12:  -  ^  -  c  a [b]
      8->   13:  - [c] -  ^  a  b 
      9->   16:  -  c  - [d] a  b 
     10->   18:  -  c [e] d  a  b 
     11->   14: [b] c  e  d  a  ^ 
     12->   22:  b  ^  e  d  a [c]
     13->   38:  ^ [b] e  d  a  c 
     14->   19: [a] b  e  d  ^  c 
     15->   17:  a  b  e  ^ [d] c 
     16->   21:  a  b  e [d] ^  c 
     17->   20:  a  b  ^  d [e] c 
     18->   27:  ^  b [a] d  e  c 
     19->   30: [e] b  a  d  ^  c 
     20->   25:  e  b  a  ^ [d] c 
     21->   23:  e  b  a [c] d  ^ 
     22->   48:  e  b  a  ^  d [c]
     23->   24:  e  b  a [!] d  c 
     24->   26:  e  b  a [d] ^  c 

This doesn't look very practical, but because we use a log structured block
device on top of it, with 25% reserved space, this approach is very managable.

## Log structured block device

In our example, we have macroblocks of `4MiB` and mesoblocks of `16kiB`. Each macroblock has
one indexblock and `255` mesoblocks. Suppose a scubed3 partition has 80 macroblocks allocated
to it. The usable size of this partition will be `0.75*80*255*16 kiB = 244800 kiB` or `0.75*80*255 = 15300` mesoblocks.

A mesoblock can have one of three states:

- never written; the mesoblock does not exist on disk or in the cache, if you
  read from it you get zeroes, if you write to it, it gets moved to the cache
and written there

- in the cache; the mesoblock exists in the cache (and maybe on disk),
  mesoblocks in the cache can easily be modified

- on disk; the mesoblock is encrypted on disk, upon read it is decrypted and
  upon write it is moved to the cache and written there

Scubed3 keeps track of the location of all mesoblocks using an `uint32_t` array
called `block_indices`. In the case of our example, this array has a size of
`15300`. Information about mesoblock `17` (the eighteent mesoblock, since we
count from zero) is stored in `block_indices[17]`.

- if the value is `0xffffffff`, then the mesoblock is never written

- otherwise the high bits are equal to the macroblock number and the low bits are the index
  of the mesoblock in the macroblock
  
  * if the macroblock pointed to is the current macroblock, the mesoblock is in the cache

  * otherwise, it is on disk

Relevant information is grabbed from `block_indices` by shifting and masking. The shift,
which represents the amount of mesoblocks in a macroblock (including the indexblock) is 
calculated as follows.

    mesobits = macroblock_log - mesoblk_log

In our example (`macrobock_log = lg(4194304) = 22` and `mesoblk_log = lg(16384) = 14`), `mesobits = 22 - 14 = 8`, which corresponds to `2^8 = 256` mesoblocks per macroblock (including the indexblock).

The macroblock number can be computed from `block_indices[17]` by shifting it `mesobits` to the right

    macroblock_number =  block_indices[17]>>mesobits

The number of the mesoblock in the macroblock, can be found by masking with `mesomask`

    mesomask = 0xffffffff>>(32 - mesobits)

In our example `mesomask = 0x000000ff`.
