// SPDX-License-Identifier: MIT
/* Ext2 implementation for the WebDAV filing backend. */

#include "ext2_fs.h"
#include "ext2_file_mode.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ext2_ext_ram.h"

enum {
    EXT2_MAGIC = 0xEF53,
    EXT2_VERSION = 1,
    EXT2_FIRST_INO = 11,
    EXT2_VALID_FS = 1,
    EXT2_ERROR_FS = 2,
    EXT2_FEATURE_INCOMPAT_FILETYPE = 0x0002,
    EXT2_SUPER_OFFSET = 1024,
    EXT2_GDT_BLOCK = 1,
    EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER = 0x0001,
    EXT2_FEATURE_RO_COMPAT_LARGE_FILE = 0x0002,
    EXT2_FEATURE_COMPAT_HAS_JOURNAL = 0x0004,
    EXT2_FEATURE_COMPAT_EXT_ATTR = 0x0008,
    EXT2_FEATURE_COMPAT_RESIZE_INODE = 0x0010,
    EXT2_FEATURE_COMPAT_DIR_INDEX = 0x0020,
    EXT2_FEATURE_INCOMPAT_RECOVER = 0x0004,
    EXT2_INDEX_FL = 0x00001000,
    EXT2_JOURNAL_INO = 8,
    EXT2_JOURNAL_MIN_VOLUME_BLOCKS = 16384, /* a volume smaller than 64 MiB has none */
    EXT2_INODES_PER_BLOCK = EXT2_BLOCK_BYTES / EXT2_INODE_BYTES,
    EXT2_DIRECT_BLOCKS = 12,
    EXT2_MAX_DIR_BLOCKS = 12 + EXT2_INDIRECT_ENTRIES +
                          EXT2_INDIRECT_ENTRIES * EXT2_INDIRECT_ENTRIES,
    EXT2_MODE_DIR = 0x41ED,  /* 040755 */
    EXT2_MODE_FILE = 0x81A4, /* 0100644 */
    EXT2_MODE_SYMLINK = 0xA1FF, /* 0120777 */
    EXT2_EPOCH = 1767225600, /* 2026-01-01 */
};

static_assert(sizeof(ext2_super_t) == 1024, "ext2 superblock layout");
static_assert(sizeof(ext2_inode_t) == EXT2_INODE_BYTES, "ext2 inode layout");
static_assert(sizeof(ext2_group_desc_t) == 32, "ext2 group descriptor layout");

static uint8_t io_block[EXT2_BLOCK_BYTES] EXT2_EXT_RAM_BSS;
static uint8_t indirect_block[EXT2_BLOCK_BYTES] EXT2_EXT_RAM_BSS;
/* Path lookup is serialized by the VFS filesystem lock.  Keep its scratch
 * paths out of task stacks so symlink resolution does not consume most of an
 * 8 KiB task stack. */
static char lookup_current_path[EXT2_PATH_BYTES];
static char lookup_resolved[EXT2_PATH_BYTES];
static char lookup_name[EXT2_NAME_BYTES];
static char lookup_target[EXT2_PATH_BYTES];
static char lookup_next_path[EXT2_PATH_BYTES];

static uint16_t get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

/* ---- the journal (JBD2 format) ----------------------------------------
 * Metadata blocks (directories, indirect blocks, bitmaps, inode tables, the
 * superblock and descriptors) are not written to their places as an operation
 * runs. They collect in a running transaction held in memory, and a commit
 * writes them to the log first -- descriptor, blocks, commit block, each step
 * made durable before the next -- then to their places, then marks the log
 * empty. A crash at any point leaves either the old state or a complete log
 * that mount replays. File data is written first (ordered mode): a committed
 * metadata block never points at data that is not on the medium. Only one
 * transaction is ever in the log, so no revoke records are needed, and the
 * log always starts at its first block. */
enum {
    JBD2_MAGIC = 0xC03B3998u,
    JBD2_DESCRIPTOR = 1,
    JBD2_COMMIT = 2,
    JBD2_SUPERBLOCK_V2 = 4,
    JBD2_FLAG_ESCAPE = 1,
    JBD2_FLAG_SAME_UUID = 2,
    JBD2_FLAG_LAST_TAG = 8,
    JBD2_TAG_BYTES = 8,
    JBD2_HEADER_BYTES = 12,
    JBD2_TXN_CAPACITY = 256,
};

struct ext2_journal {
    uint32_t length;      /* blocks in the log, block 0 is its superblock */
    uint32_t sequence;    /* the next transaction id */
    uint32_t count;       /* blocks in the running transaction (with holes) */
    uint32_t live;        /* of them, not forgotten */
    uint32_t *map;        /* length entries: the log's physical blocks */
    uint32_t *lbas;       /* JBD2_TXN_CAPACITY */
    uint16_t *hash;       /* 2 * JBD2_TXN_CAPACITY slots: index + 1 */
    uint8_t *data;        /* JBD2_TXN_CAPACITY blocks */
    uint8_t *scratch;     /* one block: descriptors, commit, log superblock */
    uint8_t *scratch2;    /* a block copy being written to the log */
    uint8_t *scratch3;    /* a descriptor being replayed */
    uint16_t *order;      /* the commit's list of live blocks */
    bool *escaped;        /* which of a descriptor's blocks need escaping */
    /* Data blocks written since the last commit.  A commit flushes these
     * before the metadata that points at them (ordered mode), so fsync does
     * not have to drain the whole cache.  Overflow falls back to a full
     * flush. */
    uint32_t *data_lbas;  /* JBD2_TXN_CAPACITY */
    uint32_t *flush_lbas; /* JBD2_TXN_CAPACITY: a sorted copy for one flush */
    unsigned data_count;
    bool data_overflow;
    bool recovering;
};

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static unsigned txn_slot(const ext2_journal_t *j, uint32_t lba)
{
    (void)j;
    return (lba * 2654435761u) % (2u * JBD2_TXN_CAPACITY);
}

/* Index of `lba` in the running transaction, or -1. */
static int txn_find(const ext2_journal_t *j, uint32_t lba)
{
    unsigned slot = txn_slot(j, lba);
    for (unsigned probes = 0; probes < 2u * JBD2_TXN_CAPACITY; ++probes) {
        const uint16_t entry = j->hash[slot];
        if (entry == 0) return -1;
        if (j->lbas[entry - 1] == lba) return (int)entry - 1;
        slot = (slot + 1) % (2u * JBD2_TXN_CAPACITY);
    }
    return -1;
}

static bool read_block(ext2_t *fs, uint32_t lba, void *buffer)
{
    if (fs->journal != NULL && fs->journal->live != 0) {
        const int index = txn_find(fs->journal, lba);
        if (index >= 0) {
            memcpy(buffer, fs->journal->data + (size_t)index * EXT2_BLOCK_BYTES,
                   EXT2_BLOCK_BYTES);
            return true;
        }
    }
    return ext2_blockdev_read(fs->device, lba, buffer, 1);
}

/* File data and anything written outside a transaction: straight to the
 * device. */
static bool io_failed(ext2_t *fs);
static ext2_status_t lookup_status_(const ext2_t *fs, ext2_status_t status);
static void dir_add_hint_invalidate_inode(ext2_t *fs, uint32_t inode);

static bool write_block(ext2_t *fs, uint32_t lba, const void *buffer)
{
    if (fs->read_only) return false;
    if (!ext2_blockdev_write(fs->device, lba, buffer, 1)) {
        return io_failed(fs);
    }
    ext2_journal_t *const j = fs->journal;
    if (j != NULL && !j->recovering) {
        /* Remember data blocks so a commit can flush just these before the
         * metadata that names them, instead of draining the whole cache. */
        if (j->data_count < JBD2_TXN_CAPACITY) {
            j->data_lbas[j->data_count++] = lba;
        } else {
            j->data_overflow = true;
        }
    }
    return true;
}

/* Record the LBAs of a multi-block data write so journal_commit() still knows
 * which blocks must be flushed before the metadata that names them. */
static void note_data_written(ext2_t *fs, uint32_t lba, uint32_t count)
{
    ext2_journal_t *const j = fs->journal;
    if (j == NULL || j->recovering) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (j->data_count < JBD2_TXN_CAPACITY)
            j->data_lbas[j->data_count++] = lba + i;
        else
            j->data_overflow = true;
    }
}

static bool journal_commit(ext2_t *fs);
static bool bitmap_test(const uint8_t *bitmap, unsigned bit);
static bool inode_number_valid(const ext2_t *fs, unsigned inode);
static ext2_status_t bmap_get(ext2_t *fs, const ext2_inode_t *inode,
                              uint32_t index, uint32_t *out);
static ext2_status_t bmap_set(ext2_t *fs, ext2_inode_t *inode, uint32_t index,
                              uint32_t phys);

/* Metadata: into the running transaction (or straight out on a volume with no
 * journal). */
static bool write_meta(ext2_t *fs, uint32_t lba, const void *buffer)
{
    ext2_journal_t *const j = fs->journal;
    if (j == NULL || j->recovering) return write_block(fs, lba, buffer);
    int index = txn_find(j, lba);
    if (index < 0) {
        if (j->count >= JBD2_TXN_CAPACITY && !journal_commit(fs)) return false;
        index = (int)j->count++;
        ++j->live;
        j->lbas[index] = lba;
        unsigned slot = txn_slot(j, lba);
        while (j->hash[slot] != 0) slot = (slot + 1) % (2u * JBD2_TXN_CAPACITY);
        j->hash[slot] = (uint16_t)(index + 1);
    }
    memcpy(j->data + (size_t)index * EXT2_BLOCK_BYTES, buffer, EXT2_BLOCK_BYTES);
    return true;
}

/* A block that is freed is no longer metadata: it must not be written to the
 * log, where replay would overwrite whatever it holds by then. */
static void txn_forget(ext2_t *fs, uint32_t lba)
{
    ext2_journal_t *const j = fs->journal;
    if (j == NULL) return;
    const int index = txn_find(j, lba);
    if (index < 0) return;
    j->lbas[index] = UINT32_MAX; /* the hash entry stays; it no longer matches */
    --j->live;
}

static bool fs_ready(const ext2_t *fs)
{
    return fs != NULL && fs->mounted && fs->device != NULL;
}

/* A write to the medium failed.  The in-memory metadata may now be ahead of
 * what is on the volume, so nothing else may be written until it is mounted
 * again: a later flush would otherwise push a half-applied change. */
static bool io_failed(ext2_t *fs)
{
    if (fs != NULL) fs->read_only = true;
    return false;
}

static ext2_status_t mutation_status(const ext2_t *fs)
{
    if (!fs_ready(fs)) return EXT2_INVALID;
    return fs->read_only ? EXT2_READ_ONLY : EXT2_OK;
}

bool ext2_read_only(const ext2_t *fs)
{
    return fs != NULL && fs->read_only;
}

static void *(*ext2_allocate)(size_t) = NULL;
static void (*ext2_release)(void *) = NULL;

void ext2_set_allocator(void *(*allocate)(size_t bytes),
                        void (*release)(void *memory))
{
    ext2_allocate = allocate;
    ext2_release = release;
}

static void *metadata_alloc(size_t bytes)
{
    void *memory = ext2_allocate != NULL ? ext2_allocate(bytes) : malloc(bytes);
    if (memory != NULL) memset(memory, 0, bytes);
    return memory;
}

static void metadata_free(void *memory)
{
    if (memory == NULL) return;
    if (ext2_release != NULL) ext2_release(memory); else free(memory);
}

/* Metadata caches. Only the group descriptors scale with the volume; bitmaps
 * and inode-table blocks are read on demand and kept in fixed-size LRUs. */
enum {
    EXT2_BITMAP_CACHE_ENTRIES = 32u,
    /* Deep directory trees hold a mutable pointer at every level. 128 entries
     * leave room for the 64-level recursion limit plus the caller's own inodes
     * without evicting a block that code is still holding. */
    EXT2_ITABLE_CACHE_ENTRIES = 128u,
};

typedef struct {
    uint32_t group;
    uint32_t used;
    uint8_t dirty_block;
    uint8_t dirty_inode;
    uint8_t valid;
    uint8_t block_bitmap[EXT2_BLOCK_BYTES];
    uint8_t inode_bitmap[EXT2_BLOCK_BYTES];
} ext2_bitmap_cache_entry_t;

typedef struct {
    uint32_t lba;
    uint32_t used;
    uint8_t dirty;
    uint8_t valid;
    /* Set while inode_ptr() has handed the block to a caller. Eviction skips
     * pinned entries; sync_metadata() clears them at the operation boundary. */
    uint8_t pinned;
    /* inode_ptr() casts this payload to ext2_inode_t *. Keep it naturally
     * aligned instead of relying on the unaligned-access trap handler. */
    _Alignas(ext2_inode_t) uint8_t data[EXT2_BLOCK_BYTES];
} ext2_itable_cache_entry_t;

static_assert(offsetof(ext2_itable_cache_entry_t, data) % _Alignof(ext2_inode_t) == 0,
              "inode cache payload must be aligned for ext2_inode_t");

static void recount_group_(ext2_t *fs, const ext2_bitmap_cache_entry_t *entry);

static ext2_bitmap_cache_entry_t *bitmap_cache_(ext2_t *fs)
{
    return (ext2_bitmap_cache_entry_t *)fs->bitmap_cache;
}

static ext2_itable_cache_entry_t *itable_cache_(ext2_t *fs)
{
    return (ext2_itable_cache_entry_t *)fs->itable_cache;
}

static bool bitmap_entry_flush_(ext2_t *fs, ext2_bitmap_cache_entry_t *entry)
{
    if (!entry->valid) return true;
    bool ok = true;
    if (entry->dirty_block) {
        ok = write_meta(fs, fs->groups[entry->group].block_bitmap, entry->block_bitmap);
        if (ok) entry->dirty_block = 0;
    }
    if (ok && entry->dirty_inode) {
        ok = write_meta(fs, fs->groups[entry->group].inode_bitmap, entry->inode_bitmap);
        if (ok) entry->dirty_inode = 0;
    }
    if (!ok) fs->cache_io_error = true;
    return ok;
}

static bool itable_entry_flush_(ext2_t *fs, ext2_itable_cache_entry_t *entry)
{
    if (!entry->valid || !entry->dirty) return true;
    if (!write_meta(fs, entry->lba, entry->data)) {
        fs->cache_io_error = true;
        return false;
    }
    entry->dirty = 0;
    return true;
}

/* Return the cached bitmap pair for a group, loading it on a miss. */
static ext2_bitmap_cache_entry_t *bitmap_cache_get_(ext2_t *fs, uint32_t group)
{
    if (group >= fs->group_count) return NULL;
    ext2_bitmap_cache_entry_t *const entries = bitmap_cache_(fs);
    ext2_bitmap_cache_entry_t *found = NULL;
    ext2_bitmap_cache_entry_t *victim = NULL;
    uint32_t oldest = UINT32_MAX;
    for (unsigned i = 0; i < EXT2_BITMAP_CACHE_ENTRIES; ++i) {
        ext2_bitmap_cache_entry_t *const entry = &entries[i];
        if (entry->valid && entry->group == group) {
            found = entry;
            break;
        }
        if (!entry->valid) {
            if (victim == NULL || victim->valid) victim = entry;
        } else if ((victim == NULL || victim->valid) && entry->used < oldest) {
            oldest = entry->used;
            victim = entry;
        }
    }
    if (found != NULL) {
        found->used = ++fs->cache_clock;
        return found;
    }
    if (victim == NULL) victim = &entries[0];
    if (victim->valid && !bitmap_entry_flush_(fs, victim)) return NULL;
    victim->valid = 0;
    if (!read_block(fs, fs->groups[group].block_bitmap, victim->block_bitmap) ||
        !read_block(fs, fs->groups[group].inode_bitmap, victim->inode_bitmap)) {
        return NULL;
    }
    victim->group = group;
    victim->dirty_block = 0;
    victim->dirty_inode = 0;
    /* The formatter fills cache entries itself before the volume is mounted;
     * only a mounted volume needs its on-disk counts corrected. */
    if (fs->mounted) recount_group_(fs, victim);
    victim->valid = 1;
    victim->used = ++fs->cache_clock;
    return victim;
}

static uint8_t *block_bitmap_for_(ext2_t *fs, uint32_t group)
{
    ext2_bitmap_cache_entry_t *const entry = bitmap_cache_get_(fs, group);
    return entry == NULL ? NULL : entry->block_bitmap;
}

static uint8_t *inode_bitmap_for_group_(ext2_t *fs, uint32_t group)
{
    ext2_bitmap_cache_entry_t *const entry = bitmap_cache_get_(fs, group);
    return entry == NULL ? NULL : entry->inode_bitmap;
}

static void block_bitmap_mark_dirty_(ext2_t *fs, uint32_t group)
{
    ext2_bitmap_cache_entry_t *const entry = bitmap_cache_get_(fs, group);
    if (entry != NULL) entry->dirty_block = 1;
}

static void inode_bitmap_mark_dirty_(ext2_t *fs, uint32_t group)
{
    ext2_bitmap_cache_entry_t *const entry = bitmap_cache_get_(fs, group);
    if (entry != NULL) entry->dirty_inode = 1;
}

/* Return a cached inode-table block, reading it from the medium on a miss.
 * A free inode slot is cleared by alloc_inode() itself; zeroing the whole
 * block here would destroy the other, still-live inodes it holds. */
static ext2_itable_cache_entry_t *itable_cache_get_(ext2_t *fs, uint32_t lba)
{
    if (lba == UINT32_MAX || lba >= fs->block_count) return NULL;
    ext2_itable_cache_entry_t *const entries = itable_cache_(fs);
    ext2_itable_cache_entry_t *found = NULL;
    ext2_itable_cache_entry_t *victim = NULL;
    uint32_t oldest = UINT32_MAX;
    for (unsigned i = 0; i < EXT2_ITABLE_CACHE_ENTRIES; ++i) {
        ext2_itable_cache_entry_t *const entry = &entries[i];
        if (entry->valid && entry->lba == lba) {
            found = entry;
            break;
        }
        if (!entry->valid) {
            if (victim == NULL || victim->valid) victim = entry;
        } else if (!entry->pinned && (victim == NULL || victim->valid) &&
                   entry->used < oldest) {
            oldest = entry->used;
            victim = entry;
        }
    }
    if (found != NULL) {
        found->used = ++fs->cache_clock;
        return found;
    }
    /* Every entry still held by the current operation stays put. Failing the
     * access is safer than invalidating a pointer the caller will write through. */
    if (victim == NULL) return NULL;
    if (victim->valid && !itable_entry_flush_(fs, victim)) return NULL;
    victim->valid = 0;
    if (!read_block(fs, lba, victim->data)) return NULL;
    victim->lba = lba;
    victim->dirty = 0;
    victim->pinned = 0;
    victim->valid = 1;
    victim->used = ++fs->cache_clock;
    return victim;
}

/* The physical block holding an inode's table entry. */
static uint32_t inode_table_lba_(const ext2_t *fs, uint32_t inode, size_t *offset)
{
    if (!inode_number_valid(fs, inode)) return UINT32_MAX;
    const uint32_t group = (inode - 1) / fs->inodes_per_group;
    if (group >= fs->group_count) return UINT32_MAX;
    const size_t in_group =
        (size_t)((inode - 1) % fs->inodes_per_group) * fs->inode_size;
    const uint32_t within = (uint32_t)(in_group / EXT2_BLOCK_BYTES);
    if (within >= fs->itable_blocks_per_group) return UINT32_MAX;
    *offset = in_group % EXT2_BLOCK_BYTES;
    return fs->groups[group].inode_table + within;
}

/* Give the volume the memory for its metadata. Group descriptors scale with
 * geometry; the two caches are fixed size and live in PSRAM when available. */
static bool metadata_attach(ext2_t *fs)
{
    const size_t gdt_bytes = (size_t)fs->group_count * sizeof(ext2_group_desc_t);
    const size_t gdt_aligned = (gdt_bytes + 15) & ~(size_t)15;
    const size_t bitmap_bytes = (size_t)EXT2_BITMAP_CACHE_ENTRIES * sizeof(ext2_bitmap_cache_entry_t);
    const size_t itable_bytes = (size_t)EXT2_ITABLE_CACHE_ENTRIES * sizeof(ext2_itable_cache_entry_t);
    const size_t total = gdt_aligned + bitmap_bytes + itable_bytes;
    uint8_t *memory = metadata_alloc(total);
    if (memory == NULL) return false;
    fs->memory = memory;
    fs->groups = (ext2_group_desc_t *)memory;
    memory += gdt_aligned;
    fs->bitmap_cache = memory;
    memset(fs->bitmap_cache, 0, bitmap_bytes);
    memory += bitmap_bytes;
    fs->itable_cache = memory;
    memset(fs->itable_cache, 0, itable_bytes);
    fs->cache_clock = 0;
    fs->cache_io_error = false;
    return true;
}

/* Journal replay writes metadata straight to the medium. Drop every cached
 * copy before reading it again, or later flushes would push stale data back. */
static void cache_invalidate(ext2_t *fs)
{
    ext2_bitmap_cache_entry_t *const bitmaps = bitmap_cache_(fs);
    for (unsigned i = 0; i < EXT2_BITMAP_CACHE_ENTRIES; ++i) {
        bitmaps[i].valid = 0;
        bitmaps[i].dirty_block = 0;
        bitmaps[i].dirty_inode = 0;
    }
    ext2_itable_cache_entry_t *const tables = itable_cache_(fs);
    for (unsigned i = 0; i < EXT2_ITABLE_CACHE_ENTRIES; ++i) {
        tables[i].valid = 0;
        tables[i].dirty = 0;
        tables[i].pinned = 0;
    }
}

static void journal_free(ext2_t *fs)
{
    ext2_journal_t *const j = fs->journal;
    if (j == NULL) return;
    metadata_free(j->map);
    metadata_free(j->lbas);
    metadata_free(j->hash);
    metadata_free(j->data);
    metadata_free(j->scratch);
    metadata_free(j->scratch2);
    metadata_free(j->scratch3);
    metadata_free(j->order);
    metadata_free(j->escaped);
    metadata_free(j->data_lbas);
    metadata_free(j->flush_lbas);
    metadata_free(j);
    fs->journal = NULL;
}

/* Memory for a journal of `length` blocks; the log's physical blocks are
 * filled in by the caller. */
static bool journal_alloc(ext2_t *fs, uint32_t length)
{
    ext2_journal_t *const j = metadata_alloc(sizeof(*j));
    if (j == NULL) return false;
    fs->journal = j;
    j->length = length;
    j->sequence = 1;
    j->map = metadata_alloc((size_t)length * sizeof(uint32_t));
    j->lbas = metadata_alloc(JBD2_TXN_CAPACITY * sizeof(uint32_t));
    j->hash = metadata_alloc(2u * JBD2_TXN_CAPACITY * sizeof(uint16_t));
    j->data = metadata_alloc((size_t)JBD2_TXN_CAPACITY * EXT2_BLOCK_BYTES);
    j->scratch = metadata_alloc(EXT2_BLOCK_BYTES);
    j->scratch2 = metadata_alloc(EXT2_BLOCK_BYTES);
    j->scratch3 = metadata_alloc(EXT2_BLOCK_BYTES);
    j->order = metadata_alloc(JBD2_TXN_CAPACITY * sizeof(uint16_t));
    j->escaped = metadata_alloc(JBD2_TXN_CAPACITY * sizeof(bool));
    j->data_lbas = metadata_alloc(JBD2_TXN_CAPACITY * sizeof(uint32_t));
    j->flush_lbas = metadata_alloc(JBD2_TXN_CAPACITY * sizeof(uint32_t));
    if (j->map == NULL || j->lbas == NULL || j->hash == NULL ||
        j->data == NULL || j->scratch == NULL || j->scratch2 == NULL ||
        j->scratch3 == NULL || j->order == NULL || j->escaped == NULL ||
        j->data_lbas == NULL || j->flush_lbas == NULL) {
        journal_free(fs);
        return false;
    }
    return true;
}

static void metadata_detach(ext2_t *fs)
{
    journal_free(fs);
    metadata_free(fs->memory);
    fs->memory = NULL;
    fs->groups = NULL;
    fs->bitmap_cache = NULL;
    fs->itable_cache = NULL;
}

/* Inode numbers come from directory entries on the medium.  A damaged volume
 * can name inode 0 or one past the table; such a number must never index the
 * table.  It resolves to an all-zero inode (neither directory nor symlink, no
 * blocks), and writes through the mutable pointer land in a scratch copy. */
static const ext2_inode_t s_invalid_inode;
static ext2_inode_t s_scratch_inode;

/* A sequential listing asks for index 0, 1, 2, ...; without this hint every
 * call re-walked the directory from its first block, making a full listing
 * O(entries^2).  The hint remembers where the last returned entry ended.  It
 * is valid only for the same volume, directory and directory generation, so
 * any add, remove or new directory invalidates it. */
typedef struct {
    bool valid;
    ext2_t *fs;
    uint32_t dir_inode;
    uint32_t generation;
    unsigned next_index;
    uint32_t block;
    size_t offset;
} dir_scan_hint_t;

static dir_scan_hint_t dir_scan_hint;


static bool inode_number_valid(const ext2_t *fs, unsigned inode)
{
    return inode >= 1 && inode <= fs->inode_count;
}

/* An inode is only real when its bitmap bit is set. Reading the bitmap on
 * demand keeps freed slots from looking like live inodes on host-formatted
 * volumes, without loading every bitmap at mount. */
static bool inode_allocated_(ext2_t *fs, uint32_t inode, uint32_t *out_group)
{
    if (!inode_number_valid(fs, inode)) return false;
    const uint32_t group = (inode - 1) / fs->inodes_per_group;
    const unsigned bit = (unsigned)((inode - 1) % fs->inodes_per_group);
    uint8_t *const bitmap = inode_bitmap_for_group_(fs, group);
    if (bitmap == NULL) return false;
    if (out_group != NULL) *out_group = group;
    return bitmap_test(bitmap, bit);
}

/* A mutable inode pointer. The table block is pinned in the cache and marked
 * dirty; LRU eviction writes it back before reuse. */
static ext2_inode_t *inode_ptr(ext2_t *fs, uint32_t inode)
{
    if (!inode_allocated_(fs, inode, NULL)) {
        memset(&s_scratch_inode, 0, sizeof(s_scratch_inode));
        return &s_scratch_inode;
    }
    size_t offset;
    const uint32_t lba = inode_table_lba_(fs, inode, &offset);
    /* An allocated inode already has a table entry on the medium; only
     * alloc_inode() creates a fresh zeroed block before setting its bit. */
    ext2_itable_cache_entry_t *const entry = itable_cache_get_(fs, lba);
    if (entry == NULL) {
        /* Writing into the scratch copy would be lost at commit. Fail the
         * mount's write path instead of silently corrupting the operation. */
        io_failed(fs);
        memset(&s_scratch_inode, 0, sizeof(s_scratch_inode));
        return &s_scratch_inode;
    }
    entry->dirty = 1;
    entry->pinned = 1;
    return (ext2_inode_t *)(entry->data + offset);
}

static const ext2_inode_t *const_inode_ptr(ext2_t *fs, uint32_t inode)
{
    if (!inode_allocated_(fs, inode, NULL)) return &s_invalid_inode;
    size_t offset;
    const uint32_t lba = inode_table_lba_(fs, inode, &offset);
    ext2_itable_cache_entry_t *const entry = itable_cache_get_(fs, lba);
    if (entry == NULL) return &s_invalid_inode;
    return (const ext2_inode_t *)(entry->data + offset);
}

static bool inode_is_dir(const ext2_inode_t *inode)
{
    return EXT2_S_ISDIR(inode->mode);
}

static bool inode_is_symlink(const ext2_inode_t *inode)
{
    return EXT2_S_ISLNK(inode->mode);
}

static bool read_super(ext2_t *fs)
{
    if (!read_block(fs, 0, io_block)) return false;
    memcpy(&fs->super, io_block + EXT2_SUPER_OFFSET, sizeof(fs->super));
    return true;
}

static bool write_super(ext2_t *fs)
{
    if (fs->read_only) return false;
    if (!read_block(fs, 0, io_block)) return false;
    memcpy(io_block + EXT2_SUPER_OFFSET, &fs->super, sizeof(fs->super));
    return write_meta(fs, 0, io_block);
}

static unsigned gdt_blocks(const ext2_t *fs)
{
    return (unsigned)((fs->group_count * sizeof(ext2_group_desc_t) +
                       EXT2_BLOCK_BYTES - 1) / EXT2_BLOCK_BYTES);
}

static bool read_groups(ext2_t *fs)
{
    for (unsigned b = 0; b < gdt_blocks(fs); ++b) {
        if (!read_block(fs, EXT2_GDT_BLOCK + b, io_block)) return false;
        const size_t first = (size_t)b * (EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t));
        size_t count = fs->group_count - first;
        if (count > EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t)) {
            count = EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t);
        }
        memcpy(fs->groups + first, io_block, count * sizeof(ext2_group_desc_t));
    }
    return true;
}

static bool write_groups(ext2_t *fs)
{
    if (fs->read_only) return false;
    for (unsigned b = 0; b < gdt_blocks(fs); ++b) {
        memset(io_block, 0, sizeof(io_block));
        const size_t first = (size_t)b * (EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t));
        size_t count = fs->group_count - first;
        if (count > EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t)) {
            count = EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t);
        }
        memcpy(io_block, fs->groups + first, count * sizeof(ext2_group_desc_t));
        if (!write_meta(fs, EXT2_GDT_BLOCK + b, io_block)) return false;
    }
    return true;
}

/* Write what is dirty in the fixed caches. Nothing here loops over the
 * volume's group count. Pins last only until this operation's writeback. */
static bool write_dirty_metadata(ext2_t *fs)
{
    if (fs->read_only) {
        ext2_itable_cache_entry_t *const tables = itable_cache_(fs);
        for (unsigned i = 0; i < EXT2_ITABLE_CACHE_ENTRIES; ++i) tables[i].pinned = 0;
        return false;
    }
    bool ok = true;
    ext2_bitmap_cache_entry_t *const bitmaps = bitmap_cache_(fs);
    for (unsigned i = 0; i < EXT2_BITMAP_CACHE_ENTRIES; ++i) {
        if (!bitmap_entry_flush_(fs, &bitmaps[i])) ok = false;
    }
    ext2_itable_cache_entry_t *const tables = itable_cache_(fs);
    for (unsigned i = 0; i < EXT2_ITABLE_CACHE_ENTRIES; ++i) {
        if (!itable_entry_flush_(fs, &tables[i])) ok = false;
        tables[i].pinned = 0;
    }
    return ok && !fs->cache_io_error;
}

static bool bitmap_test(const uint8_t *bitmap, unsigned bit)
{
    return (bitmap[bit / 8] & (uint8_t)(1u << (bit % 8))) != 0;
}

static void bitmap_set(uint8_t *bitmap, unsigned bit)
{
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bitmap_clear(uint8_t *bitmap, unsigned bit)
{
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* The counts on disk may be stale after a power cut; a bitmap is not. Recount
 * one group when its bitmaps first enter the cache, so no scan of the whole
 * volume is needed at mount. */
static void recount_group_(ext2_t *fs, const ext2_bitmap_cache_entry_t *entry)
{
    const uint32_t g = entry->group;
    ext2_group_desc_t *const d = &fs->groups[g];
    const uint64_t block_start = (uint64_t)g * fs->blocks_per_group;
    uint32_t blocks = block_start >= fs->block_count
                          ? 0
                          : (uint32_t)((uint64_t)fs->block_count - block_start);
    if (blocks > fs->blocks_per_group) blocks = fs->blocks_per_group;
    const uint64_t inode_start = (uint64_t)g * fs->inodes_per_group;
    uint32_t inodes = inode_start >= fs->inode_count
                          ? 0
                          : (uint32_t)((uint64_t)fs->inode_count - inode_start);
    if (inodes > fs->inodes_per_group) inodes = fs->inodes_per_group;

    uint32_t free_blocks = 0;
    for (uint32_t b = 0; b < blocks; ++b) {
        if (!bitmap_test(entry->block_bitmap, b)) ++free_blocks;
    }
    uint32_t free_inodes = 0;
    for (uint32_t i = 0; i < inodes; ++i) {
        if (!bitmap_test(entry->inode_bitmap, i)) ++free_inodes;
    }

    int64_t total = (int64_t)fs->free_blocks + free_blocks - d->free_blocks_count;
    if (total < 0) total = 0;
    else if ((uint64_t)total > fs->block_count) total = fs->block_count;
    fs->free_blocks = (uint32_t)total;
    total = (int64_t)fs->free_inodes + free_inodes - d->free_inodes_count;
    if (total < 0) total = 0;
    else if ((uint64_t)total > fs->inode_count) total = fs->inode_count;
    fs->free_inodes = (uint32_t)total;
    d->free_blocks_count = (uint16_t)free_blocks;
    d->free_inodes_count = (uint16_t)free_inodes;
}

/* Write the superblock and group descriptors: their only news is the free
 * counts, which are recomputed from the bitmaps at mount, so they are written
 * when the file system is committed (sync, idle flush, unmount) and not after
 * every operation. */
static bool write_summary(ext2_t *fs)
{
    if (fs->read_only) return false;
    fs->super.free_blocks_count = fs->free_blocks;
    fs->super.free_inodes_count = fs->free_inodes;
    if (!write_super(fs) || !write_groups(fs)) return false;
    fs->summary_dirty = false;
    return true;
}

static bool sync_metadata(ext2_t *fs)
{
    if (fs->read_only) return false;
    fs->summary_dirty = true;
    if (!write_dirty_metadata(fs)) return false;
    /* Between operations, with the transaction filling up: a good moment. */
    if (fs->journal != NULL && fs->journal->count >= JBD2_TXN_CAPACITY * 3 / 4 &&
        !(write_summary(fs) && journal_commit(fs))) {
        return false;
    }
    return ext2_blockdev_sync(fs->device);
}


/* ---- journal: set up, commit, replay ---------------------------------- */

/* The superblock straight to the medium, bypassing the transaction (the
 * recovery flag must be on the medium before the log is). A commit publishes
 * the in-memory counts; replay must preserve the counts the log just wrote. */
static bool write_super_raw(ext2_t *fs, bool from_memory_counts)
{
    if (fs->read_only) return false;
    uint8_t *const block = fs->journal->scratch2;
    if (!ext2_blockdev_read(fs->device, 0, block, 1)) return false;
    if (from_memory_counts) {
        fs->super.free_blocks_count = fs->free_blocks;
        fs->super.free_inodes_count = fs->free_inodes;
    }
    memcpy(block + EXT2_SUPER_OFFSET, &fs->super, sizeof(fs->super));
    if (!ext2_blockdev_write(fs->device, 0, block, 1)) return io_failed(fs);
    return true;
}

static bool journal_write(ext2_t *fs, uint32_t index, const void *buffer)
{
    if (fs->read_only) return false;
    if (!ext2_blockdev_write(fs->device, fs->journal->map[index], buffer, 1)) {
        return io_failed(fs);
    }
    return true;
}

/* The log's own superblock: where the log starts (0 = empty) and the id the
 * next transaction will have. */
static bool journal_set_state(ext2_t *fs, uint32_t start, uint32_t sequence)
{
    ext2_journal_t *const j = fs->journal;
    if (!ext2_blockdev_read(fs->device, j->map[0], j->scratch, 1)) return false;
    put_be32(j->scratch + 24, sequence);
    put_be32(j->scratch + 28, start);
    return journal_write(fs, 0, j->scratch);
}

static void txn_reset(ext2_journal_t *j)
{
    j->count = 0;
    j->live = 0;
    j->data_count = 0;
    j->data_overflow = false;
    memset(j->hash, 0, 2u * JBD2_TXN_CAPACITY * sizeof(uint16_t));
}

/* Sort a block list and flush each contiguous run.  Insertion sort is near
 * linear for the sequential writes that dominate, and the list is at most the
 * transaction capacity. */
static bool flush_block_list(ext2_t *fs, uint32_t *list, unsigned count)
{
    for (unsigned i = 1; i < count; ++i) {
        const uint32_t value = list[i];
        unsigned at = i;
        while (at > 0 && list[at - 1] > value) {
            list[at] = list[at - 1];
            --at;
        }
        list[at] = value;
    }
    unsigned start = 0;
    while (start < count) {
        unsigned end = start + 1;
        while (end < count && list[end] == list[end - 1] + 1) ++end;
        if (!ext2_blockdev_flush_range(fs->device, list[start], end - start)) {
            return false;
        }
        start = end;
    }
    return true;
}

/* Ordered mode: the file data the metadata refers to goes first.  Only the
 * blocks written since the last commit are flushed; a list overflow (a very
 * large transfer) falls back to the whole cache. */
static bool flush_ordered_data(ext2_t *fs)
{
    ext2_journal_t *const j = fs->journal;
    if (j->data_overflow) return ext2_blockdev_flush(fs->device);
    memcpy(j->flush_lbas, j->data_lbas,
           (size_t)j->data_count * sizeof(uint32_t));
    if (!flush_block_list(fs, j->flush_lbas, j->data_count)) return false;
    j->data_count = 0;
    j->data_overflow = false;
    return true;
}

/* Flush the journal's own blocks [0, count).  The map need not be
 * contiguous, so each run is flushed separately. */
static bool flush_journal_prefix(ext2_t *fs, uint32_t count)
{
    const uint32_t *const map = fs->journal->map;
    uint32_t start = 0;
    while (start < count) {
        uint32_t end = start + 1;
        while (end < count && map[end] == map[end - 1] + 1) ++end;
        if (!ext2_blockdev_flush_range(fs->device, map[start], end - start)) {
            return false;
        }
        start = end;
    }
    return true;
}

/* The transaction's home blocks, after they have been copied out of the log. */
static bool flush_home_blocks(ext2_t *fs, const uint16_t *order, unsigned n)
{
    ext2_journal_t *const j = fs->journal;
    if (n > JBD2_TXN_CAPACITY) return ext2_blockdev_flush(fs->device);
    for (unsigned k = 0; k < n; ++k) {
        j->flush_lbas[k] = j->lbas[order[k]];
    }
    return flush_block_list(fs, j->flush_lbas, n);
}

/* Make the running transaction durable and apply it. */
static bool journal_commit(ext2_t *fs)
{
    ext2_journal_t *const j = fs->journal;
    if (j == NULL) return true;
    if (fs->read_only) return false;
    if (j->live == 0) {
        /* Data-only changes (overwriting an existing block) have no journal
         * entry but still need their ordered flush. */
        const bool ok = flush_ordered_data(fs);
        txn_reset(j);
        return ok;
    }
    /* Ordered mode: the file data the metadata refers to goes first. */
    if (!flush_ordered_data(fs)) return false;

    uint16_t *const order = j->order;
    bool *const escaped = j->escaped;
    unsigned n = 0;
    for (unsigned i = 0; i < j->count; ++i) {
        if (j->lbas[i] != UINT32_MAX) order[n++] = (uint16_t)i;
    }
    const unsigned per_descriptor =
        (EXT2_BLOCK_BYTES - JBD2_HEADER_BYTES - 16) / JBD2_TAG_BYTES;
    const unsigned descriptors = (n + per_descriptor - 1) / per_descriptor;
    if (1 + descriptors + n + 1 > j->length) return false;
    const uint32_t tid = j->sequence;

    /* A: the recovery flag, the log's start, then descriptors and blocks. */
    fs->super.feature_incompat |= EXT2_FEATURE_INCOMPAT_RECOVER;
    if (!write_super_raw(fs, true) || !journal_set_state(fs, 1, tid)) return false;
    uint32_t position = 1;
    for (unsigned done = 0; done < n;) {
        const unsigned chunk = n - done < per_descriptor ? n - done : per_descriptor;
        uint8_t *const desc = j->scratch;
        memset(desc, 0, EXT2_BLOCK_BYTES);
        put_be32(desc, JBD2_MAGIC);
        put_be32(desc + 4, JBD2_DESCRIPTOR);
        put_be32(desc + 8, tid);
        size_t at = JBD2_HEADER_BYTES;
        for (unsigned k = 0; k < chunk; ++k) {
            const unsigned i = order[done + k];
            const uint8_t *const block = j->data + (size_t)i * EXT2_BLOCK_BYTES;
            escaped[k] = get_be32(block) == JBD2_MAGIC;
            uint16_t flags = 0;
            if (escaped[k]) flags |= JBD2_FLAG_ESCAPE;
            if (k != 0) flags |= JBD2_FLAG_SAME_UUID;
            if (k + 1 == chunk) flags |= JBD2_FLAG_LAST_TAG;
            put_be32(desc + at, j->lbas[i]);
            desc[at + 4] = 0;
            desc[at + 5] = 0;
            desc[at + 6] = (uint8_t)(flags >> 8);
            desc[at + 7] = (uint8_t)flags;
            at += JBD2_TAG_BYTES;
            if (k == 0) at += 16; /* the volume's uuid, zero here */
        }
        if (!journal_write(fs, position++, desc)) return false;
        for (unsigned k = 0; k < chunk; ++k) {
            const unsigned i = order[done + k];
            uint8_t *const copy = j->scratch2;
            memcpy(copy, j->data + (size_t)i * EXT2_BLOCK_BYTES, EXT2_BLOCK_BYTES);
            if (escaped[k]) memset(copy, 0, 4);
            if (!journal_write(fs, position++, copy)) return false;
        }
        done += chunk;
    }
    /* The recovery flag on block 0 must be durable before the log blocks. */
    if (!ext2_blockdev_flush_range(fs->device, 0, 1) ||
        !flush_journal_prefix(fs, position)) {
        return false;
    }

    /* B: the commit block. Replay trusts the log only up to a commit. */
    memset(j->scratch, 0, EXT2_BLOCK_BYTES);
    put_be32(j->scratch, JBD2_MAGIC);
    put_be32(j->scratch + 4, JBD2_COMMIT);
    put_be32(j->scratch + 8, tid);
    if (!journal_write(fs, position, j->scratch) ||
        !ext2_blockdev_flush_range(fs->device, j->map[position], 1)) {
        return false;
    }

    /* C: the blocks go to their places. */
    for (unsigned k = 0; k < n; ++k) {
        const unsigned i = order[k];
        if (!write_block(fs, j->lbas[i], j->data + (size_t)i * EXT2_BLOCK_BYTES)) {
            return false;
        }
    }
    if (!flush_home_blocks(fs, order, n)) return false;

    /* D: the log is empty again. */
    fs->super.feature_incompat &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
    if (!write_super_raw(fs, true) || !journal_set_state(fs, 0, tid + 1) ||
        !ext2_blockdev_flush_range(fs->device, 0, 1) ||
        !flush_journal_prefix(fs, 1)) {
        return false;
    }
    j->sequence = tid + 1;
    txn_reset(j);
    return true;
}

static unsigned group_of_block(const ext2_t *fs, uint32_t block)
{
    return block / fs->blocks_per_group;
}

/* Find a free block: scan the bitmap from where the last search ended, so a
 * file grows into the space after its own previous block. */
static uint32_t alloc_block(ext2_t *fs)
{
    if (fs->read_only) return UINT32_MAX;
    const uint32_t total = fs->block_count;
    const uint32_t first_alloc = EXT2_GDT_BLOCK + gdt_blocks(fs);
    uint32_t block = fs->alloc_hint < total ? fs->alloc_hint : first_alloc;
    if (block < first_alloc) block = first_alloc;
    for (uint32_t scanned = 0; scanned < total; ++scanned, ++block) {
        if (block >= total) block = first_alloc;
        if (block < first_alloc) block = first_alloc;
        const unsigned g = group_of_block(fs, block);
        /* Do not trust the descriptor's free count: it may be stale after a
         * power cut. Loading the bitmap recounts the group lazily. */
        uint8_t *const bitmap = block_bitmap_for_(fs, g);
        if (bitmap == NULL) return UINT32_MAX;
        const uint32_t bit = block % fs->blocks_per_group;
        if (bit % 8 == 0 && bit + 8 <= fs->blocks_per_group && bitmap[bit / 8] == 0xFF) {
            block += 7;
            scanned += 7;
            continue;
        }
        if (!bitmap_test(bitmap, bit)) {
            bitmap_set(bitmap, bit);
            block_bitmap_mark_dirty_(fs, g);
            if (fs->groups[g].free_blocks_count > 0) --fs->groups[g].free_blocks_count;
            if (fs->free_blocks > 0) --fs->free_blocks;
            fs->summary_dirty = true;
            fs->alloc_hint = block + 1;
            return block;
        }
    }
    return UINT32_MAX;
}

/* Find a run of free blocks in one block group. Single-block allocations stay
 * with alloc_block(); this is for streaming whole ext2 blocks to SD in one
 * multi-block request. */
static uint32_t alloc_block_run(ext2_t *fs, uint32_t wanted, uint32_t *out_count)
{
    *out_count = 0;
    if (fs->read_only || wanted == 0 || fs->block_count <= fs->first_data_block)
        return UINT32_MAX;

    const uint32_t total = fs->block_count;
    const uint32_t first_alloc = EXT2_GDT_BLOCK + gdt_blocks(fs);
    uint32_t block = fs->alloc_hint < total ? fs->alloc_hint : first_alloc;
    if (block < first_alloc) block = first_alloc;
    for (uint32_t scanned = 0; scanned < total; ++scanned, ++block) {
        if (block >= total) block = first_alloc;
        if (block < first_alloc) block = first_alloc;

        const unsigned g = group_of_block(fs, block);
        /* The bitmap is the authority; the descriptor count may be stale. */
        const uint32_t bit = block % fs->blocks_per_group;
        uint32_t max = wanted;
        const uint32_t group_left = fs->blocks_per_group - bit;
        if (max > group_left) max = group_left;
        if (max > total - block) max = total - block;

        uint8_t *const bitmap = block_bitmap_for_(fs, g);
        if (bitmap == NULL) return UINT32_MAX;
        uint32_t count = 0;
        while (count < max && !bitmap_test(bitmap, bit + count)) ++count;
        if (count > fs->groups[g].free_blocks_count) count = fs->groups[g].free_blocks_count;
        if (count > fs->free_blocks) count = fs->free_blocks;
        if (count < 2) continue; /* let alloc_block() handle a lone block */

        for (uint32_t i = 0; i < count; ++i) bitmap_set(bitmap, bit + i);
        block_bitmap_mark_dirty_(fs, g);
        fs->groups[g].free_blocks_count -= count;
        fs->free_blocks -= count;
        fs->summary_dirty = true;
        fs->alloc_hint = block + count;
        *out_count = count;
        return block;
    }
    return UINT32_MAX;
}

static bool free_block(ext2_t *fs, uint32_t block)
{
    if (fs->read_only) return false;
    const uint32_t first_alloc = EXT2_GDT_BLOCK + gdt_blocks(fs);
    if (block < first_alloc || block >= fs->block_count) return false;
    const unsigned g = group_of_block(fs, block);
    uint8_t *const bitmap = block_bitmap_for_(fs, g);
    if (bitmap == NULL) return false;
    const unsigned bit = block % fs->blocks_per_group;
    if (!bitmap_test(bitmap, bit)) return false;
    bitmap_clear(bitmap, bit);
    txn_forget(fs, block);
    block_bitmap_mark_dirty_(fs, g);
    if (fs->groups[g].free_blocks_count < fs->blocks_per_group) ++fs->groups[g].free_blocks_count;
    if (fs->free_blocks < fs->block_count) ++fs->free_blocks;
    fs->summary_dirty = true;
    if (block < fs->alloc_hint) fs->alloc_hint = block;
    return true;
}

static uint8_t *inode_bitmap_of(ext2_t *fs, unsigned inode, unsigned *bit,
                                unsigned *group)
{
    const unsigned index = inode - 1;
    *group = index / fs->inodes_per_group;
    *bit = index % fs->inodes_per_group;
    return inode_bitmap_for_group_(fs, *group);
}

/* Inode numbers start at 1, so 0 is a safe error sentinel even when the
 * volume's inode count reaches UINT32_MAX. */
static uint32_t alloc_inode(ext2_t *fs)
{
    if (fs->read_only) return 0;
    const uint32_t total = fs->inode_count;
    if (total < fs->first_ino) return 0;
    uint32_t inode = fs->inode_hint;
    if (inode < fs->first_ino || inode > total) inode = fs->first_ino;
    /* Start where the last allocation ended and wrap once, rather than
     * rescanning the whole table from the first usable inode on every create:
     * adding N files was O(N^2) inodes on a large volume. */
    const uint32_t span = total - fs->first_ino + 1;
    for (uint32_t scanned = 0; scanned < span; ++scanned) {
        unsigned bit, group;
        uint8_t *const bitmap = inode_bitmap_of(fs, inode, &bit, &group);
        if (bitmap == NULL) return 0;
        if (!bitmap_test(bitmap, bit)) {
            bitmap_set(bitmap, bit);
            inode_bitmap_mark_dirty_(fs, group);
            if (fs->groups[group].free_inodes_count > 0) --fs->groups[group].free_inodes_count;
            if (fs->free_inodes > 0) --fs->free_inodes;
            fs->summary_dirty = true;
            dir_add_hint_invalidate_inode(fs, inode);
            /* inode_ptr() loads the table block from the medium on a miss.
             * Only this inode's entry is cleared: other entries in the block
             * may belong to live files. */
            ext2_inode_t *const out = inode_ptr(fs, inode);
            if (fs->read_only) {
                bitmap_clear(bitmap, bit);
                inode_bitmap_mark_dirty_(fs, group);
                if (fs->groups[group].free_inodes_count < fs->inodes_per_group)
                    ++fs->groups[group].free_inodes_count;
                if (fs->free_inodes < fs->inode_count) ++fs->free_inodes;
                return 0;
            }
            memset(out, 0, fs->inode_size);
            fs->inode_hint = inode < total ? inode + 1 : fs->first_ino;
            return inode;
        }
        if (inode == total) inode = fs->first_ino;
        else ++inode;
    }
    return 0;
}

static void free_inode(ext2_t *fs, uint32_t inode)
{
    if (fs->read_only) return;
    if (inode < fs->first_ino || inode > fs->inode_count) return;
    /* Any cached listing or lookup for this inode is now stale. */
    ++fs->dir_generation;
    dir_add_hint_invalidate_inode(fs, inode);
    /* Take the table pointer while the allocation bit still says the inode is
     * live; clearing the bit first would make inode_ptr() return the scratch
     * copy and leave the real entry untouched. */
    ext2_inode_t *const entry = inode_ptr(fs, inode);
    unsigned bit, group;
    uint8_t *const bitmap = inode_bitmap_of(fs, inode, &bit, &group);
    if (bitmap == NULL) return;
    bitmap_clear(bitmap, bit);
    inode_bitmap_mark_dirty_(fs, group);
    if (fs->groups[group].free_inodes_count < fs->inodes_per_group) ++fs->groups[group].free_inodes_count;
    if (fs->free_inodes < fs->inode_count) ++fs->free_inodes;
    fs->summary_dirty = true;
    if (inode < fs->inode_hint) fs->inode_hint = inode;
    memset(entry, 0, fs->inode_size);
}

/* Directories are counted per group, as e2fsck checks. */
static void count_dir(ext2_t *fs, uint32_t inode, int delta)
{
    const unsigned group = (inode - 1) / fs->inodes_per_group;
    if (delta > 0) {
        ++fs->groups[group].used_dirs_count;
        ++fs->dir_count;
    } else {
        if (fs->groups[group].used_dirs_count > 0) --fs->groups[group].used_dirs_count;
        if (fs->dir_count > 0) --fs->dir_count;
    }
    fs->summary_dirty = true;
}

static uint32_t (*ext2_clock)(void);

void ext2_set_clock(uint32_t (*now)(void))
{
    ext2_clock = now;
}

static uint32_t now_seconds(void)
{
    const uint32_t now = ext2_clock != NULL ? ext2_clock() : 0;
    return now != 0 ? now : EXT2_EPOCH;
}

/* Data or directory contents changed. atime is deliberately left alone: a
 * flash-backed volume behaves as if mounted noatime. */
static void touch_modified(ext2_inode_t *inode)
{
    inode->mtime = inode->ctime = now_seconds();
}

/* Only inode metadata (mode, owner, link count) changed. */
static void touch_changed(ext2_inode_t *inode)
{
    inode->ctime = now_seconds();
}

static void init_inode(ext2_t *fs, ext2_inode_t *inode, uint16_t mode)
{
    memset(inode, 0, fs->inode_size);
    inode->mode = mode;
    inode->uid = 0;
    inode->gid = 0;
    inode->atime = inode->ctime = inode->mtime = now_seconds();
    inode->links_count = 1;
}

static size_t dirent_name_len(const ext2_t *fs, const uint8_t *entry)
{
    /* Revision 0 (and volumes without the FILETYPE feature) use a 16-bit
     * name_len.  The high byte became the file type later, so it must not be
     * treated as part of the name when the feature is absent. */
    return fs->has_filetype ? entry[6] : get_u16(entry + 6);
}

static size_t dirent_used(const ext2_t *fs, const uint8_t *entry)
{
    return (dirent_name_len(fs, entry) + 8u + 3u) & ~(size_t)3u;
}

/* A live entry's name must lie inside its record, which must lie inside the
 * block; otherwise a crafted image makes name compares read past io_block. */
static bool dirent_fits(const ext2_t *fs, const uint8_t *entry, uint16_t rec_len, size_t offset)
{
    if (rec_len < 8 || rec_len > EXT2_BLOCK_BYTES - offset) return false;
    const size_t name_len = dirent_name_len(fs, entry);
    if (name_len > 255) return false;
    return get_u32(entry) == 0 || 8u + name_len <= rec_len;
}

static uint8_t inode_dirent_type(const ext2_inode_t *inode)
{
    if (inode == NULL) return 0;
    if (EXT2_S_ISDIR(inode->mode)) return 2;
    if (EXT2_S_ISREG(inode->mode)) return 1;
    if (EXT2_S_ISLNK(inode->mode)) return 7;
    if (EXT2_S_ISCHR(inode->mode)) return 3;
    if (EXT2_S_ISBLK(inode->mode)) return 4;
    if (EXT2_S_ISFIFO(inode->mode)) return 5;
    if (EXT2_S_ISSOCK(inode->mode)) return 6;
    return 0;
}

static uint8_t dirent_type(ext2_t *fs, const uint8_t *entry)
{
    if (fs->has_filetype) return entry[7];
    const uint32_t inode = get_u32(entry);
    if (!inode_number_valid(fs, inode)) return 0;
    return inode_dirent_type(const_inode_ptr(fs, inode));
}

static bool dir_block_ok(const ext2_t *fs, uint32_t block)
{
    return block >= fs->first_data_block && block < fs->block_count;
}

static void dirent_write(const ext2_t *fs, uint8_t *block, size_t offset,
                         uint32_t inode, uint16_t rec_len, const char *name,
                         uint8_t type)
{
    const size_t name_len = strlen(name);
    put_u32(block + offset, inode);
    put_u16(block + offset + 4, rec_len);
    if (fs->has_filetype) {
        block[offset + 6] = (uint8_t)name_len;
        block[offset + 7] = type;
    } else {
        put_u16(block + offset + 6, (uint16_t)name_len);
    }
    memcpy(block + offset + 8, name, name_len);
    if (rec_len > 8 + name_len) {
        memset(block + offset + 8 + name_len, 0,
               rec_len - 8 - name_len);
    }
}

static bool init_dir_block(ext2_t *fs, uint32_t block, uint32_t self,
                           uint32_t parent)
{
    memset(io_block, 0, sizeof(io_block));
    dirent_write(fs, io_block, 0, self, 12, ".", 2);
    dirent_write(fs, io_block, 12, parent, EXT2_BLOCK_BYTES - 12, "..", 2);
    ++fs->dir_generation;
    return write_meta(fs, block, io_block);
}

/* A directory may use direct, single-indirect and double-indirect blocks, like
 * a regular file. */
static unsigned dir_blocks(const ext2_inode_t *dir)
{
    size_t n = ((size_t)dir->size + EXT2_BLOCK_BYTES - 1) / EXT2_BLOCK_BYTES;
    if (n == 0) n = 1;
    if (n > EXT2_MAX_DIR_BLOCKS) n = EXT2_MAX_DIR_BLOCKS;
    return (unsigned)n;
}

/* Physical block for directory logical block `index`. */
static ext2_status_t dir_block(ext2_t *fs, const ext2_inode_t *dir,
                               unsigned index, uint32_t *out_block)
{
    if (index >= EXT2_MAX_DIR_BLOCKS) return EXT2_NO_SPACE;
    /* bmap_get's data-block validator deliberately excludes the root directory
     * block (first_data_block is just after it); a directory's own block 0 is
     * valid, so handle the direct range here. */
    if (index < EXT2_DIRECT_BLOCKS) {
        const uint32_t block = dir->block[index];
        if (!dir_block_ok(fs, block)) return EXT2_IO_ERROR;
        *out_block = block;
        return EXT2_OK;
    }
    uint32_t block = 0;
    const ext2_status_t status = bmap_get(fs, dir, index, &block);
    if (status != EXT2_OK) return status;
    if (!dir_block_ok(fs, block)) return EXT2_IO_ERROR;
    *out_block = block;
    return EXT2_OK;
}

/* Load block `index` of a directory into io_block. */
static ext2_status_t dir_load(ext2_t *fs, const ext2_inode_t *dir, unsigned index)
{
    uint32_t block = 0;
    const ext2_status_t status = dir_block(fs, dir, index, &block);
    if (status != EXT2_OK) return status;
    return read_block(fs, block, io_block) ? EXT2_OK : EXT2_IO_ERROR;
}

/* A path resolution looks the same names up several times (stat, permission
 * and open on one path, for example).  This small direct-mapped cache keeps
 * recent directory lookups; the directory generation in the key makes every
 * add, remove or new directory invalidate them together. */
enum {
    DIR_LOOKUP_SLOTS = 8,
    DIR_LOOKUP_NAME = 32,
};

typedef struct {
    ext2_t *fs;
    uint32_t generation;
    uint32_t dir_inode;
    uint32_t inode;
    uint8_t type;
    uint8_t name_len;
    bool valid;
    char name[DIR_LOOKUP_NAME];
} dir_lookup_entry_t;

static dir_lookup_entry_t dir_lookup[DIR_LOOKUP_SLOTS];

/* Sequential lookups (ls -l, find, a recursive copy) ask for the names in
 * directory order.  Remembering where the last one was found turns that from
 * a from-the-start scan per name (O(n^2) over a directory) into a forward
 * walk.  A random lookup starts at the hint and wraps once, so it is still
 * correct; the generation and inode keys discard it after any change. */
typedef struct {
    ext2_t *fs;
    uint32_t dir_inode;
    uint32_t generation;
    uint32_t block;
    size_t offset;
} dir_find_hint_t;

static dir_find_hint_t dir_find_hint;

/* Where the last directory insertion found space. This is deliberately not
 * keyed by dir_generation: adding a file changes the generation, but the
 * position remains a safe scan start and makes appending N files linear. */
typedef struct {
    bool valid;
    ext2_t *fs;
    uint32_t dir_inode;
    uint32_t block;
    size_t offset;
} dir_add_hint_t;

static dir_add_hint_t dir_add_hint;

static void dir_add_hint_invalidate_inode(ext2_t *fs, uint32_t inode)
{
    if (dir_add_hint.valid && dir_add_hint.fs == fs &&
        dir_add_hint.dir_inode == inode) {
        dir_add_hint.valid = false;
    }
}

static void dir_lookup_reset(void)
{
    memset(dir_lookup, 0, sizeof(dir_lookup));
    memset(&dir_find_hint, 0, sizeof(dir_find_hint));
    memset(&dir_add_hint, 0, sizeof(dir_add_hint));
}

static unsigned dir_lookup_slot(uint32_t dir_inode, const char *name)
{
    uint32_t hash = UINT32_C(2166136261) ^ dir_inode;
    for (const char *p = name; *p != '\0'; ++p) {
        hash ^= (uint8_t)*p;
        hash *= UINT32_C(16777619);
    }
    return (unsigned)(hash & (DIR_LOOKUP_SLOTS - 1u));
}

static ext2_status_t dir_find(ext2_t *fs, uint32_t dir_inode,
                              const char *name, uint32_t *out_inode,
                              uint8_t *out_type)
{
    const ext2_inode_t *const dir = const_inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir)) return EXT2_NOT_DIR;
    const bool indexed = (dir->flags & EXT2_INDEX_FL) != 0;
    if (indexed && strcmp(name, "..") == 0) {
        /* An indexed directory keeps "." and ".." in its root block; the rest
         * of that block is the htree root, but the leaf blocks are ordinary
         * dirent chains. */
        const ext2_status_t loaded = dir_load(fs, dir, 0);
        if (loaded != EXT2_OK) return loaded;
        const uint8_t *const entry = io_block + 12;
        if (!dirent_fits(fs, entry, get_u16(entry + 4), 12) ||
            dirent_name_len(fs, entry) != 2 || entry[8] != '.' || entry[9] != '.') {
            return EXT2_IO_ERROR;
        }
        const uint32_t parent = get_u32(entry);
        if (parent == 0 || parent > fs->inode_count) return EXT2_IO_ERROR;
        if (out_inode != NULL) *out_inode = parent;
        if (out_type != NULL) *out_type = dirent_type(fs, entry);
        return EXT2_OK;
    }
    const size_t name_len = strlen(name);
    dir_lookup_entry_t *cache = NULL;
    if (name_len < DIR_LOOKUP_NAME) {
        cache = &dir_lookup[dir_lookup_slot(dir_inode, name)];
        if (cache->valid && cache->fs == fs &&
            cache->dir_inode == dir_inode &&
            cache->generation == fs->dir_generation &&
            cache->name_len == name_len &&
            memcmp(cache->name, name, name_len) == 0) {
            if (out_inode != NULL) *out_inode = cache->inode;
            if (out_type != NULL) *out_type = cache->type;
            return EXT2_OK;
        }
    }
    const uint32_t total = dir_blocks(dir);
    const uint32_t first_block = indexed ? 1 : 0;
    uint32_t start_block = first_block;
    size_t start_offset = 0;
    if (!indexed && dir_find_hint.fs == fs &&
        dir_find_hint.dir_inode == dir_inode &&
        dir_find_hint.generation == fs->dir_generation &&
        dir_find_hint.block < total) {
        start_block = dir_find_hint.block;
        start_offset = dir_find_hint.offset;
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (uint32_t bi = start_block; bi < total; ++bi) {
            const ext2_status_t loaded = dir_load(fs, dir, bi);
            if (loaded != EXT2_OK) return bi == 0 ? EXT2_NOT_DIR : loaded;
            size_t offset = bi == start_block ? start_offset : 0;
            while (offset < EXT2_BLOCK_BYTES) {
                const uint8_t *const entry = io_block + offset;
                const uint16_t rec_len = get_u16(entry + 4);
                if (!dirent_fits(fs, entry, rec_len, offset)) {
                    return EXT2_IO_ERROR;
                }
                const uint32_t inode = get_u32(entry);
                if (inode != 0 && dirent_name_len(fs, entry) == name_len &&
                    memcmp(entry + 8, name, name_len) == 0) {
                    /* A name that points outside the inode table is damage,
                     * not "a different inode": never truncate it into a valid
                     * one. */
                    if (inode > fs->inode_count) return EXT2_IO_ERROR;
                    if (cache != NULL) {
                        cache->fs = fs;
                        cache->generation = fs->dir_generation;
                        cache->dir_inode = dir_inode;
                        cache->inode = (uint32_t)inode;
                        cache->type = dirent_type(fs, entry);
                        cache->name_len = (uint8_t)name_len;
                        memcpy(cache->name, name, name_len);
                        cache->valid = true;
                    }
                    if (!indexed) {
                        dir_find_hint.fs = fs;
                        dir_find_hint.dir_inode = dir_inode;
                        dir_find_hint.generation = fs->dir_generation;
                        dir_find_hint.block = bi;
                        dir_find_hint.offset = offset;
                    }
                    if (out_inode != NULL) *out_inode = (uint32_t)inode;
                    if (out_type != NULL) *out_type = dirent_type(fs, entry);
                    return EXT2_OK;
                }
                offset += rec_len;
            }
        }
        if (start_block == first_block && start_offset == 0) break;
        start_block = first_block;
        start_offset = 0;
    }
    return EXT2_NOT_FOUND;
}

/* We do not implement htree insertion/removal. Once a directory needs to be
 * mutated, clear INDEX_FL and let the existing linear-directory code operate
 * on the leaf blocks. The htree index blocks read as empty dirents, so Linux
 * and e2fsck can still walk the directory as a normal linear directory. */
static ext2_status_t dir_clear_index(ext2_t *fs, uint32_t dir_inode)
{
    ext2_inode_t *const dir = inode_ptr(fs, dir_inode);
    if ((dir->flags & EXT2_INDEX_FL) == 0) return EXT2_OK;
    dir->flags &= ~EXT2_INDEX_FL;
    touch_modified(dir);
    return EXT2_OK;
}

static ext2_status_t dir_add_entry(ext2_t *fs, uint32_t dir_inode,
                             const char *name, uint32_t child_inode,
                             uint8_t type)
{
    const ext2_inode_t *dir = const_inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir)) return EXT2_NOT_DIR;
    if ((dir->flags & EXT2_INDEX_FL) != 0) {
        const ext2_status_t cleared = dir_clear_index(fs, dir_inode);
        if (cleared != EXT2_OK) return cleared;
        dir = const_inode_ptr(fs, dir_inode);
    }
    const size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return EXT2_NAME_TOO_LONG;
    const size_t needed = (8 + name_len + 3u) & ~(size_t)3u;

    const unsigned count = dir_blocks(dir);
    unsigned start_block = 0;
    size_t start_offset = 0;
    if (dir_add_hint.valid && dir_add_hint.fs == fs &&
        dir_add_hint.dir_inode == dir_inode && dir_add_hint.block < count) {
        start_block = dir_add_hint.block;
        start_offset = dir_add_hint.offset;
    }
    /* Try the last insertion point first, then wrap once. This keeps appending
     * to a populated media directory from rescanning it from block zero for
     * every new file. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned bi = start_block; bi < count; ++bi) {
            uint32_t physical = 0;
            const ext2_status_t loaded = dir_block(fs, dir, bi, &physical);
            if (loaded != EXT2_OK) return bi == 0 ? EXT2_NOT_DIR : loaded;
            if (!read_block(fs, physical, io_block)) return EXT2_IO_ERROR;
            size_t offset = bi == start_block ? start_offset : 0;
            while (offset < EXT2_BLOCK_BYTES) {
                uint8_t *const entry = io_block + offset;
                const uint16_t rec_len = get_u16(entry + 4);
                if (!dirent_fits(fs, entry, rec_len, offset)) {
                    return EXT2_IO_ERROR;
                }
                const uint32_t inode = get_u32(entry);
                if (inode == 0) {
                    if (rec_len >= needed) {
                        dirent_write(fs, entry, 0, child_inode, rec_len, name, type);
                        if (!write_meta(fs, physical, io_block)) return EXT2_IO_ERROR;
                        dir_add_hint = (dir_add_hint_t){true, fs, dir_inode, bi, offset};
                        return EXT2_OK;
                    }
                } else {
                    const size_t used = dirent_used(fs, entry);
                    if (used <= rec_len && rec_len - used >= needed) {
                        put_u16(entry + 4, (uint16_t)used);
                        dirent_write(fs, entry + used, 0, child_inode,
                                     (uint16_t)(rec_len - used), name, type);
                        if (!write_meta(fs, physical, io_block)) return EXT2_IO_ERROR;
                        dir_add_hint = (dir_add_hint_t){true, fs, dir_inode, bi, offset};
                        return EXT2_OK;
                    }
                }
                offset += rec_len;
            }
        }
        if (start_block == 0 && start_offset == 0) break;
        start_block = 0;
        start_offset = 0;
    }
    /* Every block is full: give the directory another one, creating the
     * indirect blocks on the way. */
    if (count >= EXT2_MAX_DIR_BLOCKS) return EXT2_NO_SPACE;
    const uint32_t fresh = alloc_block(fs);
    if (fresh == UINT32_MAX) return EXT2_NO_SPACE;
    memset(io_block, 0, sizeof(io_block));
    dirent_write(fs, io_block, 0, child_inode, EXT2_BLOCK_BYTES, name, type);
    if (!write_meta(fs, fresh, io_block)) {
        (void)free_block(fs, fresh);
        return EXT2_IO_ERROR;
    }
    ext2_inode_t *const grown = inode_ptr(fs, dir_inode);
    const ext2_status_t mapped = bmap_set(fs, grown, count, fresh);
    if (mapped != EXT2_OK) {
        (void)free_block(fs, fresh);
        return mapped;
    }
    grown->size = (count + 1) * EXT2_BLOCK_BYTES;
    grown->blocks += EXT2_BLOCK_BYTES / 512u;
    dir_add_hint = (dir_add_hint_t){true, fs, dir_inode, count, 0};
    return EXT2_OK;
}

static ext2_status_t dir_remove_entry(ext2_t *fs, uint32_t dir_inode,
                                const char *name)
{
    const ext2_inode_t *dir = const_inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir)) return EXT2_NOT_DIR;
    if ((dir->flags & EXT2_INDEX_FL) != 0) {
        const ext2_status_t cleared = dir_clear_index(fs, dir_inode);
        if (cleared != EXT2_OK) return cleared;
        dir = const_inode_ptr(fs, dir_inode);
    }
    const size_t name_len = strlen(name);
    const unsigned count = dir_blocks(dir);
    unsigned start_block = 0;
    size_t start_offset = 0;
    if (dir_find_hint.fs == fs && dir_find_hint.dir_inode == dir_inode &&
        dir_find_hint.generation == fs->dir_generation &&
        dir_find_hint.block < count) {
        start_block = dir_find_hint.block;
        start_offset = dir_find_hint.offset;
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned bi = start_block; bi < count; ++bi) {
            uint32_t physical = 0;
            const ext2_status_t loaded = dir_block(fs, dir, bi, &physical);
            if (loaded != EXT2_OK) return bi == 0 ? EXT2_NOT_DIR : loaded;
            if (!read_block(fs, physical, io_block)) return EXT2_IO_ERROR;
            size_t offset = bi == start_block ? start_offset : 0;
            while (offset < EXT2_BLOCK_BYTES) {
                uint8_t *const entry = io_block + offset;
                const uint16_t rec_len = get_u16(entry + 4);
                if (!dirent_fits(fs, entry, rec_len, offset)) {
                    return EXT2_IO_ERROR;
                }
                if (get_u32(entry) != 0 && dirent_name_len(fs, entry) == name_len &&
                    memcmp(entry + 8, name, name_len) == 0) {
                    put_u32(entry, 0);
                    entry[6] = 0;
                    entry[7] = 0;
                    if (!write_meta(fs, physical, io_block)) return EXT2_IO_ERROR;
                    dir_add_hint = (dir_add_hint_t){true, fs, dir_inode, bi, offset};
                    return EXT2_OK;
                }
                offset += rec_len;
            }
        }
        if (start_block == 0 && start_offset == 0) break;
        start_block = 0;
        start_offset = 0;
    }
    return EXT2_NOT_FOUND;
}

/* Adding or removing an entry changes the directory's contents. */
static ext2_status_t dir_add(ext2_t *fs, uint32_t dir_inode, const char *name,
                             uint32_t child_inode, uint8_t type)
{
    const ext2_status_t status =
        dir_add_entry(fs, dir_inode, name, child_inode, type);
    if (status == EXT2_OK) {
        ++fs->dir_generation;
        touch_modified(inode_ptr(fs, dir_inode));
    }
    return status;
}

static ext2_status_t dir_remove(ext2_t *fs, uint32_t dir_inode,
                                const char *name)
{
    const ext2_status_t status = dir_remove_entry(fs, dir_inode, name);
    if (status == EXT2_OK) {
        ++fs->dir_generation;
        touch_modified(inode_ptr(fs, dir_inode));
    }
    return status;
}

static bool dir_empty(ext2_t *fs, uint32_t dir_inode)
{
    const ext2_inode_t *dir = const_inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir)) return false;
    if ((dir->flags & EXT2_INDEX_FL) != 0) {
        if (dir_clear_index(fs, dir_inode) != EXT2_OK) return false;
        dir = const_inode_ptr(fs, dir_inode);
    }
    for (unsigned bi = 0; bi < dir_blocks(dir); ++bi) {
        if (dir_load(fs, dir, bi) != EXT2_OK) return false;
        size_t offset = 0;
        while (offset < EXT2_BLOCK_BYTES) {
            const uint8_t *const entry = io_block + offset;
            const uint16_t rec_len = get_u16(entry + 4);
            if (!dirent_fits(fs, entry, rec_len, offset)) {
                return false;
            }
            if (get_u32(entry) != 0) {
                const size_t name_len = dirent_name_len(fs, entry);
                if (!(name_len == 1 && entry[8] == '.') &&
                    !(name_len == 2 && entry[8] == '.' && entry[9] == '.')) {
                    return false;
                }
            }
            offset += rec_len;
        }
    }
    return true;
}

static ext2_status_t resolve_parent(ext2_t *fs, const char *path,
                                    uint32_t *out_parent,
                                    const char **out_name)
{
    if (!fs_ready(fs) || path == NULL || path[0] != '/') {
        return EXT2_INVALID;
    }
    if (strcmp(path, "/") == 0) return EXT2_INVALID;
    const char *const slash = strrchr(path, '/');
    if (slash == NULL) return EXT2_INVALID;
    *out_name = slash + 1;
    if (**out_name == '\0') return EXT2_INVALID;
    char parent[EXT2_PATH_BYTES];
    const size_t length = (size_t)(slash - path);
    if (length == 0) {
        *out_parent = EXT2_ROOT_INO;
        return EXT2_OK;
    }
    if (length >= sizeof(parent)) return EXT2_NAME_TOO_LONG;
    memcpy(parent, path, length);
    parent[length] = '\0';
    return ext2_lookup(fs, parent, out_parent);
}

static ext2_status_t create_inode_at(ext2_t *fs, uint32_t parent,
                                     const char *name, uint16_t mode)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || name == NULL || name[0] == '\0' ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        strlen(name) > 255) {
        return EXT2_INVALID;
    }
    if (!inode_number_valid(fs, parent) ||
        const_inode_ptr(fs, parent)->mode == 0) {
        return EXT2_NOT_FOUND;
    }
    if (!inode_is_dir(const_inode_ptr(fs, parent))) return EXT2_NOT_DIR;
    if (dir_find(fs, parent, name, NULL, NULL) == EXT2_OK) {
        return EXT2_EXISTS;
    }

    const uint32_t index = alloc_inode(fs);
    if (index == 0) return EXT2_NO_SPACE;
    ext2_inode_t *const inode = inode_ptr(fs, index);
    init_inode(fs, inode, mode);
    inode->uid = fs->create_uid;
    inode->gid = fs->create_gid;
    inode->mode = (uint16_t)((inode->mode & EXT2_S_IFMT) |
                             ((inode->mode & 07777u) &
                              (uint16_t)~fs->create_umask));
    const bool is_dir = EXT2_S_ISDIR(mode);
    if (is_dir) {
        const uint32_t block = alloc_block(fs);
        if (block == UINT32_MAX) {
            free_inode(fs, index);
            return EXT2_NO_SPACE;
        }
        inode->block[0] = block;
        inode->size = EXT2_BLOCK_BYTES;
        inode->blocks = EXT2_BLOCK_BYTES / 512u;
        inode->links_count = 2;
        if (!init_dir_block(fs, block, index, parent)) {
            (void)free_block(fs, block);
            free_inode(fs, index);
            return EXT2_IO_ERROR;
        }
    }
    ext2_status_t status = dir_add(fs, parent, name, index,
                                   is_dir ? 2 : 1);
    if (status != EXT2_OK) {
        if (is_dir) {
            (void)free_block(fs, inode->block[0]);
        }
        free_inode(fs, index);
        return status;
    }
    if (is_dir) {
        ext2_inode_t *const parent_inode = inode_ptr(fs, parent);
        ++parent_inode->links_count;
        count_dir(fs, index, +1);
    }
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

static ext2_status_t create_inode(ext2_t *fs, const char *path, uint16_t mode)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    uint32_t parent;
    const char *name;
    const ext2_status_t status = resolve_parent(fs, path, &parent, &name);
    if (status != EXT2_OK) return status;
    return create_inode_at(fs, parent, name, mode);
}

/* Free an indirect block of the given depth (1 = entries are data blocks) and
 * everything below it.  The shared indirect_block buffer is re-read after each
 * recursion so no per-level buffers are needed on the task stack. */
static bool free_tree(ext2_t *fs, uint32_t block, unsigned depth)
{
    if (block < fs->first_data_block || block >= fs->block_count) return false;
    for (unsigned i = 0; i < EXT2_INDIRECT_ENTRIES; ++i) {
        if (!read_block(fs, block, indirect_block)) return false;
        const uint32_t child = get_u32(indirect_block + i * 4);
        if (child < fs->first_data_block) continue;
        if (depth > 1) {
            if (!free_tree(fs, child, depth - 1)) return false;
        } else if (!free_block(fs, child)) {
            return false;
        }
    }
    return free_block(fs, block);
}

static bool free_inode_blocks(ext2_t *fs, ext2_inode_t *inode)
{
    /* Fast symlinks store their target in the block pointer array, not in
     * data blocks.  Treating those bytes as block numbers would corrupt the
     * allocation bitmap. */
    if (inode_is_symlink(inode) &&
        inode->size <= sizeof(inode->block)) return true;
    for (unsigned i = 0; i < EXT2_DIRECT_BLOCKS; ++i) {
        if (inode->block[i] >= fs->first_data_block) {
            if (!free_block(fs, inode->block[i])) return false;
            inode->block[i] = 0;
        }
    }
    for (unsigned level = 1; level <= 3; ++level) {
        uint32_t *const root = &inode->block[EXT2_DIRECT_BLOCKS + level - 1];
        if (*root >= fs->first_data_block) {
            if (!free_tree(fs, *root, level)) return false;
        }
        *root = 0;
    }
    inode->size = 0;
    inode->blocks = 0;
    return true;
}

/* Groups that keep a copy of the superblock and descriptor table (the
 * sparse_super rule): 0, 1 and the powers of 3, 5 and 7. */
static bool group_has_backup(uint32_t group)
{
    if (group <= 1) return true;
    for (uint32_t base = 3; base <= 7; base += 2) {
        uint32_t power = base;
        while (power < group) power *= base;
        if (power == group) return true;
    }
    return false;
}

/* The size of a volume's inode tables: one inode per 32 KiB for large
 * volumes, 256 inodes for the small flash and RAM volumes (as before). */
static uint32_t choose_inodes_per_group(uint32_t blocks, uint32_t groups)
{
    if (groups == 1 && blocks <= 2048) return 256;
    uint32_t per_group = 1024;
    const uint32_t cap =
        (EXT2_FORMAT_INODES_TOTAL / groups) & ~(EXT2_INODES_PER_BLOCK - 1u);
    if (per_group > cap) per_group = cap;
    return per_group;
}

/* The journal: inode 8, a file of `length` blocks (contiguous: the volume is
 * new), with a JBD2 version-2 superblock. */
static bool create_journal(ext2_t *fs)
{
    uint32_t length = fs->block_count / 16;
    if (length < 1024) length = 1024;
    if (length > 2048) length = 2048;
    if (!journal_alloc(fs, length)) return false;
    /* The format writes straight to the medium; the journal is for what comes
     * after (ext2_format clears this when it is done). */
    fs->journal->recovering = true;
    ext2_inode_t *const inode = inode_ptr(fs, EXT2_JOURNAL_INO);
    init_inode(fs, inode, 0x8180); /* 0100600 */
    inode->links_count = 1;
    for (uint32_t i = 0; i < length; ++i) {
        const uint32_t block = alloc_block(fs);
        if (block == UINT32_MAX || bmap_set(fs, inode, i, block) != EXT2_OK) {
            return false;
        }
        fs->journal->map[i] = block;
        inode->blocks += EXT2_BLOCK_BYTES / 512u;
    }
    inode->size = length * EXT2_BLOCK_BYTES;
    uint8_t *const block = fs->journal->scratch;
    memset(block, 0, EXT2_BLOCK_BYTES);
    put_be32(block, JBD2_MAGIC);
    put_be32(block + 4, JBD2_SUPERBLOCK_V2);
    put_be32(block + 8, 0);
    put_be32(block + 12, EXT2_BLOCK_BYTES);
    put_be32(block + 16, length);
    put_be32(block + 20, 1);  /* first log block */
    put_be32(block + 24, 1);  /* the next transaction id */
    put_be32(block + 28, 0);  /* empty */
    put_be32(block + 64, 1);  /* users */
    if (!write_block(fs, fs->journal->map[0], block)) return false;
    fs->super.feature_compat |= EXT2_FEATURE_COMPAT_HAS_JOURNAL;
    fs->super.journal_inum = EXT2_JOURNAL_INO;
    fs->super.jnl_backup_type = 1; /* the backup of the inode's blocks follows */
    for (unsigned i = 0; i < 15; ++i) fs->super.journal_blocks[i] = inode->block[i];
    fs->super.journal_blocks[15] = 0;
    fs->super.journal_blocks[16] = inode->size;
    return true;
}

ext2_status_t ext2_format(ext2_t *fs, ext2_blockdev_t *device)
{
    if (fs == NULL || !ext2_blockdev_valid(device) ||
        device->block_size != EXT2_BLOCK_BYTES || device->block_count < 16) {
        return EXT2_INVALID;
    }
    uint32_t blocks = device->block_count;
    /* The formatter is a host-fixture utility. Keep its inode budget bounded
     * separately from the mount cap: firmware never calls ext2_format(). */
    const uint32_t format_max_groups = EXT2_FORMAT_INODES_TOTAL / 1024u;
    if (blocks > format_max_groups * EXT2_BLOCKS_PER_GROUP) {
        blocks = format_max_groups * EXT2_BLOCKS_PER_GROUP;
    }
    uint32_t groups = (blocks + EXT2_BLOCKS_PER_GROUP - 1) / EXT2_BLOCKS_PER_GROUP;
    uint32_t ipg = choose_inodes_per_group(blocks, groups);
    uint32_t itb = ipg / EXT2_INODES_PER_BLOCK;
    const uint32_t gdt = (groups * (uint32_t)sizeof(ext2_group_desc_t) +
                          EXT2_BLOCK_BYTES - 1) / EXT2_BLOCK_BYTES;
    /* A last group too small to hold its own metadata and some data is left
     * out of the volume. */
    if (groups > 1) {
        const uint32_t last_blocks = blocks - (groups - 1) * EXT2_BLOCKS_PER_GROUP;
        const uint32_t overhead = (group_has_backup(groups - 1) ? 1 + gdt : 0) + 2 + itb;
        if (last_blocks < overhead + 8) {
            blocks = (groups - 1) * EXT2_BLOCKS_PER_GROUP;
            --groups;
            ipg = choose_inodes_per_group(blocks, groups);
            itb = ipg / EXT2_INODES_PER_BLOCK;
        }
    }
    if (blocks < 1 + gdt + 2 + itb + 4) return EXT2_INVALID;
    if (!ext2_blockdev_erase(device, 0, blocks)) {
        return EXT2_IO_ERROR;
    }
    /* Invalidate the old superblock before building the new one.  sd_erase is
     * a no-op, so without this an interrupted format would leave the previous
     * superblock pointing at half-written metadata: the volume would mount and
     * be corrupt.  An invalid magic makes it "not formatted" instead. */
    memset(io_block, 0, EXT2_BLOCK_BYTES);
    if (!ext2_blockdev_write(device, 0, io_block, 1) ||
        !ext2_blockdev_flush(device)) {
        return EXT2_IO_ERROR;
    }
    memset(fs, 0, sizeof(*fs));
    fs->device = device;
    fs->block_count = blocks;
    fs->group_count = groups;
    fs->blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    fs->inodes_per_group = ipg;
    fs->inode_size = EXT2_INODE_BYTES;
    fs->inodes_per_block = EXT2_INODES_PER_BLOCK;
    fs->first_ino = EXT2_FIRST_INO;
    fs->has_filetype = true;
    fs->inode_count = groups * ipg;
    fs->itable_blocks_per_group = itb;
    fs->itable_blocks = groups * itb;
    if (!metadata_attach(fs)) return EXT2_NO_SPACE;

    fs->free_blocks = 0;
    fs->free_inodes = fs->inode_count - (fs->first_ino - 1);
    fs->dir_count = 0;

    fs->super.inodes_count = fs->inode_count;
    fs->super.blocks_count = blocks;
    fs->super.first_data_block = 0;
    fs->super.log_block_size = 2; /* 1024 << 2 = 4096 */
    fs->super.log_frag_size = 2;
    fs->super.blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    fs->super.frags_per_group = EXT2_BLOCKS_PER_GROUP;
    fs->super.inodes_per_group = ipg;
    fs->super.mtime = now_seconds();
    fs->super.wtime = now_seconds();
    fs->super.max_mnt_count = 0xffff;
    fs->super.magic = EXT2_MAGIC;
    fs->super.state = EXT2_VALID_FS;
    fs->super.errors = 1;
    fs->super.lastcheck = now_seconds();
    fs->super.creator_os = 0;
    fs->super.rev_level = EXT2_VERSION;
    fs->super.first_ino = fs->first_ino;
    fs->super.inode_size = EXT2_INODE_BYTES;
    fs->super.feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
    if (groups > 1) fs->super.feature_ro_compat = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
    for (unsigned i = 0; i < 16; ++i) {
        fs->super.uuid[i] = (uint8_t)(0x40u + i * 3u + (groups > 1 ? 1u : 0u));
    }
    memcpy(fs->super.volume_name, "DAVF", 4);
    memcpy(fs->super.last_mounted, "/", 1);
    fs->super.mkfs_time = now_seconds();

    /* Lay out every group: [superblock+descriptor backup] block bitmap,
     * inode bitmap, inode table, then data. */
    for (uint32_t g = 0; g < groups; ++g) {
        const uint32_t start = g * EXT2_BLOCKS_PER_GROUP;
        const uint32_t in_group = blocks - start < EXT2_BLOCKS_PER_GROUP
                                      ? blocks - start : EXT2_BLOCKS_PER_GROUP;
        uint32_t position = start;
        if (group_has_backup(g)) position += 1 + gdt;
        fs->groups[g].block_bitmap = position++;
        fs->groups[g].inode_bitmap = position++;
        fs->groups[g].inode_table = position;
        position += itb;
        ext2_bitmap_cache_entry_t *const cached = bitmap_cache_get_(fs, g);
        if (cached == NULL) {
            metadata_detach(fs);
            return EXT2_IO_ERROR;
        }
        memset(cached->block_bitmap, 0xFF, EXT2_BITMAP_BYTES);
        for (uint32_t b = position - start; b < in_group; ++b) {
            bitmap_clear(cached->block_bitmap, b);
        }
        memset(cached->inode_bitmap, 0xFF, EXT2_BITMAP_BYTES);
        for (uint32_t i = (g == 0 ? fs->first_ino - 1 : 0); i < ipg; ++i) {
            bitmap_clear(cached->inode_bitmap, i);
        }
        fs->groups[g].free_blocks_count = (uint16_t)(in_group - (position - start));
        fs->groups[g].free_inodes_count = (uint16_t)(ipg - (g == 0 ? fs->first_ino - 1 : 0));
        fs->groups[g].used_dirs_count = 0;
        fs->free_blocks += fs->groups[g].free_blocks_count;
        cached->dirty_block = 1;
        cached->dirty_inode = 1;
    }

    fs->mounted = true;
    fs->alloc_hint = 0;
    fs->inode_hint = fs->first_ino;
    fs->dir_generation = 0;
    dir_scan_hint.valid = false;
    dir_lookup_reset();
    const uint32_t root_block = alloc_block(fs);
    if (root_block == UINT32_MAX) {
        fs->mounted = false;
        metadata_detach(fs);
        return EXT2_NO_SPACE;
    }
    fs->root_dir_block = root_block;
    fs->first_data_block = 1; /* block 0 holds the superblock on 4 KiB ext2 */
    ext2_inode_t *const root = inode_ptr(fs, EXT2_ROOT_INO);
    init_inode(fs, root, EXT2_MODE_DIR);
    root->links_count = 2;
    root->size = EXT2_BLOCK_BYTES;
    root->blocks = EXT2_BLOCK_BYTES / 512u;
    root->block[0] = root_block;
    count_dir(fs, EXT2_ROOT_INO, +1);

    fs->super.block_group_nr = 0;
    bool ok = init_dir_block(fs, root_block, EXT2_ROOT_INO, EXT2_ROOT_INO);
    if (ok && blocks >= EXT2_JOURNAL_MIN_VOLUME_BLOCKS) ok = create_journal(fs);
    ok = ok && sync_metadata(fs) && write_summary(fs);
    if (fs->journal != NULL) fs->journal->recovering = false;
    /* Backup copies of the superblock and descriptor table. */
    for (uint32_t g = 1; ok && g < groups; ++g) {
        if (!group_has_backup(g)) continue;
        const uint32_t start = g * EXT2_BLOCKS_PER_GROUP;
        ext2_super_t backup = fs->super;
        backup.block_group_nr = (uint16_t)g;
        memset(io_block, 0, sizeof(io_block));
        memcpy(io_block, &backup, sizeof(backup));
        ok = write_block(fs, start, io_block);
        for (unsigned b = 0; ok && b < gdt; ++b) {
            memset(io_block, 0, sizeof(io_block));
            const size_t first = (size_t)b * (EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t));
            size_t count = groups - first;
            if (count > EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t)) {
                count = EXT2_BLOCK_BYTES / sizeof(ext2_group_desc_t);
            }
            memcpy(io_block, fs->groups + first, count * sizeof(ext2_group_desc_t));
            ok = write_block(fs, start + 1 + b, io_block);
        }
    }
    if (!ok) {
        fs->mounted = false;
        metadata_detach(fs);
        return EXT2_IO_ERROR;
    }
    return EXT2_OK;
}

/* Group descriptors and the root directory. Bitmaps and inode tables are
 * loaded on demand, so mounting does not read the whole volume. */
static ext2_status_t load_metadata(ext2_t *fs)
{
    /* Replaying a journal writes metadata behind the caches' back. */
    cache_invalidate(fs);
    fs->dir_count = 0;
    if (!read_groups(fs)) return EXT2_IO_ERROR;
    fs->free_blocks = fs->super.free_blocks_count;
    fs->free_inodes = fs->super.free_inodes_count;
    const uint32_t first_alloc = EXT2_GDT_BLOCK + gdt_blocks(fs);
    for (uint32_t g = 0; g < fs->group_count; ++g) {
        const ext2_group_desc_t *const d = &fs->groups[g];
        if (d->block_bitmap < first_alloc || d->block_bitmap >= fs->block_count ||
            d->inode_bitmap < first_alloc || d->inode_bitmap >= fs->block_count ||
            d->inode_table < first_alloc ||
            (uint64_t)d->inode_table + fs->itable_blocks_per_group > fs->block_count ||
            d->free_blocks_count > fs->blocks_per_group ||
            d->free_inodes_count > fs->inodes_per_group) {
            return EXT2_INVALID;
        }
        fs->dir_count += d->used_dirs_count;
    }
    if (fs->free_blocks > fs->block_count || fs->free_inodes > fs->inode_count)
        return EXT2_INVALID;
    fs->mounted = true; /* const_inode_ptr loads the root's table block on demand */
    const ext2_inode_t *const root = const_inode_ptr(fs, EXT2_ROOT_INO);
    if (!inode_is_dir(root) || root->block[0] < 2 || root->block[0] >= fs->block_count) {
        fs->mounted = false;
        return EXT2_INVALID;
    }
    fs->root_dir_block = root->block[0];
    fs->first_data_block = 1; /* block 0 holds the superblock on 4 KiB ext2 */
    fs->alloc_hint = EXT2_GDT_BLOCK + gdt_blocks(fs);
    fs->inode_hint = fs->first_ino;
    fs->dir_generation = 0;
    dir_scan_hint.valid = false;
    dir_lookup_reset();
    return EXT2_OK;
}

/* Locate the journal (inode 8), check its superblock, and replay a committed
 * transaction that was not yet applied. A journal in a format we do not write
 * is left alone when empty and refused when it holds work. */
static ext2_status_t journal_open(ext2_t *fs, bool *out_replayed)
{
    *out_replayed = false;
    const ext2_inode_t *const inode = const_inode_ptr(fs, EXT2_JOURNAL_INO);
    const uint32_t length = inode->size / EXT2_BLOCK_BYTES;
    if (!EXT2_S_ISREG(inode->mode) || length < 8 ||
        length > 65536 || inode->size % EXT2_BLOCK_BYTES != 0) {
        return EXT2_INVALID;
    }
    /* A full transaction has to fit in the log: its superblock/start block,
     * the descriptor blocks, up to JBD2_TXN_CAPACITY data blocks and the
     * commit record.  A foreign image with a smaller journal would mount and
     * then fail every write for ever (journal_commit cannot split one). */
    {
        const unsigned per_descriptor =
            (EXT2_BLOCK_BYTES - JBD2_HEADER_BYTES - 16) / JBD2_TAG_BYTES;
        const unsigned needed =
            1 + (JBD2_TXN_CAPACITY + per_descriptor - 1) / per_descriptor +
            JBD2_TXN_CAPACITY + 1;
        if (length < needed) return EXT2_UNSUPPORTED;
    }
    if (!journal_alloc(fs, length)) return EXT2_NO_SPACE;
    ext2_journal_t *const j = fs->journal;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t physical = 0;
        const ext2_status_t status = bmap_get(fs, inode, i, &physical);
        if (status != EXT2_OK || physical == 0) {
            journal_free(fs);
            return EXT2_INVALID;
        }
        j->map[i] = physical;
    }
    uint8_t *const sb = j->scratch;
    if (!ext2_blockdev_read(fs->device, j->map[0], sb, 1)) {
        journal_free(fs);
        return EXT2_IO_ERROR;
    }
    const uint32_t type = get_be32(sb + 4);
    const bool foreign = get_be32(sb) != JBD2_MAGIC ||
                         (type != JBD2_SUPERBLOCK_V2 && type != 3) ||
                         get_be32(sb + 12) != EXT2_BLOCK_BYTES ||
                         get_be32(sb + 20) != 1 ||
                         get_be32(sb + 36) != 0 || get_be32(sb + 40) != 0 ||
                         get_be32(sb + 44) != 0;
    uint32_t start = get_be32(sb + 28);
    uint32_t sequence = get_be32(sb + 24);
    if (foreign) {
        journal_free(fs);
        if (start != 0 || (fs->super.feature_incompat & EXT2_FEATURE_INCOMPAT_RECOVER)) {
            return EXT2_UNSUPPORTED;
        }
        return EXT2_OK; /* empty: used as if there were none */
    }
    j->sequence = sequence != 0 ? sequence : 1;
    if (start == 0) return EXT2_OK;

    /* Walk the log: descriptors and their blocks until the commit block. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint32_t position = start;
        bool committed = false;
        for (;;) {
            if (position >= length) break;
            if (!ext2_blockdev_read(fs->device, j->map[position], sb, 1)) {
                return EXT2_IO_ERROR;
            }
            if (get_be32(sb) != JBD2_MAGIC || get_be32(sb + 8) != sequence) break;
            const uint32_t kind = get_be32(sb + 4);
            if (kind == JBD2_COMMIT) {
                committed = true;
                break;
            }
            if (kind != JBD2_DESCRIPTOR) break;
            /* the tags: where each following block belongs */
            uint8_t *const tags = j->scratch3;
            memcpy(tags, sb, EXT2_BLOCK_BYTES);
            size_t at = JBD2_HEADER_BYTES;
            uint32_t data_at = position + 1;
            bool last = false;
            while (!last && at + JBD2_TAG_BYTES <= EXT2_BLOCK_BYTES) {
                const uint32_t home = get_be32(tags + at);
                const uint16_t flags = (uint16_t)((tags[at + 6] << 8) | tags[at + 7]);
                at += JBD2_TAG_BYTES;
                if (!(flags & JBD2_FLAG_SAME_UUID)) at += 16;
                last = (flags & JBD2_FLAG_LAST_TAG) != 0;
                if (data_at >= length) return EXT2_INVALID;
                if (pass == 1) {
                    if (home >= fs->block_count) return EXT2_INVALID;
                    uint8_t *const block = j->scratch2;
                    if (!ext2_blockdev_read(fs->device, j->map[data_at], block, 1)) {
                        return EXT2_IO_ERROR;
                    }
                    if (flags & JBD2_FLAG_ESCAPE) put_be32(block, JBD2_MAGIC);
                    if (!ext2_blockdev_write(fs->device, home, block, 1)) {
                        return EXT2_IO_ERROR;
                    }
                }
                ++data_at;
            }
            position = data_at;
        }
        if (!committed) break; /* nothing complete: discard the log */
        if (pass == 1) *out_replayed = true;
    }
    if (*out_replayed && !ext2_blockdev_flush(fs->device)) return EXT2_IO_ERROR;
    /* The log is empty, whatever happened. */
    if (!journal_set_state(fs, 0, sequence + 1) ||
        !ext2_blockdev_flush(fs->device)) {
        return EXT2_IO_ERROR;
    }
    j->sequence = sequence + 1;
    /* The log was nonempty, so the superblock must be refreshed and RECOVER
     * cleared even when there was no complete transaction to replay. */
    if (!read_super(fs)) return EXT2_IO_ERROR;
    fs->super.feature_incompat &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
    /* Replaying is idempotent, but a mount that reports success after this
     * write failed leaves RECOVER set and replays again on the next boot. */
    if (!write_super_raw(fs, false) || !ext2_blockdev_flush(fs->device)) {
        return EXT2_IO_ERROR;
    }
    return EXT2_OK;
}

static ext2_status_t mount_fail(ext2_t *fs, ext2_status_t status)
{
    metadata_detach(fs);
    memset(fs, 0, sizeof(*fs));
    return status;
}

ext2_status_t ext2_mount(ext2_t *fs, ext2_blockdev_t *device)
{
    if (fs == NULL || !ext2_blockdev_valid(device) ||
        device->block_size != EXT2_BLOCK_BYTES) {
        return EXT2_INVALID;
    }
    memset(fs, 0, sizeof(*fs));
    fs->device = device;
    if (!read_super(fs)) {
        return mount_fail(fs, EXT2_IO_ERROR);
    }
    if (fs->super.magic != EXT2_MAGIC) {
        return mount_fail(fs, EXT2_NOT_FORMATTED);
    }
    const uint32_t rev = fs->super.rev_level;
    const uint32_t blocks = fs->super.blocks_count;
    const uint32_t ipg = fs->super.inodes_per_group;
    /* Revision 0 has no dynamic fields: 128-byte inodes and inode 11 as the
     * first non-reserved one.  Revision 1 and later carry them in the
     * superblock. */
    const uint32_t inode_size = rev == 0 ? EXT2_INODE_BYTES : fs->super.inode_size;
    const uint32_t first_ino = rev == 0 ? EXT2_FIRST_INO : fs->super.first_ino;
    if (rev > EXT2_VERSION || fs->super.log_block_size != 2 ||
        /* Linux formats 128 or 256 (or larger) byte inodes; the extra bytes
         * carry fields this kernel does not use but must preserve. */
        inode_size < EXT2_INODE_BYTES || inode_size > EXT2_BLOCK_BYTES ||
        (inode_size & (inode_size - 1)) != 0 ||
        inode_size % EXT2_INODE_BYTES != 0 ||
        first_ino < EXT2_FIRST_INO ||
        blocks < 16 || blocks > device->block_count ||
        fs->super.first_data_block != 0 ||
        fs->super.blocks_per_group == 0 ||
        fs->super.blocks_per_group > EXT2_BITMAP_BYTES * 8 ||
        fs->super.frags_per_group != fs->super.blocks_per_group ||
        ipg == 0 || ipg > EXT2_BITMAP_BYTES * 8) {
        return mount_fail(fs, EXT2_INVALID);
    }
    const uint32_t bpg = fs->super.blocks_per_group;
    const uint32_t groups = (blocks + bpg - 1) / bpg;
    if (groups > EXT2_MAX_GROUPS) return mount_fail(fs, EXT2_INVALID);
    const uint32_t inodes_per_block = EXT2_BLOCK_BYTES / inode_size;
    if (fs->super.inodes_count > (uint64_t)groups * ipg ||
        first_ino > fs->super.inodes_count ||
        ipg % inodes_per_block != 0) {
        return mount_fail(fs, EXT2_INVALID);
    }
    /* A Linux mke2fs volume carries ext_attr, resize_inode, dir_index and
     * large_file by default.  The first three only describe structures this
     * kernel leaves alone; large_file changes the size field above 2 GiB and
     * is accepted with that caveat.  Anything else (extents, 64bit, flex_bg,
     * metadata_csum, ...) changes the on-disk layout and is refused. */
    if ((fs->super.feature_incompat &
         ~(EXT2_FEATURE_INCOMPAT_FILETYPE | EXT2_FEATURE_INCOMPAT_RECOVER)) != 0 ||
        (fs->super.feature_ro_compat &
         ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
           EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) != 0 ||
        (fs->super.feature_compat &
         ~(EXT2_FEATURE_COMPAT_HAS_JOURNAL | EXT2_FEATURE_COMPAT_EXT_ATTR |
           EXT2_FEATURE_COMPAT_RESIZE_INODE |
           EXT2_FEATURE_COMPAT_DIR_INDEX)) != 0) {
        return mount_fail(fs, EXT2_UNSUPPORTED);
    }
    fs->block_count = blocks;
    fs->group_count = groups;
    fs->blocks_per_group = bpg;
    fs->inodes_per_group = ipg;
    fs->inode_size = inode_size;
    fs->inodes_per_block = EXT2_BLOCK_BYTES / inode_size;
    fs->inode_count = fs->super.inodes_count;
    fs->first_ino = first_ino;
    fs->has_filetype = rev != 0 &&
        (fs->super.feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    fs->itable_blocks_per_group =
        (ipg + fs->inodes_per_block - 1) / fs->inodes_per_block;
    fs->itable_blocks = groups * fs->itable_blocks_per_group;
    fs->free_blocks = fs->super.free_blocks_count;
    fs->free_inodes = fs->super.free_inodes_count;
    if (!metadata_attach(fs)) {
        return mount_fail(fs, EXT2_NO_SPACE);
    }
    ext2_status_t status = load_metadata(fs);
    if (status != EXT2_OK) return mount_fail(fs, status);
    /* A journal: find its blocks, and replay it if the volume was left in the
     * middle of a commit. */
    if ((fs->super.feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) != 0) {
        bool replayed = false;
        status = journal_open(fs, &replayed);
        if (status != EXT2_OK) return mount_fail(fs, status);
        if (replayed) {
            /* The blocks on the medium changed under us: read it all again. */
            fs->journal->recovering = true;
            if (!read_super(fs)) return mount_fail(fs, EXT2_IO_ERROR);
            status = load_metadata(fs);
            fs->journal->recovering = false;
            if (status != EXT2_OK) return mount_fail(fs, status);
        }
    }
    /* Mark the volume in use; a clean unmount clears it again.  ext2 uses
     * EXT2_ERROR_FS (2), not 0, while mounted, so a power cut is visible to
     * e2fsck on unjournaled volumes. */
    if (fs->super.state != EXT2_ERROR_FS) {
        fs->super.state = EXT2_ERROR_FS;
        fs->summary_dirty = true;
    }
    if (fs->super.mnt_count != UINT16_MAX) ++fs->super.mnt_count;
    fs->super.mtime = now_seconds();
    fs->summary_dirty = true;
    return EXT2_OK;
}

ext2_status_t ext2_unmount(ext2_t *fs)
{
    if (fs == NULL) return EXT2_INVALID;
    ext2_status_t status = EXT2_OK;
    if (fs->mounted && fs->device != NULL) {
        if (fs->read_only) {
            /* The medium already failed: do not push the in-memory state that
             * is now ahead of it.  Detach all the same, so the mount can be
             * retried from a clean mount (and the journal replay). */
            fs->mounted = false;
            metadata_detach(fs);
            return EXT2_READ_ONLY;
        }
        /* Clean, and the commit below writes that with the summary. */
        if (fs->super.state != EXT2_VALID_FS) {
            fs->super.state = EXT2_VALID_FS;
            fs->summary_dirty = true;
        }
        fs->super.mtime = now_seconds();
        fs->summary_dirty = true;
        /* commit() is the durability point: it flushes a journaled
         * transaction (and its data) or a journal-less volume. */
        status = ext2_commit(fs);
    }
    /* A failed flush leaves the data in the cache: stay mounted so the
     * caller can retry (or the idle flusher can) instead of dropping it. */
    if (status != EXT2_OK) return status;
    fs->mounted = false;
    metadata_detach(fs);
    return status;
}

bool ext2_pending(const ext2_t *fs)
{
    return fs != NULL && fs->mounted &&
           (fs->summary_dirty || (fs->journal != NULL && fs->journal->live != 0));
}

ext2_status_t ext2_commit(ext2_t *fs)
{
    if (!fs_ready(fs)) return EXT2_INVALID;
    if (fs->read_only) return EXT2_IO_ERROR;
    if (!write_dirty_metadata(fs)) return EXT2_IO_ERROR;
    if (fs->summary_dirty && !write_summary(fs)) return EXT2_IO_ERROR;
    if (fs->journal != NULL && !journal_commit(fs)) return EXT2_IO_ERROR;
    if (!ext2_blockdev_sync(fs->device)) return EXT2_IO_ERROR;
    /* A journaled commit flushed the transaction and its data blocks.  A
     * volume without a journal wrote straight into the cache, so the flush is
     * what makes it durable. */
    if (fs->journal == NULL && !ext2_blockdev_flush(fs->device)) {
        return EXT2_IO_ERROR;
    }
    return EXT2_OK;
}

static ext2_status_t read_inode_symlink(ext2_t *fs,
                                        const ext2_inode_t *inode,
                                        char *out, size_t capacity,
                                        size_t *out_length)
{
    if (out == NULL || out_length == NULL || capacity == 0) {
        return EXT2_INVALID;
    }
    const size_t length = inode->size;
    if (length >= capacity) return EXT2_NAME_TOO_LONG;
    if (length > EXT2_BLOCK_BYTES) return EXT2_IO_ERROR;
    if (length <= sizeof(inode->block)) {
        memcpy(out, inode->block, length);
    } else {
        if (inode->block[0] < fs->first_data_block) return EXT2_IO_ERROR;
        if (!read_block(fs, inode->block[0], io_block)) return EXT2_IO_ERROR;
        memcpy(out, io_block, length);
    }
    out[length] = '\0';
    *out_length = length;
    return EXT2_OK;
}

static ext2_status_t lookup_internal_checked(ext2_t *fs, const char *path,
                                             bool follow_final,
                                             ext2_traverse_check_fn check,
                                             void *context,
                                             uint32_t *out_inode)
{
    if (!fs_ready(fs) || path == NULL || path[0] != '/' ||
        out_inode == NULL) {
        return EXT2_INVALID;
    }
    if (strcmp(path, "/") == 0) {
        *out_inode = EXT2_ROOT_INO;
        return EXT2_OK;
    }
    char *const current_path = lookup_current_path;
    const size_t path_length = strlen(path);
    if (path_length >= EXT2_PATH_BYTES) return EXT2_NAME_TOO_LONG;
    memcpy(current_path, path, path_length + 1);

    /* Iterative resolution keeps the stack frame bounded even for a symlink
     * chain that reaches the follow limit. */
    for (unsigned follows = 0; follows <= 8; ++follows) {
        uint32_t current = EXT2_ROOT_INO;
        char *const resolved = lookup_resolved;
        size_t resolved_length = 0;
        resolved[0] = '\0';
        const char *cursor = current_path;
        while (*cursor == '/') ++cursor;
        bool restarted = false;
        /* A symlink target may restart at the root; its search bit matters
         * before any component below it. */
        if (check != NULL) {
            const ext2_inode_t *const root = const_inode_ptr(fs, current);
            if (!check(context, current, true, root->mode, root->uid,
                       root->gid)) {
                return EXT2_ACCESS;
            }
        }

        while (*cursor != '\0') {
            const char *const start = cursor;
            while (*cursor != '\0' && *cursor != '/') ++cursor;
            const size_t length = (size_t)(cursor - start);
            const char *remaining = cursor;
            while (*remaining == '/') ++remaining;
            if (length == 0) {
                cursor = remaining;
                continue;
            }
            if (length > 255) return EXT2_NAME_TOO_LONG;
            if (!inode_is_dir(const_inode_ptr(fs, current))) {
                return EXT2_NOT_DIR;
            }

            char *const name = lookup_name;
            memcpy(name, start, length);
            name[length] = '\0';
            uint32_t next;
            ext2_status_t status = dir_find(fs, current, name, &next, NULL);
            if (status != EXT2_OK) return status;

            const ext2_inode_t *const next_inode = const_inode_ptr(fs, next);
            const bool last = *remaining == '\0';
            if (check != NULL && !last && inode_is_dir(next_inode) &&
                !check(context, next, true, next_inode->mode,
                       next_inode->uid, next_inode->gid)) {
                return EXT2_ACCESS;
            }
            if (inode_is_symlink(next_inode) && (!last || follow_final)) {
                char *const target = lookup_target;
                size_t target_length = 0;
                status = read_inode_symlink(fs, next_inode, target,
                                            EXT2_PATH_BYTES, &target_length);
                if (status != EXT2_OK) return status;
                if (target_length == 0) return EXT2_INVALID;

                char *const next_path = lookup_next_path;
                size_t position = 0;
                if (target[0] == '/') {
                    if (target_length >= EXT2_PATH_BYTES) {
                        return EXT2_NAME_TOO_LONG;
                    }
                    memcpy(next_path, target, target_length);
                    position = target_length;
                } else {
                    if (resolved_length >= EXT2_PATH_BYTES) {
                        return EXT2_NAME_TOO_LONG;
                    }
                    if (resolved_length != 0) {
                        memcpy(next_path, resolved, resolved_length);
                        position = resolved_length;
                    }
                    if (position == 0 || next_path[position - 1] != '/') {
                        if (position >= EXT2_PATH_BYTES) {
                            return EXT2_NAME_TOO_LONG;
                        }
                        next_path[position++] = '/';
                    }
                    if (target_length > EXT2_PATH_BYTES - position) {
                        return EXT2_NAME_TOO_LONG;
                    }
                    memcpy(next_path + position, target, target_length);
                    position += target_length;
                }
                if (*remaining != '\0') {
                    if (position == 0 || next_path[position - 1] != '/') {
                        if (position >= EXT2_PATH_BYTES) {
                            return EXT2_NAME_TOO_LONG;
                        }
                        next_path[position++] = '/';
                    }
                    const size_t remaining_length = strlen(remaining);
                    if (remaining_length > EXT2_PATH_BYTES - position) {
                        return EXT2_NAME_TOO_LONG;
                    }
                    memcpy(next_path + position, remaining, remaining_length);
                    position += remaining_length;
                }
                if (position == 0 || position >= EXT2_PATH_BYTES) {
                    return EXT2_NAME_TOO_LONG;
                }
                next_path[position] = '\0';
                memcpy(current_path, next_path, position + 1);
                restarted = true;
                break;
            }

            current = next;
            if (resolved_length + 1 + length >= EXT2_PATH_BYTES) {
                return EXT2_NAME_TOO_LONG;
            }
            resolved[resolved_length++] = '/';
            memcpy(resolved + resolved_length, name, length);
            resolved_length += length;
            resolved[resolved_length] = '\0';
            cursor = remaining;
        }
        if (!restarted) {
            *out_inode = current;
            return EXT2_OK;
        }
    }
    return EXT2_LOOP;
}

static ext2_status_t lookup_internal(ext2_t *fs, const char *path,
                                     bool follow_final, uint32_t *out_inode)
{
    return lookup_internal_checked(fs, path, follow_final, NULL, NULL,
                                   out_inode);
}

/* Resolve a path starting at `start` instead of the root, for a cwd whose
 * directory has been unlinked and no longer has a path.  `.` and `..` are
 * directory entries; a relative symlink restarts from the directory that
 * contains the link, an absolute one from the root.  `follow_final` selects
 * stat/open semantics versus lstat/readlink. */
static ext2_status_t lookup_from_checked(ext2_t *fs, uint32_t start,
                                         const char *path, bool follow_final,
                                         ext2_traverse_check_fn check,
                                         void *context, uint32_t *out_inode,
                                         char *cross_path,
                                         size_t cross_size)
{
    if (!fs_ready(fs) || path == NULL || out_inode == NULL) {
        return EXT2_INVALID;
    }
    char work[EXT2_PATH_BYTES];
    const size_t path_length = strlen(path);
    if (path_length >= sizeof(work)) return EXT2_NAME_TOO_LONG;
    memcpy(work, path, path_length + 1);

    uint32_t current = start;
    if (!inode_number_valid(fs, current) ||
        const_inode_ptr(fs, current)->mode == 0) {
        return EXT2_NOT_FOUND;
    }
    for (unsigned follows = 0; follows <= 8; ++follows) {
        const char *cursor = work;
        while (*cursor == '/') ++cursor;
        bool restarted = false;
        /* A symlink target may restart at the volume root; searching it is
         * still required before any component below it. */
        if (check != NULL && current == EXT2_ROOT_INO) {
            const ext2_inode_t *const root = const_inode_ptr(fs, current);
            if (!check(context, current, true, root->mode, root->uid,
                       root->gid)) {
                return EXT2_ACCESS;
            }
        }
        while (*cursor != '\0') {
            const char *const name = cursor;
            while (*cursor != '\0' && *cursor != '/') ++cursor;
            const size_t length = (size_t)(cursor - name);
            const char *remaining = cursor;
            while (*remaining == '/') ++remaining;
            if (length == 0) {
                cursor = remaining;
                continue;
            }
            if (length > 255) return EXT2_NAME_TOO_LONG;
            if (!inode_is_dir(const_inode_ptr(fs, current))) {
                return EXT2_NOT_DIR;
            }
            uint32_t next = current;
            if (length == 1 && name[0] == '.') {
                /* stay */
            } else if (length == 2 && name[0] == '.' && name[1] == '.') {
                const ext2_status_t status =
                    dir_find(fs, current, "..", &next, NULL);
                if (status != EXT2_OK) return status;
            } else {
                char component[EXT2_NAME_BYTES];
                memcpy(component, name, length);
                component[length] = '\0';
                const ext2_status_t found =
                    dir_find(fs, current, component, &next, NULL);
                if (found != EXT2_OK) return found;
                const bool last = *remaining == '\0';
                if (inode_is_symlink(const_inode_ptr(fs, next)) &&
                    (!last || follow_final)) {
                    char target[EXT2_PATH_BYTES];
                    size_t target_length = 0;
                    const ext2_status_t read =
                        read_inode_symlink(fs, const_inode_ptr(fs, next),
                                           target, sizeof(target),
                                           &target_length);
                    if (read != EXT2_OK) return read;
                    if (target_length == 0) return EXT2_INVALID;
                    const size_t remaining_length = strlen(remaining);
                    if (target_length + 1 + remaining_length + 1 >
                        sizeof(work)) {
                        return EXT2_NAME_TOO_LONG;
                    }
                    /* It is copied into `work` below, so the length check
                     * above (which rejects anything that does not fit there)
                     * also bounds this buffer. */
                    char combined[EXT2_PATH_BYTES];
                    memcpy(combined, target, target_length);
                    size_t pos = target_length;
                    if (remaining_length != 0) {
                        if (combined[pos - 1] != '/') combined[pos++] = '/';
                        memcpy(combined + pos, remaining, remaining_length);
                        pos += remaining_length;
                    }
                    combined[pos] = '\0';
                    if (target[0] == '/' && cross_path != NULL) {
                        /* The VFS owns the global root and the mount table. */
                        if (pos + 1 > cross_size) return EXT2_NAME_TOO_LONG;
                        memcpy(cross_path, combined, pos + 1);
                        return EXT2_CROSS_MOUNT;
                    }
                    memcpy(work, combined, pos + 1);
                    /* A relative target is relative to the directory that
                     * held the link, which is `current` here. */
                    current = target[0] == '/' ? EXT2_ROOT_INO : current;
                    restarted = true;
                    break;
                }
            }
            /* Every directory the walk descends through must be searchable.
             * The final component is left to the caller: stat(2) on a
             * directory does not need its own x bit, opendir/chdir check it
             * separately. */
            if (check != NULL && *remaining != '\0' &&
                inode_is_dir(const_inode_ptr(fs, next))) {
                const ext2_inode_t *const entry = const_inode_ptr(fs, next);
                if (!check(context, next, true, entry->mode, entry->uid,
                           entry->gid)) {
                    return EXT2_ACCESS;
                }
            }
            /* The "." branch keeps `current`, ".." and a name move it. */
            current = next;
            cursor = remaining;
        }
        if (!restarted) {
            *out_inode = current;
            return EXT2_OK;
        }
    }
    return EXT2_LOOP;
}

static ext2_status_t lookup_from(ext2_t *fs, uint32_t start,
                                 const char *path, bool follow_final,
                                 uint32_t *out_inode)
{
    return lookup_from_checked(fs, start, path, follow_final, NULL, NULL,
                               out_inode, NULL, 0);
}

ext2_status_t ext2_lookup_at(ext2_t *fs, uint32_t start_inode,
                             const char *path, uint32_t *out_inode)
{
    return lookup_status_(fs, lookup_from(fs, start_inode, path, true, out_inode));
}

ext2_status_t ext2_lookup_at_nofollow(ext2_t *fs, uint32_t start_inode,
                                      const char *path, uint32_t *out_inode)
{
    return lookup_status_(fs, lookup_from(fs, start_inode, path, false, out_inode));
}

ext2_status_t ext2_lookup_at_checked(ext2_t *fs, uint32_t start_inode,
                                     const char *path, bool follow_final,
                                     ext2_traverse_check_fn check,
                                     void *context, uint32_t *out_inode)
{
    return lookup_status_(fs, lookup_from_checked(fs, start_inode, path, follow_final, check,
                                                  context, out_inode, NULL, 0));
}

ext2_status_t ext2_lookup_cross(ext2_t *fs, uint32_t start_inode,
                                const char *path, bool follow_final,
                                ext2_traverse_check_fn check, void *context,
                                uint32_t *out_inode, char *cross_path,
                                size_t cross_size)
{
    if (cross_path == NULL || cross_size == 0) return EXT2_INVALID;
    return lookup_from_checked(fs, start_inode, path, follow_final, check,
                               context, out_inode, cross_path, cross_size);
}

/* Split a relative path and resolve everything before the last component,
 * starting at `start_inode`.  The returned name points into `path`. */
static ext2_status_t resolve_parent_at_checked(ext2_t *fs, uint32_t start_inode,
                                               const char *path,
                                               ext2_traverse_check_fn check,
                                               void *context,
                                               uint32_t *out_parent,
                                               const char **out_name)
{
    if (!fs_ready(fs) || path == NULL || out_parent == NULL ||
        out_name == NULL || path[0] == '\0') {
        return EXT2_INVALID;
    }
    const char *const slash = strrchr(path, '/');
    if (slash == NULL) {
        *out_name = path;
        *out_parent = start_inode;
        return inode_is_dir(const_inode_ptr(fs, start_inode)) ? EXT2_OK
                                                              : EXT2_NOT_DIR;
    }
    *out_name = slash + 1;
    if (**out_name == '\0') return EXT2_INVALID;
    char parent[EXT2_PATH_BYTES];
    const size_t length = (size_t)(slash - path);
    if (length >= sizeof(parent)) return EXT2_NAME_TOO_LONG;
    if (length == 0) {
        *out_parent = start_inode;
        return inode_is_dir(const_inode_ptr(fs, start_inode)) ? EXT2_OK
                                                              : EXT2_NOT_DIR;
    }
    memcpy(parent, path, length);
    parent[length] = '\0';
    /* An absolute path restarts at the volume root, as resolve_parent does,
     * but this walk reports an unsearchable component. */
    const uint32_t base = path[0] == '/' ? EXT2_ROOT_INO : start_inode;
    return lookup_from_checked(fs, base, parent, true, check, context,
                               out_parent, NULL, 0);
}

ext2_status_t ext2_resolve_parent_at(ext2_t *fs, uint32_t start_inode,
                                     const char *path, uint32_t *out_parent,
                                     const char **out_name)
{
    return resolve_parent_at_checked(fs, start_inode, path, NULL, NULL,
                                     out_parent, out_name);
}

ext2_status_t ext2_resolve_parent_at_checked(
    ext2_t *fs, uint32_t start_inode, const char *path,
    ext2_traverse_check_fn check, void *context, uint32_t *out_parent,
    const char **out_name)
{
    return resolve_parent_at_checked(fs, start_inode, path, check, context,
                                     out_parent, out_name);
}

static ext2_status_t lookup_status_(const ext2_t *fs, ext2_status_t status)
{
    // A bitmap or inode-table read failure must not look like a missing file:
    // WebDAV would otherwise turn a transient card error into 404/empty data.
    return fs != NULL && fs->cache_io_error ? EXT2_IO_ERROR : status;
}

ext2_status_t ext2_lookup(ext2_t *fs, const char *path, uint32_t *out_inode)
{
    return lookup_status_(fs, lookup_internal(fs, path, true, out_inode));
}

ext2_status_t ext2_lookup_nofollow(ext2_t *fs, const char *path,
                                   uint32_t *out_inode)
{
    return lookup_status_(fs, lookup_internal(fs, path, false, out_inode));
}

ext2_status_t ext2_lookup_checked(ext2_t *fs, const char *path,
                                  bool follow_final,
                                  ext2_traverse_check_fn check,
                                  void *context, uint32_t *out_inode)
{
    return lookup_status_(fs, lookup_internal_checked(fs, path, follow_final, check, context,
                                                      out_inode));
}

/* A regular file's high 32-bit size lives in i_dir_acl when large_file is
 * enabled.  ext2 file sizes are 32-bit, so refuse to expose a file that does
 * not fit instead of reading or writing the wrong bytes. */
static bool inode_large_file(const ext2_t *fs, const ext2_inode_t *inode)
{
    if (!EXT2_S_ISREG(inode->mode)) return false;
    if (inode->size > EXT2_MAX_FILE_BYTES) return true;
    return (fs->super.feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE) != 0 &&
           inode->dir_acl != 0;
}

static bool stat_from_inode(const ext2_t *fs, const ext2_inode_t *entry,
                            uint32_t ino, ext2_stat_t *out_stat)
{
    if (inode_large_file(fs, entry)) return false;
    out_stat->is_dir = inode_is_dir(entry);
    out_stat->size = entry->size;
    out_stat->mode = entry->mode;
    out_stat->uid = entry->uid;
    out_stat->gid = entry->gid;
    out_stat->links = entry->links_count;
    out_stat->ino = ino;
    out_stat->blocks = entry->blocks;
    out_stat->atime = entry->atime;
    out_stat->mtime = entry->mtime;
    out_stat->ctime = entry->ctime;
    return true;
}

ext2_status_t ext2_stat(ext2_t *fs, const char *path, ext2_stat_t *out_stat)
{
    if (out_stat == NULL) return EXT2_INVALID;
    uint32_t inode;
    const ext2_status_t status = ext2_lookup(fs, path, &inode);
    if (status != EXT2_OK) return status;
    const ext2_inode_t *const entry = const_inode_ptr(fs, inode);
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    return stat_from_inode(fs, entry, inode, out_stat) ? EXT2_OK : EXT2_UNSUPPORTED;
}

ext2_status_t ext2_lstat(ext2_t *fs, const char *path, ext2_stat_t *out_stat)
{
    if (out_stat == NULL) return EXT2_INVALID;
    uint32_t inode;
    const ext2_status_t status = ext2_lookup_nofollow(fs, path, &inode);
    if (status != EXT2_OK) return status;
    const ext2_inode_t *const entry = const_inode_ptr(fs, inode);
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    return stat_from_inode(fs, entry, inode, out_stat) ? EXT2_OK : EXT2_UNSUPPORTED;
}

/* An inode that has been freed (mode zero) no longer exists, even though its
 * number is still inside the table.  Every inode-handle operation starts
 * here so a descriptor cannot write through a number that was reused. */
static bool inode_live(ext2_t *fs, uint32_t inode)
{
    return inode_number_valid(fs, inode) &&
           const_inode_ptr(fs, inode)->mode != 0;
}

ext2_status_t ext2_stat_inode(ext2_t *fs, uint32_t inode,
                              ext2_stat_t *out_stat)
{
    if (!fs_ready(fs) || out_stat == NULL) return EXT2_INVALID;
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    if (!inode_live(fs, inode)) return fs->cache_io_error ? EXT2_IO_ERROR : EXT2_NOT_FOUND;
    const ext2_inode_t *const entry = const_inode_ptr(fs, inode);
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    return stat_from_inode(fs, entry, inode, out_stat)
               ? EXT2_OK : EXT2_UNSUPPORTED;
}

static ext2_status_t symlink_at_impl(ext2_t *fs, uint32_t parent,
                                     const char *name, const char *target)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || target == NULL || name == NULL ||
        name[0] == '\0') {
        return EXT2_INVALID;
    }
    const size_t length = strlen(target);
    if (length == 0) return EXT2_INVALID;
    if (length >= EXT2_NAME_BYTES) return EXT2_NAME_TOO_LONG;
    if (strlen(name) > 255) return EXT2_NAME_TOO_LONG;
    const ext2_status_t existing = dir_find(fs, parent, name, NULL, NULL);
    if (existing == EXT2_OK) return EXT2_EXISTS;
    if (existing != EXT2_NOT_FOUND) return existing;

    const uint32_t index = alloc_inode(fs);
    if (index == 0) return EXT2_NO_SPACE;
    ext2_inode_t *const inode = inode_ptr(fs, index);
    init_inode(fs, inode, EXT2_MODE_SYMLINK);
    inode->uid = fs->create_uid;
    inode->gid = fs->create_gid;
    inode->size = (uint32_t)length;
    inode->links_count = 1;

    uint32_t block = 0;
    if (length > sizeof(inode->block)) {
        block = alloc_block(fs);
        if (block == UINT32_MAX) {
            free_inode(fs, index);
            return EXT2_NO_SPACE;
        }
        memset(io_block, 0, sizeof(io_block));
        memcpy(io_block, target, length);
        if (!write_meta(fs, block, io_block)) {
            (void)free_block(fs, block);
            free_inode(fs, index);
            return EXT2_IO_ERROR;
        }
        inode->block[0] = block;
        inode->blocks = EXT2_BLOCK_BYTES / 512u;
    } else {
        memcpy(inode->block, target, length);
    }

    const ext2_status_t status = dir_add(fs, parent, name, index, 7);
    if (status != EXT2_OK) {
        if (block != 0) (void)free_block(fs, block);
        free_inode(fs, index);
        return status;
    }
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_symlink(ext2_t *fs, const char *target,
                           const char *link_path)
{
    if (!fs_ready(fs) || target == NULL || link_path == NULL) {
        return EXT2_INVALID;
    }
    uint32_t parent;
    const char *name;
    const ext2_status_t status = resolve_parent(fs, link_path, &parent, &name);
    if (status != EXT2_OK) return status;
    return symlink_at_impl(fs, parent, name, target);
}

ext2_status_t ext2_symlink_at(ext2_t *fs, uint32_t parent, const char *name,
                              const char *target)
{
    return symlink_at_impl(fs, parent, name, target);
}

ext2_status_t ext2_readlink(ext2_t *fs, const char *path, char *buffer,
                            size_t capacity, size_t *out_length)
{
    if (out_length != NULL) *out_length = 0;
    if (!fs_ready(fs) || path == NULL || buffer == NULL || capacity == 0 ||
        out_length == NULL) {
        return EXT2_INVALID;
    }
    uint32_t inode;
    const ext2_status_t status = ext2_lookup_nofollow(fs, path, &inode);
    if (status != EXT2_OK) return status;
    const ext2_inode_t *const entry = const_inode_ptr(fs, inode);
    if (!inode_is_symlink(entry)) return EXT2_INVALID;
    return read_inode_symlink(fs, entry, buffer, capacity, out_length);
}

ext2_status_t ext2_readlink_at(ext2_t *fs, uint32_t start_inode,
                               const char *path, char *buffer, size_t capacity,
                               size_t *out_length)
{
    if (out_length != NULL) *out_length = 0;
    if (!fs_ready(fs) || path == NULL || buffer == NULL || capacity == 0 ||
        out_length == NULL) {
        return EXT2_INVALID;
    }
    uint32_t inode;
    const ext2_status_t status =
        lookup_from(fs, start_inode, path, false, &inode);
    if (status != EXT2_OK) return status;
    const ext2_inode_t *const entry = const_inode_ptr(fs, inode);
    if (!inode_is_symlink(entry)) return EXT2_INVALID;
    return read_inode_symlink(fs, entry, buffer, capacity, out_length);
}

static bool dir_is_descendant(ext2_t *fs, uint32_t ancestor,
                              uint32_t candidate)
{
    uint32_t current = candidate;
    for (unsigned depth = 0; depth < 256; ++depth) {
        if (current == ancestor) return true;
        if (current == EXT2_ROOT_INO) return false;
        uint32_t parent;
        if (dir_find(fs, current, "..", &parent, NULL) != EXT2_OK) {
            return true;
        }
        if (parent == current) return false;
        current = parent;
    }
    return true;
}

static bool set_dotdot(ext2_t *fs, uint32_t dir_inode, uint32_t new_parent)
{
    ext2_inode_t *const dir = inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir) || !dir_block_ok(fs, dir->block[0])) {
        return false; /* ".." is in the first block */
    }
    if (!read_block(fs, dir->block[0], io_block)) return false;
    size_t offset = 0;
    while (offset < EXT2_BLOCK_BYTES) {
        uint8_t *const entry = io_block + offset;
        const uint16_t rec_len = get_u16(entry + 4);
        if (!dirent_fits(fs, entry, rec_len, offset)) {
            return false;
        }
        if (get_u32(entry) != 0 && dirent_name_len(fs, entry) == 2 &&
            entry[8] == '.' && entry[9] == '.') {
            put_u32(entry, new_parent);
            if (!write_meta(fs, dir->block[0], io_block)) return false;
            /* A cached ".." in this directory is now stale. */
            ++fs->dir_generation;
            return true;
        }
        offset += rec_len;
    }
    return false;
}

ext2_status_t ext2_rename(ext2_t *fs, const char *old_path,
                          const char *new_path)
{
    return ext2_rename_keep_target(fs, old_path, new_path, false);
}

static ext2_status_t rename_at_impl(ext2_t *fs, uint32_t old_parent,
                                    const char *old_name, uint32_t new_parent,
                                    const char *new_name, bool keep_target)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || old_name == NULL || new_name == NULL ||
        old_name[0] == '\0' || new_name[0] == '\0' ||
        strcmp(old_name, ".") == 0 || strcmp(old_name, "..") == 0 ||
        strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0) {
        return EXT2_INVALID;
    }

    uint32_t old_inode;
    uint8_t old_type;
    ext2_status_t status =
        dir_find(fs, old_parent, old_name, &old_inode, &old_type);
    if (status != EXT2_OK) return status;
    if (old_parent == new_parent && strcmp(old_name, new_name) == 0) {
        return EXT2_OK;
    }

    uint32_t target_inode = 0;
    uint8_t target_type = 0;
    const ext2_status_t target_status =
        dir_find(fs, new_parent, new_name, &target_inode, &target_type);
    if (target_status != EXT2_OK && target_status != EXT2_NOT_FOUND) {
        return target_status;
    }
    const bool target_exists = target_status == EXT2_OK;
    if (target_exists && target_inode == old_inode) return EXT2_OK;

    // The dirent type byte is a hint, not the authority: derive the type from
    // the inode so a corrupt byte cannot corrupt parent link counts.
    const ext2_inode_t *const old_entry = const_inode_ptr(fs, old_inode);
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    if (!inode_live(fs, old_inode)) return EXT2_NOT_FOUND;
    const bool old_is_dir = inode_is_dir(old_entry);
    bool target_is_dir = false;
    if (target_exists) {
        const ext2_inode_t *const target_entry = const_inode_ptr(fs, target_inode);
        if (fs->cache_io_error) return EXT2_IO_ERROR;
        if (!inode_live(fs, target_inode)) return EXT2_NOT_FOUND;
        target_is_dir = inode_is_dir(target_entry);
    }
    if (old_is_dir) {
        if (dir_is_descendant(fs, old_inode, new_parent)) return EXT2_INVALID;
        if (target_exists) {
            /* rename(2) reports ENOTDIR when a directory would replace a
             * non-directory (EISDIR is the other direction, below). */
            if (!target_is_dir) return EXT2_NOT_DIR;
            if (!dir_empty(fs, target_inode)) return EXT2_NOT_EMPTY;
        }
    } else if (target_exists && target_is_dir) {
        return EXT2_IS_DIR;
    }
    const uint8_t new_type = old_is_dir ? 2 : (old_type == 2 ? 1 : old_type);

    ext2_inode_t *const moved = inode_ptr(fs, old_inode);
    /* The renamed inode carries two names from here until the old one is
     * removed.  A file's link count says so, so a failure in between leaves a
     * consistent (if not yet renamed) entry instead of a name whose count is
     * one too low.  Directories count their subdirectories instead. */
    const bool double_name = !old_is_dir;
    if (double_name) {
        ++moved->links_count;
        touch_changed(moved);
    }
    if (target_exists) {
        /* The target's name goes, but its blocks and inode stay until the new
         * name is in place: a failure adding it then leaves an orphan that a
         * check can recover, not a file that has been silently destroyed. */
        status = dir_remove(fs, new_parent, new_name);
        if (status != EXT2_OK) {
            if (double_name) --moved->links_count;
            return status;
        }
    }

    status = dir_add(fs, new_parent, new_name, old_inode, new_type);
    if (status != EXT2_OK) {
        if (double_name) --moved->links_count;
        return status;
    }
    status = dir_remove(fs, old_parent, old_name);
    if (status != EXT2_OK) return status; /* two names, both counted */
    if (double_name) --moved->links_count;
    touch_changed(moved);
    if (target_exists) {
        ext2_inode_t *const target = inode_ptr(fs, target_inode);
        if (target_is_dir) {
            if (keep_target) {
                /* An open directory descriptor still names it: drop the last
                 * name but keep the inode (and its directory count) until
                 * that descriptor closes. */
                target->links_count = 0;
                touch_changed(target);
            } else {
                if (!free_inode_blocks(fs, target)) return EXT2_IO_ERROR;
                free_inode(fs, target_inode);
                count_dir(fs, target_inode, -1);
            }
            ext2_inode_t *const parent = inode_ptr(fs, new_parent);
            if (parent->links_count > 0) --parent->links_count;
        } else if (target->links_count > 1) {
            --target->links_count;
            touch_changed(target);
        } else if (keep_target) {
            target->links_count = 0;
            touch_changed(target);
        } else {
            if (!free_inode_blocks(fs, target)) return EXT2_IO_ERROR;
            free_inode(fs, target_inode);
        }
    }

    if (old_is_dir) {
        if (!set_dotdot(fs, old_inode, new_parent)) return EXT2_IO_ERROR;
        ext2_inode_t *const old_parent_node = inode_ptr(fs, old_parent);
        if (old_parent_node->links_count > 0) --old_parent_node->links_count;
        ext2_inode_t *const new_parent_node = inode_ptr(fs, new_parent);
        ++new_parent_node->links_count;
        /* POSIX: the renamed directory's status change time moves too. */
        touch_changed(inode_ptr(fs, old_inode));
    }
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_rename_keep_target(ext2_t *fs, const char *old_path,
                                      const char *new_path,
                                      bool keep_target)
{
    if (!fs_ready(fs) || old_path == NULL || new_path == NULL) {
        return EXT2_INVALID;
    }
    uint32_t old_parent;
    uint32_t new_parent;
    const char *old_name;
    const char *new_name;
    ext2_status_t status = resolve_parent(fs, old_path, &old_parent, &old_name);
    if (status != EXT2_OK) return status;
    status = resolve_parent(fs, new_path, &new_parent, &new_name);
    if (status != EXT2_OK) return status;
    return rename_at_impl(fs, old_parent, old_name, new_parent, new_name,
                          keep_target);
}

ext2_status_t ext2_rename_at(ext2_t *fs, uint32_t old_parent,
                             const char *old_name, uint32_t new_parent,
                             const char *new_name, bool keep_target)
{
    return rename_at_impl(fs, old_parent, old_name, new_parent, new_name,
                          keep_target);
}

ext2_status_t ext2_chown_inode(ext2_t *fs, uint32_t inode_index,
                               uint16_t uid, uint16_t gid)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs)) return EXT2_INVALID;
    if (!inode_live(fs, inode_index)) return EXT2_NOT_FOUND;
    /* chown(2) passes (uid_t)-1 for a field that must stay as it is; on this
     * target uid_t is 16 bits, so UINT16_MAX is the sentinel. */
    if (uid == UINT16_MAX && gid == UINT16_MAX) return EXT2_OK;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (uid != UINT16_MAX) inode->uid = uid;
    if (gid != UINT16_MAX) inode->gid = gid;
    /* chown(2): the set-user-ID and set-group-ID bits must not survive an
     * ownership change (POSIX; Linux clears both on any chown). */
    inode->mode &= (uint16_t)~(04000u | 02000u);
    touch_changed(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_chown(ext2_t *fs, const char *path, uint16_t uid,
                         uint16_t gid)
{
    if (!fs_ready(fs) || path == NULL) return EXT2_INVALID;
    uint32_t inode_index;
    const ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    return ext2_chown_inode(fs, inode_index, uid, gid);
}

static ext2_status_t link_at_impl(ext2_t *fs, uint32_t target,
                                  uint32_t parent, const char *name)
{
    if (!fs_ready(fs) || name == NULL || name[0] == '\0') {
        return EXT2_INVALID;
    }
    ext2_inode_t *const inode = inode_ptr(fs, target);
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode->links_count == UINT16_MAX) return EXT2_NO_SPACE;
    const uint8_t type = inode_is_symlink(inode) ? 7 : 1;

    if (strlen(name) > 255) return EXT2_NAME_TOO_LONG;
    const ext2_status_t existing = dir_find(fs, parent, name, NULL, NULL);
    if (existing == EXT2_OK) return EXT2_EXISTS;
    if (existing != EXT2_NOT_FOUND) return existing;

    const ext2_status_t status = dir_add(fs, parent, name, target, type);
    if (status != EXT2_OK) return status;
    ++inode->links_count;
    touch_changed(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_link(ext2_t *fs, const char *existing_path,
                        const char *new_path)
{
    if (!fs_ready(fs) || existing_path == NULL || new_path == NULL) {
        return EXT2_INVALID;
    }
    uint32_t target;
    /* link(2) does not follow a symlink in the final component: the new name
     * refers to the link itself (intermediate components are followed by the
     * path walker, as POSIX says). */
    const ext2_status_t status =
        ext2_lookup_nofollow(fs, existing_path, &target);
    if (status != EXT2_OK) return status;
    uint32_t parent;
    const char *name;
    const ext2_status_t resolved =
        resolve_parent(fs, new_path, &parent, &name);
    if (resolved != EXT2_OK) return resolved;
    return link_at_impl(fs, target, parent, name);
}

ext2_status_t ext2_link_at(ext2_t *fs, uint32_t target, uint32_t parent,
                           const char *name)
{
    return link_at_impl(fs, target, parent, name);
}

ext2_status_t ext2_chmod_inode(ext2_t *fs, uint32_t inode_index, uint16_t mode)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || (mode & (uint16_t)~0x0FFFu) != 0) {
        return EXT2_INVALID;
    }
    if (!inode_live(fs, inode_index)) return EXT2_NOT_FOUND;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    inode->mode = (uint16_t)((inode->mode & EXT2_S_IFMT) | (mode & 0x0FFFu));
    touch_changed(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_chmod(ext2_t *fs, const char *path, uint16_t mode)
{
    if (!fs_ready(fs) || path == NULL) return EXT2_INVALID;
    uint32_t inode_index;
    const ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    return ext2_chmod_inode(fs, inode_index, mode);
}

ext2_status_t ext2_set_times(ext2_t *fs, const char *path, uint32_t atime,
                             uint32_t mtime)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || path == NULL) return EXT2_INVALID;
    uint32_t inode_index;
    const ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    return ext2_set_times_inode(fs, inode_index, atime, mtime);
}

ext2_status_t ext2_set_times_inode(ext2_t *fs, uint32_t inode_index,
                                   uint32_t atime, uint32_t mtime)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs)) return EXT2_INVALID;
    if (!inode_live(fs, inode_index)) return EXT2_NOT_FOUND;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (mtime == UINT32_MAX) atime = mtime = now_seconds(); /* "now" */
    inode->atime = atime;
    inode->mtime = mtime;
    touch_changed(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

#ifdef WEBDAV_FILING_HOST_TEST
ext2_status_t ext2_test_set_group_bitmap_byte(ext2_t *fs, uint32_t group,
                                              size_t offset, uint8_t value)
{
    if (!fs_ready(fs) || offset >= EXT2_BITMAP_BYTES) return EXT2_INVALID;
    ext2_bitmap_cache_entry_t *const entry = bitmap_cache_get_(fs, group);
    if (entry == NULL) return EXT2_IO_ERROR;
    entry->block_bitmap[offset] = value;
    entry->dirty_block = 1;
    return EXT2_OK;
}

void ext2_test_set_inode_hint(ext2_t *fs, uint32_t inode)
{
    if (fs_ready(fs)) fs->inode_hint = inode;
}

ext2_status_t ext2_test_reload_metadata(ext2_t *fs)
{
    if (!fs_ready(fs)) return EXT2_INVALID;
    /* Journal replay uses the same three steps after writing the medium. */
    cache_invalidate(fs);
    if (!read_super(fs)) return EXT2_IO_ERROR;
    return load_metadata(fs);
}
#endif

ext2_status_t ext2_space(ext2_t *fs, ext2_space_t *out_space)
{
    if (!fs_ready(fs) || out_space == NULL) return EXT2_INVALID;
    out_space->block_size = EXT2_BLOCK_BYTES;
    out_space->block_count = fs->block_count;
    out_space->free_blocks = fs->free_blocks;
    out_space->inode_count = fs->inode_count;
    out_space->free_inodes = fs->free_inodes;
    return EXT2_OK;
}

ext2_status_t ext2_mkdir(ext2_t *fs, const char *path)
{
    return create_inode(fs, path, EXT2_MODE_DIR);
}

ext2_status_t ext2_mkdir_mode(ext2_t *fs, const char *path, uint16_t mode)
{
    const uint16_t permissions =
        mode != 0 ? (uint16_t)(mode & 07777u)
                  : (uint16_t)(EXT2_MODE_DIR & 07777u);
    return create_inode(fs, path,
                        (uint16_t)(EXT2_S_IFDIR | permissions));
}

ext2_status_t ext2_mkdir_at(ext2_t *fs, uint32_t parent, const char *name,
                            uint16_t mode)
{
    const uint16_t permissions =
        mode != 0 ? (uint16_t)(mode & 07777u)
                  : (uint16_t)(EXT2_MODE_DIR & 07777u);
    return create_inode_at(fs, parent, name,
                           (uint16_t)(EXT2_S_IFDIR |
                                      permissions));
}

ext2_status_t ext2_touch_at(ext2_t *fs, uint32_t parent, const char *name,
                            uint16_t mode)
{
    if (fs == NULL || parent == 0 || name == NULL || name[0] == '\0') {
        return EXT2_INVALID;
    }
    uint32_t inode;
    ext2_status_t status = dir_find(fs, parent, name, &inode, NULL);
    if (status == EXT2_OK) {
        return inode_is_dir(const_inode_ptr(fs, inode)) ? EXT2_IS_DIR
                                                        : EXT2_OK;
    }
    if (status != EXT2_NOT_FOUND) return status;
    const uint16_t permissions =
        mode != 0 ? (uint16_t)(mode & 07777u)
                  : (uint16_t)(EXT2_MODE_FILE & 07777u);
    return create_inode_at(fs, parent, name,
                           (uint16_t)(EXT2_S_IFREG |
                                      permissions));
}

ext2_status_t ext2_create_at(ext2_t *fs, uint32_t parent, const char *name)
{
    return create_inode_at(fs, parent, name, EXT2_MODE_FILE);
}

ext2_status_t ext2_mkfifo(ext2_t *fs, const char *path, uint16_t mode)
{
    return create_inode(fs, path,
                        (uint16_t)(EXT2_S_IFIFO | (mode & 07777u)));
}

ext2_status_t ext2_mkfifo_at(ext2_t *fs, uint32_t parent, const char *name,
                             uint16_t mode)
{
    return create_inode_at(fs, parent, name,
                           (uint16_t)(EXT2_S_IFIFO | (mode & 07777u)));
}

ext2_status_t ext2_touch(ext2_t *fs, const char *path)
{
    return ext2_touch_mode(fs, path, 0);
}

ext2_status_t ext2_touch_mode(ext2_t *fs, const char *path, uint16_t mode)
{
    uint32_t inode;
    ext2_status_t status = ext2_lookup(fs, path, &inode);
    if (status == EXT2_OK) {
        return inode_is_dir(const_inode_ptr(fs, inode)) ? EXT2_IS_DIR
                                                        : EXT2_OK;
    }
    if (status != EXT2_NOT_FOUND) return status;
    /* Mode 0 means "the default", so an old caller that passed no mode keeps
     * the previous creation permissions. */
    const uint16_t permissions =
        mode != 0 ? (uint16_t)(mode & 07777u)
                  : (uint16_t)(EXT2_MODE_FILE & 07777u);
    return create_inode(fs, path,
                        (uint16_t)(EXT2_S_IFREG | permissions));
}

/* Remove `name` from `parent`.  With retain, the inode and its blocks stay
 * allocated after the name is gone (links_count reaches zero) so an open
 * descriptor or a retained cwd can keep using it; ext2_release_inode() frees
 * it later. */
static ext2_status_t unlink_at_impl(ext2_t *fs, uint32_t parent,
                                    const char *name, bool retain)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || name == NULL || name[0] == '\0' ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return EXT2_INVALID;
    }
    if (!inode_number_valid(fs, parent) || !inode_live(fs, parent) ||
        !inode_is_dir(const_inode_ptr(fs, parent))) {
        return EXT2_NOT_DIR;
    }
    uint32_t inode_index;
    uint8_t type;
    ext2_status_t status = dir_find(fs, parent, name, &inode_index, &type);
    if (status != EXT2_OK) return status;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    const bool is_dir = type == 2;
    if (is_dir && !dir_empty(fs, inode_index)) return EXT2_NOT_EMPTY;
    if (!is_dir && inode->links_count > 1) {
        status = dir_remove(fs, parent, name);
        if (status != EXT2_OK) return status;
        --inode->links_count;
        touch_changed(inode);
        return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
    }
    /* The name goes first: if the write fails, nothing has been destroyed and
     * the file is still there to retry.  Freeing first (as it did) emptied the
     * file and then reported the error, with the name still pointing at it. */
    status = dir_remove(fs, parent, name);
    if (status != EXT2_OK) return status;
    if (is_dir) {
        ext2_inode_t *const parent_inode = inode_ptr(fs, parent);
        if (parent_inode->links_count > 0) --parent_inode->links_count;
        /* A retained directory stays a live directory inode: its own count
         * goes when the inode is finally freed, with any entries created
         * after the name was removed. */
        if (!retain) count_dir(fs, inode_index, -1);
    }
    if (retain) {
        inode->links_count = 0;
        touch_changed(inode);
        return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
    }
    if (!free_inode_blocks(fs, inode)) return EXT2_IO_ERROR; /* blocks leak, name is gone */
    free_inode(fs, inode_index);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_unlink(ext2_t *fs, const char *path)
{
    uint32_t parent;
    const char *name;
    const ext2_status_t status = resolve_parent(fs, path, &parent, &name);
    if (status != EXT2_OK) return status;
    return unlink_at_impl(fs, parent, name, false);
}

/* unlink(2) for a file that still has an open descriptor. */
ext2_status_t ext2_unlink_retain(ext2_t *fs, const char *path)
{
    uint32_t parent;
    const char *name;
    const ext2_status_t status = resolve_parent(fs, path, &parent, &name);
    if (status != EXT2_OK) return status;
    return unlink_at_impl(fs, parent, name, true);
}

ext2_status_t ext2_unlink_at(ext2_t *fs, uint32_t parent, const char *name)
{
    return unlink_at_impl(fs, parent, name, false);
}

ext2_status_t ext2_unlink_retain_at(ext2_t *fs, uint32_t parent,
                                    const char *name)
{
    return unlink_at_impl(fs, parent, name, true);
}

/* Free an inode tree.  A directory whose name was removed can still gain
 * entries through an inode-relative cwd (POSIX allows it); when the last
 * reference goes, those entries must be freed with it, or their inodes and
 * blocks are lost to the volume. */
static bool free_inode_tree(ext2_t *fs, uint32_t inode_index, unsigned depth)
{
    if (depth > 64 || !inode_live(fs, inode_index)) return false;
    ext2_inode_t *const node = inode_ptr(fs, inode_index);
    if (inode_is_dir(node)) {
        unsigned index = 0;
        for (;;) {
            uint32_t child = 0;
            const ext2_status_t status =
                ext2_list_child(fs, inode_index, index, &child);
            if (status == EXT2_NOT_FOUND) break;
            if (status != EXT2_OK) return false;
            ++index;
            if (!free_inode_tree(fs, child, depth + 1)) return false;
        }
    }
    if (!free_inode_blocks(fs, node)) return false;
    if (inode_is_dir(node)) count_dir(fs, inode_index, -1);
    free_inode(fs, inode_index);
    return true;
}

ext2_status_t ext2_release_inode(ext2_t *fs, uint32_t inode_index)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!inode_number_valid(fs, inode_index)) {
        return EXT2_INVALID;
    }
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (inode->mode == 0) return EXT2_OK; /* already freed */
    if (inode->links_count != 0) return EXT2_INVALID; /* still has a name */
    if (!free_inode_tree(fs, inode_index, 0)) return EXT2_IO_ERROR;
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

/* Free one inode and its blocks regardless of its link count.  The caller
 * (the VFS tree release) has already disposed of the directory entries that
 * named it; it commits once after the whole tree instead of syncing per
 * inode. */
ext2_status_t ext2_free_inode_unlinked(ext2_t *fs, uint32_t inode_index)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || !inode_number_valid(fs, inode_index)) {
        return EXT2_INVALID;
    }
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (inode->mode == 0) return EXT2_OK; /* already freed */
    if (!free_inode_blocks(fs, inode)) return EXT2_IO_ERROR;
    if (inode_is_dir(inode)) count_dir(fs, inode_index, -1);
    free_inode(fs, inode_index);
    /* The caller commits once after the whole tree.  A big tree must not
     * overflow the journal transaction, so flush when it is nearly full. */
    if (fs->journal != NULL &&
        fs->journal->count >= JBD2_TXN_CAPACITY * 3 / 4 &&
        !sync_metadata(fs)) {
        return EXT2_IO_ERROR;
    }
    return EXT2_OK;
}

/* Point a directory's ".." at `new_parent`.  Used when an entry inside an
 * unlinked directory survives the directory's release: it becomes its own
 * root so navigation never reaches the freed parent. */
ext2_status_t ext2_reparent(ext2_t *fs, uint32_t dir_inode,
                            uint32_t new_parent)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || !inode_live(fs, dir_inode) ||
        !inode_live(fs, new_parent) ||
        !inode_is_dir(const_inode_ptr(fs, dir_inode))) {
        return EXT2_INVALID;
    }
    if (dir_inode == EXT2_ROOT_INO) return EXT2_OK;
    if (!set_dotdot(fs, dir_inode, new_parent)) return EXT2_IO_ERROR;
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

/* ---- mapping a file's blocks ------------------------------------------
 * Block pointers: 12 direct, one single-indirect (1024 blocks), one
 * double-indirect (1024 x 1024). That reaches 4 GiB; files are limited to 2 GiB
 * so no "large file" feature is needed. A pointer of 0 is a block that is not
 * there. Indirect blocks are read through two static buffers (one per level),
 * which the VFS lock serialises. */
enum {
    EXT2_PTRS = EXT2_INDIRECT_ENTRIES,
    EXT2_SINGLE_FIRST = EXT2_DIRECT_BLOCKS,
    EXT2_DOUBLE_FIRST = EXT2_DIRECT_BLOCKS + EXT2_INDIRECT_ENTRIES,
    EXT2_MAP_LIMIT = EXT2_DOUBLE_FIRST + EXT2_INDIRECT_ENTRIES * EXT2_INDIRECT_ENTRIES,
};

static uint8_t map_level1[EXT2_BLOCK_BYTES] EXT2_EXT_RAM_BSS;
static uint8_t map_level2[EXT2_BLOCK_BYTES] EXT2_EXT_RAM_BSS;

/* Which indirect block each static buffer currently holds.  A read or write
 * that walks consecutive blocks of one file used to reload the same root (and
 * middle) block from the cache for every 4 KiB of data, doubling or tripling
 * the memory traffic of a large transfer.  The cache is local to one loop and
 * is dropped whenever a mapping changes. */
typedef struct {
    uint32_t level1;
    uint32_t level2;
} bmap_cache_t;

static bool valid_data_block(const ext2_t *fs, uint32_t block)
{
    return block >= fs->first_data_block && block < fs->block_count;
}

/* The physical block holding logical block `index`, 0 if there is none. */
static ext2_status_t bmap_get_cached(ext2_t *fs, const ext2_inode_t *inode,
                                     uint32_t index, uint32_t *out,
                                     bmap_cache_t *cache)
{
    *out = 0;
    if (index < EXT2_SINGLE_FIRST) {
        *out = inode->block[index];
    } else if (index < EXT2_DOUBLE_FIRST) {
        const uint32_t root = inode->block[EXT2_DIRECT_BLOCKS];
        if (root == 0) return EXT2_OK;
        if (cache->level1 != root) {
            if (!valid_data_block(fs, root) ||
                !read_block(fs, root, map_level1)) {
                return EXT2_IO_ERROR;
            }
            cache->level1 = root;
            cache->level2 = 0;
        }
        *out = get_u32(map_level1 + (index - EXT2_SINGLE_FIRST) * 4);
    } else if (index < EXT2_MAP_LIMIT) {
        const uint32_t root = inode->block[EXT2_DIRECT_BLOCKS + 1];
        if (root == 0) return EXT2_OK;
        const uint32_t rel = index - EXT2_DOUBLE_FIRST;
        if (cache->level1 != root) {
            if (!valid_data_block(fs, root) ||
                !read_block(fs, root, map_level1)) {
                return EXT2_IO_ERROR;
            }
            cache->level1 = root;
            cache->level2 = 0;
        }
        const uint32_t mid = get_u32(map_level1 + (rel / EXT2_PTRS) * 4);
        if (mid == 0) return EXT2_OK;
        if (cache->level2 != mid) {
            if (!valid_data_block(fs, mid) ||
                !read_block(fs, mid, map_level2)) {
                return EXT2_IO_ERROR;
            }
            cache->level2 = mid;
        }
        *out = get_u32(map_level2 + (rel % EXT2_PTRS) * 4);
    } else {
        return EXT2_UNSUPPORTED;
    }
    if (*out != 0 && !valid_data_block(fs, *out)) return EXT2_IO_ERROR;
    return EXT2_OK;
}

static ext2_status_t bmap_get(ext2_t *fs, const ext2_inode_t *inode,
                              uint32_t index, uint32_t *out)
{
    bmap_cache_t cache = {0};
    return bmap_get_cached(fs, inode, index, out, &cache);
}

/* Find a bounded run of physically contiguous data blocks. This reads each
 * indirect table once for the whole run instead of once per logical block,
 * which matters because one ext2 block is only 4 KiB on an SD card. A hole
 * reports a one-block run so the caller can zero it and retry. */
static ext2_status_t bmap_get_contiguous_run(ext2_t *fs, const ext2_inode_t *inode,
                                             uint32_t index, uint32_t limit,
                                             uint32_t *out_first, uint32_t *out_count,
                                             bmap_cache_t *cache)
{
    *out_first = 0;
    *out_count = 0;
    if (limit == 0 || index >= EXT2_MAP_LIMIT) return EXT2_UNSUPPORTED;

    const uint8_t *entries = NULL;
    uint32_t entry_index;
    uint32_t available = limit;

    if (index < EXT2_SINGLE_FIRST) {
        const uint32_t direct_left = EXT2_SINGLE_FIRST - index;
        if (available > direct_left) available = direct_left;
        const uint32_t first = inode->block[index];
        if (first == 0) {
            *out_count = 1;
            return EXT2_OK;
        }
        if (!valid_data_block(fs, first)) return EXT2_IO_ERROR;
        uint32_t count = 1;
        while (count < available && first <= UINT32_MAX - count) {
            const uint32_t next = inode->block[index + count];
            if (next == 0 || !valid_data_block(fs, next) || next != first + count) break;
            ++count;
        }
        *out_first = first;
        *out_count = count;
        return EXT2_OK;
    }

    if (index < EXT2_DOUBLE_FIRST) {
        const uint32_t root = inode->block[EXT2_DIRECT_BLOCKS];
        if (root == 0) {
            *out_count = 1;
            return EXT2_OK;
        }
        if (cache->level1 != root) {
            if (!valid_data_block(fs, root) || !read_block(fs, root, map_level1))
                return EXT2_IO_ERROR;
            cache->level1 = root;
            cache->level2 = 0;
        }
        entries = map_level1;
        entry_index = index - EXT2_SINGLE_FIRST;
        const uint32_t single_left = EXT2_DOUBLE_FIRST - index;
        if (available > single_left) available = single_left;
    } else {
        const uint32_t root = inode->block[EXT2_DIRECT_BLOCKS + 1];
        if (root == 0) {
            *out_count = 1;
            return EXT2_OK;
        }
        const uint32_t relative = index - EXT2_DOUBLE_FIRST;
        if (cache->level1 != root) {
            if (!valid_data_block(fs, root) || !read_block(fs, root, map_level1))
                return EXT2_IO_ERROR;
            cache->level1 = root;
            cache->level2 = 0;
        }
        const uint32_t mid = get_u32(map_level1 + (relative / EXT2_PTRS) * 4);
        if (mid == 0) {
            *out_count = 1;
            return EXT2_OK;
        }
        if (cache->level2 != mid) {
            if (!valid_data_block(fs, mid) || !read_block(fs, mid, map_level2))
                return EXT2_IO_ERROR;
            cache->level2 = mid;
        }
        entries = map_level2;
        entry_index = relative % EXT2_PTRS;
        const uint32_t leaf_left = EXT2_PTRS - entry_index;
        if (available > leaf_left) available = leaf_left;
    }

    const uint32_t first = get_u32(entries + (size_t) entry_index * 4);
    if (first == 0) {
        *out_count = 1;
        return EXT2_OK;
    }
    if (!valid_data_block(fs, first)) return EXT2_IO_ERROR;
    uint32_t count = 1;
    while (count < available && first <= UINT32_MAX - count) {
        const uint32_t next = get_u32(entries + (size_t) (entry_index + count) * 4);
        if (next == 0 || !valid_data_block(fs, next) || next != first + count) break;
        ++count;
    }
    *out_first = first;
    *out_count = count;
    return EXT2_OK;
}

/* Allocate an indirect block (zeroed) and count it in the inode. */
static uint32_t alloc_indirect(ext2_t *fs, ext2_inode_t *inode)
{
    const uint32_t block = alloc_block(fs);
    if (block == UINT32_MAX) return 0;
    memset(io_block, 0, sizeof(io_block));
    if (!write_meta(fs, block, io_block)) {
        (void)free_block(fs, block);
        return 0;
    }
    inode->blocks += EXT2_BLOCK_BYTES / 512u;
    return block;
}

/* Record that logical block `index` lives at `phys`, creating the indirect
 * blocks on the way. */
static ext2_status_t bmap_set(ext2_t *fs, ext2_inode_t *inode, uint32_t index,
                              uint32_t phys)
{
    if (index < EXT2_SINGLE_FIRST) {
        inode->block[index] = phys;
        return EXT2_OK;
    }
    if (index >= EXT2_MAP_LIMIT) return EXT2_NO_SPACE;
    uint32_t *root;
    uint32_t rel;
    if (index < EXT2_DOUBLE_FIRST) {
        root = &inode->block[EXT2_DIRECT_BLOCKS];
        rel = index - EXT2_SINGLE_FIRST;
    } else {
        root = &inode->block[EXT2_DIRECT_BLOCKS + 1];
        rel = index - EXT2_DOUBLE_FIRST;
    }
    if (*root == 0) {
        *root = alloc_indirect(fs, inode);
        if (*root == 0) return EXT2_NO_SPACE;
    }
    uint32_t leaf = *root;
    uint32_t slot = rel;
    if (index >= EXT2_DOUBLE_FIRST) {
        if (!read_block(fs, *root, map_level1)) return EXT2_IO_ERROR;
        uint32_t mid = get_u32(map_level1 + (rel / EXT2_PTRS) * 4);
        if (mid == 0) {
            mid = alloc_indirect(fs, inode);
            if (mid == 0) return EXT2_NO_SPACE;
            if (!read_block(fs, *root, map_level1)) return EXT2_IO_ERROR;
            put_u32(map_level1 + (rel / EXT2_PTRS) * 4, mid);
            if (!write_meta(fs, *root, map_level1)) return EXT2_IO_ERROR;
        }
        leaf = mid;
        slot = rel % EXT2_PTRS;
    }
    if (!read_block(fs, leaf, map_level2)) return EXT2_IO_ERROR;
    put_u32(map_level2 + slot * 4, phys);
    return write_meta(fs, leaf, map_level2) ? EXT2_OK : EXT2_IO_ERROR;
}

static bool table_is_empty(const uint8_t *table)
{
    for (unsigned i = 0; i < EXT2_BLOCK_BYTES; ++i) {
        if (table[i] != 0) return false;
    }
    return true;
}

/* Free every logical block from `keep` on, and the indirect blocks that end up
 * empty. */
static bool bmap_free_from(ext2_t *fs, ext2_inode_t *inode, uint32_t keep)
{
    if (inode_is_symlink(inode) && inode->size <= sizeof(inode->block)) return true;
    const uint32_t metadata_unit = EXT2_BLOCK_BYTES / 512u;
    for (uint32_t i = keep < EXT2_SINGLE_FIRST ? keep : EXT2_SINGLE_FIRST;
         i < EXT2_SINGLE_FIRST; ++i) {
        if (inode->block[i] != 0) {
            if (!free_block(fs, inode->block[i])) return false;
            inode->block[i] = 0;
            inode->blocks -= inode->blocks >= metadata_unit ? metadata_unit : 0;
        }
    }
    /* single indirect */
    uint32_t *const single = &inode->block[EXT2_DIRECT_BLOCKS];
    if (*single != 0 && keep < EXT2_DOUBLE_FIRST) {
        if (!read_block(fs, *single, map_level1)) return false;
        const uint32_t from = keep > EXT2_SINGLE_FIRST ? keep - EXT2_SINGLE_FIRST : 0;
        for (uint32_t k = from; k < EXT2_PTRS; ++k) {
            const uint32_t block = get_u32(map_level1 + k * 4);
            if (block == 0) continue;
            if (!free_block(fs, block)) return false;
            put_u32(map_level1 + k * 4, 0);
            inode->blocks -= inode->blocks >= metadata_unit ? metadata_unit : 0;
        }
        if (from == 0 || table_is_empty(map_level1)) {
            if (!free_block(fs, *single)) return false;
            *single = 0;
            inode->blocks -= inode->blocks >= metadata_unit ? metadata_unit : 0;
        } else if (!write_meta(fs, *single, map_level1)) {
            return false;
        }
    }
    /* double indirect */
    uint32_t *const dbl = &inode->block[EXT2_DIRECT_BLOCKS + 1];
    if (*dbl != 0) {
        const uint32_t rel_keep = keep > EXT2_DOUBLE_FIRST ? keep - EXT2_DOUBLE_FIRST : 0;
        for (uint32_t m = rel_keep / EXT2_PTRS; m < EXT2_PTRS; ++m) {
            if (!read_block(fs, *dbl, map_level1)) return false;
            const uint32_t mid = get_u32(map_level1 + m * 4);
            if (mid == 0) continue;
            const uint32_t from = (m == rel_keep / EXT2_PTRS) ? rel_keep % EXT2_PTRS : 0;
            if (!read_block(fs, mid, map_level2)) return false;
            for (uint32_t k = from; k < EXT2_PTRS; ++k) {
                const uint32_t block = get_u32(map_level2 + k * 4);
                if (block == 0) continue;
                if (!free_block(fs, block)) return false;
                put_u32(map_level2 + k * 4, 0);
                inode->blocks -= inode->blocks >= metadata_unit ? metadata_unit : 0;
            }
            if (from == 0 || table_is_empty(map_level2)) {
                if (!free_block(fs, mid)) return false;
                inode->blocks -= inode->blocks >= metadata_unit ? metadata_unit : 0;
                if (!read_block(fs, *dbl, map_level1)) return false;
                put_u32(map_level1 + m * 4, 0);
                if (!write_meta(fs, *dbl, map_level1)) return false;
            } else if (!write_meta(fs, mid, map_level2)) {
                return false;
            }
        }
        if (!read_block(fs, *dbl, map_level1)) return false;
        if (rel_keep == 0 || table_is_empty(map_level1)) {
            if (!free_block(fs, *dbl)) return false;
            *dbl = 0;
            inode->blocks -= inode->blocks >= metadata_unit ? metadata_unit : 0;
        }
    }
    return true;
}

/* How many blocks (data and indirect) a file of `blocks` data blocks needs
 * beyond what `have` data blocks already use. */
static uint32_t blocks_needed(uint32_t have, uint32_t want)
{
    if (want <= have) return 0;
    uint32_t extra = want - have;
    if (want > EXT2_SINGLE_FIRST && have <= EXT2_SINGLE_FIRST) ++extra;
    if (want > EXT2_DOUBLE_FIRST) {
        const uint32_t leaves = (want - EXT2_DOUBLE_FIRST + EXT2_PTRS - 1) / EXT2_PTRS;
        const uint32_t had = have > EXT2_DOUBLE_FIRST
            ? (have - EXT2_DOUBLE_FIRST + EXT2_PTRS - 1) / EXT2_PTRS : 0;
        extra += leaves - had + (have <= EXT2_DOUBLE_FIRST ? 1 : 0);
    }
    return extra;
}

static uint32_t file_blocks(size_t size)
{
    return (uint32_t)((size + EXT2_BLOCK_BYTES - 1) / EXT2_BLOCK_BYTES);
}

/* Write `length` bytes at `offset` (<= the file's size), growing it as needed.
 * Blocks past the old end are allocated; whole-block pieces are written
 * without reading what was there. The inode's size is advanced after every
 * block, so a failure leaves a consistent (shorter) file. */
static ext2_status_t file_write_range(ext2_t *fs, ext2_inode_t *inode,
                                      size_t offset, const uint8_t *data,
                                      size_t length)
{
    size_t done = 0;
    bmap_cache_t cache = {0};
    while (done != length) {
        const size_t position = offset + done;
        const uint32_t index = (uint32_t)(position / EXT2_BLOCK_BYTES);
        const size_t within = position % EXT2_BLOCK_BYTES;
        size_t chunk = EXT2_BLOCK_BYTES - within;
        if (chunk > length - done) chunk = length - done;

        if (within == 0 && chunk == EXT2_BLOCK_BYTES) {
            enum { MAX_WRITE_BLOCKS = 16 };
            size_t max_blocks = (length - done) / EXT2_BLOCK_BYTES;
            if (max_blocks > MAX_WRITE_BLOCKS) max_blocks = MAX_WRITE_BLOCKS;

            uint32_t first = 0, run = 0;
            ext2_status_t status = bmap_get_contiguous_run(
                fs, inode, index, (uint32_t) max_blocks, &first, &run, &cache);
            if (status != EXT2_OK) return status;

            if (first != 0 && run > 1) {
                /* Existing blocks are file data, so a direct multi-block
                 * device write is safe even while metadata is journaled. */
                if (!ext2_blockdev_write(fs->device, first, data + done, run)) {
                    io_failed(fs);
                    return EXT2_IO_ERROR;
                }
                note_data_written(fs, first, run);
                const size_t bytes = (size_t) run * EXT2_BLOCK_BYTES;
                done += bytes;
                if (position + bytes > inode->size)
                    inode->size = (uint32_t) (position + bytes);
                continue;
            }

            if (first == 0) {
                size_t holes = 0;
                while (holes < max_blocks) {
                    uint32_t mapped = 0;
                    status = bmap_get_cached(fs, inode, index + holes, &mapped, &cache);
                    if (status != EXT2_OK) return status;
                    if (mapped != 0) break;
                    ++holes;
                }
                uint32_t allocated = 0;
                const uint32_t fresh = alloc_block_run(fs, (uint32_t) holes, &allocated);
                if (allocated > 1) {
                    for (uint32_t i = 0; i < allocated; ++i) {
                        status = bmap_set(fs, inode, index + i, fresh + i);
                        cache.level1 = cache.level2 = 0; /* bmap_set rewrote a table */
                        if (status != EXT2_OK) {
                            for (uint32_t k = 0; k < allocated; ++k) {
                                if (k < i) {
                                    (void) bmap_set(fs, inode, index + k, 0);
                                    inode->blocks -= EXT2_BLOCK_BYTES / 512u;
                                }
                                (void) free_block(fs, fresh + k);
                            }
                            return status;
                        }
                        inode->blocks += EXT2_BLOCK_BYTES / 512u;
                    }
                    if (!ext2_blockdev_write(fs->device, fresh, data + done, allocated)) {
                        io_failed(fs);
                        return EXT2_IO_ERROR;
                    }
                    note_data_written(fs, fresh, allocated);
                    const size_t bytes = (size_t) allocated * EXT2_BLOCK_BYTES;
                    done += bytes;
                    if (position + bytes > inode->size)
                        inode->size = (uint32_t) (position + bytes);
                    continue;
                }
            }
        }

        uint32_t block = 0;
        ext2_status_t status = bmap_get_cached(fs, inode, index, &block, &cache);
        if (status != EXT2_OK) return status;
        bool fresh = false;
        if (block == 0) {
            block = alloc_block(fs);
            if (block == UINT32_MAX) return EXT2_NO_SPACE;
            status = bmap_set(fs, inode, index, block);
            cache.level1 = cache.level2 = 0; /* bmap_set rewrote a table */
            if (status != EXT2_OK) {
                (void)free_block(fs, block);
                return status;
            }
            inode->blocks += EXT2_BLOCK_BYTES / 512u;
            fresh = true;
        }
        if (chunk == EXT2_BLOCK_BYTES) {
            /* A whole block can go straight from the caller's buffer to the
             * device (the block cache, normally); staging it through
             * io_block copied every large write twice. */
            if (!write_block(fs, block, data + done)) return EXT2_IO_ERROR;
        } else {
            if (fresh) memset(io_block, 0, sizeof(io_block));
            else if (!read_block(fs, block, io_block)) return EXT2_IO_ERROR;
            memcpy(io_block + within, data + done, chunk);
            if (!write_block(fs, block, io_block)) return EXT2_IO_ERROR;
        }
        done += chunk;
        if (position + chunk > inode->size) inode->size = (uint32_t)(position + chunk);
    }
    return EXT2_OK;
}

/* Find the file at `path` or create it. */
static ext2_status_t file_open_or_create(ext2_t *fs, const char *path,
                                         uint32_t *out_inode, bool *out_created)
{
    ext2_status_t status = ext2_lookup(fs, path, out_inode);
    *out_created = false;
    if (status == EXT2_NOT_FOUND) {
        status = create_inode(fs, path, EXT2_MODE_FILE);
        if (status != EXT2_OK) return status;
        status = ext2_lookup(fs, path, out_inode);
        if (status != EXT2_OK) return status;
        *out_created = true;
    }
    return status;
}

ext2_status_t ext2_write_file(ext2_t *fs, const char *path, const void *data,
                              size_t length)
{
    if (!fs_ready(fs) || path == NULL || (length != 0 && data == NULL)) {
        return EXT2_INVALID;
    }
    if (length > EXT2_MAX_FILE_BYTES) return EXT2_NO_SPACE;

    uint32_t inode_index;
    bool created;
    ext2_status_t status = file_open_or_create(fs, path, &inode_index, &created);
    if (status != EXT2_OK) return status;
    ext2_inode_t *inode = inode_ptr(fs, inode_index);
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode_large_file(fs, inode)) return EXT2_UNSUPPORTED;

    /* Refuse up front if the new content cannot fit once the old is freed, so
     * a failed write leaves the old file alone. */
    const uint32_t old_units = inode->blocks / (EXT2_BLOCK_BYTES / 512u);
    if (blocks_needed(0, file_blocks(length)) > fs->free_blocks + old_units) {
        if (created) (void)ext2_unlink(fs, path);
        return EXT2_NO_SPACE;
    }
    if (!free_inode_blocks(fs, inode)) return EXT2_IO_ERROR;
    inode->size = 0;
    inode->blocks = 0;
    status = file_write_range(fs, inode, 0, data, length);
    if (status != EXT2_OK) {
        if (created) (void)ext2_unlink(fs, path);
        return status;
    }
    inode->size = (uint32_t)length;
    touch_modified(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

static ext2_status_t append_inode_impl(ext2_t *fs, uint32_t inode_index,
                                        const void *data, size_t length,
                                        size_t *out_size, bool sync_now)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || (length != 0 && data == NULL)) return EXT2_INVALID;
    if (!inode_live(fs, inode_index)) return EXT2_NOT_FOUND;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode_large_file(fs, inode)) return EXT2_UNSUPPORTED;
    if (out_size != NULL) *out_size = inode->size;
    if (length == 0) return EXT2_OK;
    if (length > EXT2_MAX_FILE_BYTES - inode->size) return EXT2_NO_SPACE;
    const uint32_t have = file_blocks(inode->size);
    if (blocks_needed(have, file_blocks((size_t)inode->size + length)) >
        fs->free_blocks) {
        return EXT2_NO_SPACE;
    }
    const ext2_status_t status =
        file_write_range(fs, inode, inode->size, data, length);
    if (status != EXT2_OK) return status;
    touch_modified(inode);
    if (out_size != NULL) *out_size = inode->size;
    if (!sync_now) return EXT2_OK;
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_append_inode(ext2_t *fs, uint32_t inode_index,
                                 const void *data, size_t length,
                                 size_t *out_size)
{
    return append_inode_impl(fs, inode_index, data, length, out_size, true);
}

static ext2_status_t append_file_impl(ext2_t *fs, const char *path,
                                      const void *data, size_t length,
                                      bool sync_now)
{
    if (!fs_ready(fs) || path == NULL || (length != 0 && data == NULL)) {
        return EXT2_INVALID;
    }
    if (length == 0) {
        return ext2_touch(fs, path);
    }
    uint32_t inode_index;
    bool created;
    ext2_status_t status = file_open_or_create(fs, path, &inode_index, &created);
    if (status != EXT2_OK) return status;
    if (inode_is_dir(const_inode_ptr(fs, inode_index))) {
        if (created) (void)ext2_unlink(fs, path);
        return EXT2_IS_DIR;
    }
    status = append_inode_impl(fs, inode_index, data, length, NULL, sync_now);
    if (status != EXT2_OK && created) (void)ext2_unlink(fs, path);
    return status;
}

ext2_status_t ext2_append_file(ext2_t *fs, const char *path, const void *data,
                                size_t length)
{
    return append_file_impl(fs, path, data, length, true);
}

ext2_status_t ext2_append_file_deferred(ext2_t *fs, const char *path,
                                        const void *data, size_t length)
{
    return append_file_impl(fs, path, data, length, false);
}

ext2_status_t ext2_write_inode_at(ext2_t *fs, uint32_t inode_index,
                                  size_t offset, const void *data,
                                  size_t length)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs) || (length != 0 && data == NULL)) return EXT2_INVALID;
    if (!inode_live(fs, inode_index)) return EXT2_NOT_FOUND;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode_large_file(fs, inode)) return EXT2_UNSUPPORTED;
    if (length == 0) return EXT2_OK;
    if (offset > EXT2_MAX_FILE_BYTES || length > EXT2_MAX_FILE_BYTES - offset) {
        return EXT2_NO_SPACE;
    }

    /* A write past the end leaves the gap as a hole: the blocks are not
     * allocated and read back as zeros.  This is a sparse file, as POSIX
     * permits, and it removes the old 16 KiB gap limit.  Only the written
     * range can allocate blocks, so bound that count against free space. */
    if (offset > inode->size) {
        const size_t first = offset / EXT2_BLOCK_BYTES;
        const size_t last = (offset + length - 1) / EXT2_BLOCK_BYTES;
        if (last - first + 1 > fs->free_blocks) return EXT2_NO_SPACE;
    } else if (blocks_needed(file_blocks(inode->size),
                             file_blocks(offset + length)) > fs->free_blocks) {
        return EXT2_NO_SPACE;
    }
    const ext2_status_t status = file_write_range(fs, inode, offset, data, length);
    if (status != EXT2_OK) return status;
    touch_modified(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_write_at(ext2_t *fs, const char *path, size_t offset,
                            const void *data, size_t length)
{
    if (!fs_ready(fs) || path == NULL || (length != 0 && data == NULL)) {
        return EXT2_INVALID;
    }
    uint32_t inode_index;
    ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    if (inode_is_dir(const_inode_ptr(fs, inode_index))) return EXT2_IS_DIR;
    return ext2_write_inode_at(fs, inode_index, offset, data, length);
}

ext2_status_t ext2_truncate_inode(ext2_t *fs, uint32_t inode_index,
                                  size_t length)
{
    const ext2_status_t mutation = mutation_status(fs);
    if (mutation != EXT2_OK) return mutation;
    if (!fs_ready(fs)) return EXT2_INVALID;
    if (!inode_live(fs, inode_index)) return EXT2_NOT_FOUND;
    ext2_inode_t *const inode = inode_ptr(fs, inode_index);
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode_large_file(fs, inode)) return EXT2_UNSUPPORTED;
    const size_t size = inode->size;
    if (length == size) return EXT2_OK;
    if (length > EXT2_MAX_FILE_BYTES) return EXT2_NO_SPACE;
    if (length > size) {
        /* Growing reads as zeros; the blocks between the old end and the new
         * last block stay holes, so only the final block is allocated. */
        static const uint8_t zero = 0;
        return ext2_write_inode_at(fs, inode_index, length - 1, &zero, 1);
    }
    if (length == 0) {
        if (!free_inode_blocks(fs, inode)) return EXT2_IO_ERROR;
        inode->size = 0;
        inode->blocks = 0;
        touch_modified(inode);
        return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
    }

    /* Clear the cut-off tail of the last kept block, so growing the file again
     * reads zeros there rather than the old bytes. */
    const size_t tail = length % EXT2_BLOCK_BYTES;
    const uint32_t new_blocks = file_blocks(length);
    if (tail != 0) {
        uint32_t last = 0;
        ext2_status_t status = bmap_get(fs, inode, new_blocks - 1, &last);
        if (status != EXT2_OK) return status;
        /* A hole reads as zeros already: there is nothing to clear. */
        if (last != 0) {
            if (!read_block(fs, last, io_block)) return EXT2_IO_ERROR;
            memset(io_block + tail, 0, EXT2_BLOCK_BYTES - tail);
            if (!write_block(fs, last, io_block)) return EXT2_IO_ERROR;
        }
    }
    if (!bmap_free_from(fs, inode, new_blocks)) return EXT2_IO_ERROR;
    inode->size = (uint32_t)length;
    touch_modified(inode);
    return sync_metadata(fs) ? EXT2_OK : EXT2_IO_ERROR;
}

ext2_status_t ext2_truncate_to(ext2_t *fs, const char *path, size_t length)
{
    if (!fs_ready(fs) || path == NULL) return EXT2_INVALID;
    uint32_t inode_index;
    ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    if (inode_is_dir(const_inode_ptr(fs, inode_index))) return EXT2_IS_DIR;
    return ext2_truncate_inode(fs, inode_index, length);
}

ext2_status_t ext2_read_file(ext2_t *fs, const char *path, void *data,
                             size_t capacity, size_t *out_length)
{
    if (!fs_ready(fs) || path == NULL) return EXT2_INVALID;
    uint32_t inode_index;
    const ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    const ext2_inode_t *const inode = const_inode_ptr(fs, inode_index);
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode_large_file(fs, inode)) return EXT2_UNSUPPORTED;
    if (out_length != NULL) *out_length = inode->size;
    if (capacity < inode->size) return EXT2_NO_SPACE;
    if (inode->size != 0 && data == NULL) return EXT2_INVALID;
    size_t got = 0;
    return ext2_read_at(fs, path, 0, data, inode->size, &got);
}

ext2_status_t ext2_read_inode_at(ext2_t *fs, uint32_t inode_index,
                                 size_t offset, void *data, size_t capacity,
                                 size_t *out_read)
{
    if (out_read != NULL) *out_read = 0;
    if (!fs_ready(fs) || out_read == NULL ||
        (capacity != 0 && data == NULL)) return EXT2_INVALID;
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    if (!inode_live(fs, inode_index)) return fs->cache_io_error ? EXT2_IO_ERROR : EXT2_NOT_FOUND;
    const ext2_inode_t *const inode = const_inode_ptr(fs, inode_index);
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    if (inode_is_dir(inode)) return EXT2_IS_DIR;
    if (inode_large_file(fs, inode)) return EXT2_UNSUPPORTED;
    if (offset >= inode->size || capacity == 0) return EXT2_OK;

    size_t remaining = inode->size - offset;
    if (remaining > capacity) remaining = capacity;
    size_t copied = 0;
    bmap_cache_t cache = {0};
    while (remaining != 0) {
        const size_t position = offset + copied;
        const uint32_t index = (uint32_t)(position / EXT2_BLOCK_BYTES);
        const size_t block_offset = position % EXT2_BLOCK_BYTES;

        if (block_offset == 0 && remaining >= EXT2_BLOCK_BYTES) {
            enum { MAX_READ_BLOCKS = 16 };
            uint32_t first_block = 0, run = 0;
            size_t max_run = remaining / EXT2_BLOCK_BYTES;
            if (max_run > MAX_READ_BLOCKS) max_run = MAX_READ_BLOCKS;
            const ext2_status_t mapped = bmap_get_contiguous_run(
                fs, inode, index, (uint32_t) max_run, &first_block, &run, &cache);
            if (mapped != EXT2_OK) return mapped;
            if (first_block == 0) {
                memset((uint8_t *)data + copied, 0, EXT2_BLOCK_BYTES);
                copied += EXT2_BLOCK_BYTES;
                remaining -= EXT2_BLOCK_BYTES;
                continue;
            }
            /* File data is written straight to the device, so it is never in
             * the metadata journal overlay; one multi-block SD request serves
             * the whole contiguous run. */
            if (!ext2_blockdev_read(fs->device, first_block,
                                    (uint8_t *)data + copied, run)) {
                return EXT2_IO_ERROR;
            }
            const size_t bytes = (size_t) run * EXT2_BLOCK_BYTES;
            copied += bytes;
            remaining -= bytes;
            continue;
        }

        uint32_t block = 0;
        const ext2_status_t mapped = bmap_get_cached(fs, inode, index, &block, &cache);
        if (mapped != EXT2_OK) return mapped;
        size_t chunk = EXT2_BLOCK_BYTES - block_offset;
        if (chunk > remaining) chunk = remaining;
        if (block == 0) { /* a block that was never written reads as zeros */
            memset((uint8_t *)data + copied, 0, chunk);
        } else {
            if (!read_block(fs, block, io_block)) return EXT2_IO_ERROR;
            memcpy((uint8_t *)data + copied, io_block + block_offset, chunk);
        }
        copied += chunk;
        remaining -= chunk;
    }
    *out_read = copied;
    return EXT2_OK;
}

ext2_status_t ext2_read_at(ext2_t *fs, const char *path, size_t offset,
                           void *data, size_t capacity, size_t *out_read)
{
    if (out_read != NULL) *out_read = 0;
    if (!fs_ready(fs) || path == NULL || out_read == NULL ||
        (capacity != 0 && data == NULL)) return EXT2_INVALID;
    uint32_t inode_index;
    const ext2_status_t status = ext2_lookup(fs, path, &inode_index);
    if (status != EXT2_OK) return status;
    if (inode_is_dir(const_inode_ptr(fs, inode_index))) return EXT2_IS_DIR;
    return ext2_read_inode_at(fs, inode_index, offset, data, capacity,
                              out_read);
}

/* Return the non-dot entry inode at `index`, without copying its name.  The
 * tree release walks with this so its recursion frame stays small. */
ext2_status_t ext2_list_child(ext2_t *fs, uint32_t dir_inode,
                              unsigned index, uint32_t *out_inode)
{
    if (out_inode == NULL) return EXT2_INVALID;
    *out_inode = 0;
    if (!fs_ready(fs) || !inode_live(fs, dir_inode)) return EXT2_NOT_FOUND;
    const ext2_inode_t *const dir = const_inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir)) return EXT2_NOT_DIR;
    const unsigned first = (dir->flags & EXT2_INDEX_FL) != 0 ? 1 : 0;
    unsigned seen = 0;
    for (unsigned bi = first; bi < dir_blocks(dir); ++bi) {
        const ext2_status_t loaded = dir_load(fs, dir, bi);
        if (loaded != EXT2_OK) return bi == first ? EXT2_NOT_DIR : loaded;
        size_t offset = 0;
        while (offset < EXT2_BLOCK_BYTES) {
            const uint8_t *const entry = io_block + offset;
            const uint16_t rec_len = get_u16(entry + 4);
            if (!dirent_fits(fs, entry, rec_len, offset)) {
                return EXT2_IO_ERROR;
            }
            const uint32_t inode = get_u32(entry);
            if (inode != 0) {
                const size_t length = dirent_name_len(fs, entry);
                const bool dot = length == 1 && entry[8] == '.';
                const bool dotdot =
                    length == 2 && entry[8] == '.' && entry[9] == '.';
                if (!dot && !dotdot && seen++ == index) {
                    if (inode > fs->inode_count) return EXT2_IO_ERROR;
                    *out_inode = (uint32_t)inode;
                    return EXT2_OK;
                }
            }
            offset += rec_len;
        }
    }
    return EXT2_NOT_FOUND;
}

ext2_status_t ext2_list_inode_full(ext2_t *fs, uint32_t dir_inode,
                                   unsigned index, char *name,
                                   size_t name_size, bool *out_is_dir,
                                   uint32_t *out_ino, uint8_t *out_type)
{
    if (out_ino != NULL) *out_ino = 0;
    if (out_type != NULL) *out_type = 0;
    if (!fs_ready(fs) || name == NULL || name_size == 0) return EXT2_INVALID;
    if (fs->cache_io_error) return EXT2_IO_ERROR;
    if (!inode_live(fs, dir_inode)) return EXT2_NOT_FOUND;
    const ext2_inode_t *const dir = const_inode_ptr(fs, dir_inode);
    if (!inode_is_dir(dir)) return EXT2_NOT_DIR;

    const bool indexed = (dir->flags & EXT2_INDEX_FL) != 0;
    unsigned seen = 0;
    uint32_t bi = indexed ? 1 : 0;
    size_t offset = 0;
    const unsigned blocks = dir_blocks(dir);
    if (!indexed && dir_scan_hint.valid && dir_scan_hint.fs == fs &&
        dir_scan_hint.dir_inode == dir_inode &&
        dir_scan_hint.generation == fs->dir_generation &&
        dir_scan_hint.next_index == index) {
        seen = index;
        bi = dir_scan_hint.block;
        offset = dir_scan_hint.offset;
    }
    while (bi < blocks) {
        const ext2_status_t loaded = dir_load(fs, dir, bi);
        if (loaded != EXT2_OK) return bi == 0 ? EXT2_NOT_DIR : loaded;
        while (offset < EXT2_BLOCK_BYTES) {
            const uint8_t *const entry = io_block + offset;
            const uint16_t rec_len = get_u16(entry + 4);
            if (!dirent_fits(fs, entry, rec_len, offset)) {
                return EXT2_IO_ERROR;
            }
            const uint32_t inode = get_u32(entry);
            if (inode != 0) {
                const size_t length = dirent_name_len(fs, entry);
                const bool dot = length == 1 && entry[8] == '.';
                const bool dotdot =
                    length == 2 && entry[8] == '.' && entry[9] == '.';
                if (!dot && !dotdot) {
                    if (seen == index) {
                        size_t next_offset = offset + rec_len;
                        uint32_t next_block = bi;
                        if (next_offset >= EXT2_BLOCK_BYTES) {
                            ++next_block;
                            next_offset = 0;
                        }
                        if (!indexed) {
                            dir_scan_hint = (dir_scan_hint_t){
                                true, fs, dir_inode, fs->dir_generation,
                                index + 1, next_block, next_offset};
                        }
                        if (length + 1 > name_size) return EXT2_NAME_TOO_LONG;
                        memcpy(name, entry + 8, length);
                        name[length] = '\0';
                        const uint8_t type = dirent_type(fs, entry);
                        if (out_is_dir != NULL) {
                            *out_is_dir = type == 2;
                        }
                        if (out_ino != NULL) *out_ino = inode;
                        if (out_type != NULL) *out_type = type;
                        return EXT2_OK;
                    }
                    ++seen;
                }
            }
            offset += rec_len;
        }
        ++bi;
        offset = 0;
    }
    if (!indexed) {
        dir_scan_hint = (dir_scan_hint_t){
            true, fs, dir_inode, fs->dir_generation, index + 1, blocks, 0};
    }
    return EXT2_NOT_FOUND;
}

ext2_status_t ext2_list_inode(ext2_t *fs, uint32_t dir_inode,
                              unsigned index, char *name, size_t name_size,
                              bool *out_is_dir)
{
    return ext2_list_inode_full(fs, dir_inode, index, name, name_size,
                                out_is_dir, NULL, NULL);
}

ext2_status_t ext2_list_full(ext2_t *fs, const char *dir_path,
                             unsigned index, char *name, size_t name_size,
                             bool *out_is_dir, uint32_t *out_ino,
                             uint8_t *out_type)
{
    if (out_ino != NULL) *out_ino = 0;
    if (out_type != NULL) *out_type = 0;
    if (!fs_ready(fs) || dir_path == NULL || name == NULL || name_size == 0) {
        return EXT2_INVALID;
    }
    uint32_t dir_inode;
    ext2_status_t status = ext2_lookup(fs, dir_path, &dir_inode);
    if (status != EXT2_OK) return status;
    return ext2_list_inode_full(fs, dir_inode, index, name, name_size,
                                out_is_dir, out_ino, out_type);
}

ext2_status_t ext2_list(ext2_t *fs, const char *dir_path, unsigned index,
                        char *name, size_t name_size, bool *out_is_dir)
{
    return ext2_list_full(fs, dir_path, index, name, name_size, out_is_dir,
                          NULL, NULL);
}
