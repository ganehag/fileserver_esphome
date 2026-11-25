// SPDX-License-Identifier: MIT
#pragma once

/* Minimal ext2 interface tailored for the WebDAV Vfs backend.
 *
 * Only the parts needed by the WebDAV Vfs backend are exposed here. The
 * on-disk structures are standard little-endian ext2/JBD2 layouts. */

#include "ext2_blockdev.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  EXT2_BLOCK_BYTES = 4096u,
  EXT2_INODE_BYTES = 128u,
  EXT2_INDIRECT_ENTRIES = EXT2_BLOCK_BYTES / 4u,
  EXT2_BITMAP_BYTES = EXT2_BLOCK_BYTES,
  EXT2_BLOCKS_PER_GROUP = 32768u,
  /* With 4 KiB blocks this is the largest volume the 32-bit block count can
   * describe (~16 TiB). Bitmaps and inode tables are held in fixed-size caches,
   * so only the group descriptors scale with the volume. */
  EXT2_MAX_GROUPS = 131072u,
  /* No separate total-inode cap: the 4096-byte per-group bitmap already bounds
   * inodes_per_group. Only group descriptors scale with the volume; fixed
   * caches and the journal come from the VFS allocator (PSRAM when present). */
  EXT2_FORMAT_INODES_TOTAL = 262144u,
  EXT2_MAX_FILE_BYTES = 0x7FFFFFFFu,
  EXT2_NAME_BYTES = 256u,
  /* Full paths may be longer than one ext2 directory entry. Match the HTTP
   * layer's decoded path limit so a valid WebDAV path is not rejected by the
   * filesystem backend. */
  EXT2_PATH_BYTES = 1025u,
  EXT2_ROOT_INO = 2u,
};

typedef enum {
  EXT2_OK = 0,
  EXT2_INVALID,
  EXT2_NOT_FORMATTED,
  EXT2_NOT_FOUND,
  EXT2_IO_ERROR,
  EXT2_NO_SPACE,
  EXT2_EXISTS,
  EXT2_NOT_DIR,
  EXT2_IS_DIR,
  EXT2_NOT_EMPTY,
  EXT2_NAME_TOO_LONG,
  EXT2_UNSUPPORTED,
  EXT2_LOOP,
  EXT2_ACCESS,
  EXT2_CROSS_MOUNT,
  EXT2_READ_ONLY,
} ext2_status_t;

typedef struct {
  uint32_t inodes_count;
  uint32_t blocks_count;
  uint32_t r_blocks_count;
  uint32_t free_blocks_count;
  uint32_t free_inodes_count;
  uint32_t first_data_block;
  uint32_t log_block_size;
  uint32_t log_frag_size;
  uint32_t blocks_per_group;
  uint32_t frags_per_group;
  uint32_t inodes_per_group;
  uint32_t mtime;
  uint32_t wtime;
  uint16_t mnt_count;
  uint16_t max_mnt_count;
  uint16_t magic;
  uint16_t state;
  uint16_t errors;
  uint16_t minor_rev_level;
  uint32_t lastcheck;
  uint32_t checkinterval;
  uint32_t creator_os;
  uint32_t rev_level;
  uint16_t def_resuid;
  uint16_t def_resgid;
  uint32_t first_ino;
  uint16_t inode_size;
  uint16_t block_group_nr;
  uint32_t feature_compat;
  uint32_t feature_incompat;
  uint32_t feature_ro_compat;
  uint8_t uuid[16];
  uint8_t volume_name[16];
  uint8_t last_mounted[64];
  uint32_t algo_bitmap;
  uint8_t prealloc_blocks;
  uint8_t prealloc_dir_blocks;
  uint16_t reserved_gdt_blocks;
  uint8_t journal_uuid[16];
  uint32_t journal_inum;
  uint32_t journal_dev;
  uint32_t last_orphan;
  uint32_t hash_seed[4];
  uint8_t def_hash_version;
  uint8_t jnl_backup_type;
  uint16_t desc_size;
  uint32_t default_mount_opts;
  uint32_t first_meta_bg;
  uint32_t mkfs_time;
  uint32_t journal_blocks[17];
  uint8_t reserved[688];
} ext2_super_t;

typedef struct {
  uint16_t mode;
  uint16_t uid;
  uint32_t size;
  uint32_t atime;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t dtime;
  uint16_t gid;
  uint16_t links_count;
  uint32_t blocks;
  uint32_t flags;
  uint32_t osd1;
  uint32_t block[15];
  uint32_t generation;
  uint32_t file_acl;
  uint32_t dir_acl;
  uint32_t faddr;
  uint8_t osd2[12];
} ext2_inode_t;

typedef struct {
  uint32_t block_bitmap;
  uint32_t inode_bitmap;
  uint32_t inode_table;
  uint16_t free_blocks_count;
  uint16_t free_inodes_count;
  uint16_t used_dirs_count;
  uint16_t flags;
  uint8_t reserved[12];
} ext2_group_desc_t;

typedef struct {
  bool is_dir;
  uint32_t size;
  uint16_t mode;
  uint16_t uid;
  uint16_t gid;
  uint16_t links;
  uint32_t ino;
  uint32_t blocks;
  uint32_t atime;
  uint32_t mtime;
  uint32_t ctime;
} ext2_stat_t;

typedef struct {
  uint32_t block_size;
  uint32_t block_count;
  uint32_t free_blocks;
  uint32_t inode_count;
  uint32_t free_inodes;
} ext2_space_t;

typedef bool (*ext2_traverse_check_fn)(void *context, uint32_t inode, bool is_dir,
                                       uint16_t mode, uint16_t uid, uint16_t gid);

struct ext2_journal;
typedef struct ext2_journal ext2_journal_t;

typedef struct {
  ext2_blockdev_t *device;
  ext2_super_t super;
  ext2_group_desc_t *groups;
  /* Fixed-size LRU caches. Only group descriptors scale with the volume. */
  void *bitmap_cache;
  void *itable_cache;
  uint8_t *memory;
  ext2_journal_t *journal;

  uint32_t block_count;
  uint32_t group_count;
  uint32_t blocks_per_group;
  uint32_t inodes_per_group;
  uint32_t inode_size;
  uint32_t inodes_per_block;
  uint32_t inode_count;
  uint32_t first_ino;
  uint32_t itable_blocks_per_group;
  uint32_t itable_blocks;
  uint32_t free_blocks;
  uint32_t free_inodes;
  uint32_t dir_count;
  uint32_t alloc_hint;
  uint32_t inode_hint;
  uint32_t root_dir_block;
  uint32_t first_data_block;
  uint32_t dir_generation;
  uint32_t cache_clock;

  bool cache_io_error;
  bool mounted;
  bool read_only;
  bool has_filetype;
  bool summary_dirty;

  uint16_t create_uid;
  uint16_t create_gid;
  uint16_t create_umask;
} ext2_t;

/* Process-wide hooks. Set these before mounting. */
void ext2_set_allocator(void *(*allocate)(size_t bytes), void (*release)(void *memory));
void ext2_set_clock(uint32_t (*now)(void));

/* Formatting is a host-fixture utility (tests/make_ext2_image.c). The WebDAV
 * firmware never calls ext2_format(); it only mounts existing volumes. */
ext2_status_t ext2_format(ext2_t *fs, ext2_blockdev_t *device);
ext2_status_t ext2_mount(ext2_t *fs, ext2_blockdev_t *device);
ext2_status_t ext2_unmount(ext2_t *fs);
bool ext2_pending(const ext2_t *fs);
ext2_status_t ext2_commit(ext2_t *fs);
bool ext2_read_only(const ext2_t *fs);

ext2_status_t ext2_lookup(ext2_t *fs, const char *path, uint32_t *out_inode);
ext2_status_t ext2_lookup_nofollow(ext2_t *fs, const char *path, uint32_t *out_inode);
ext2_status_t ext2_lookup_checked(ext2_t *fs, const char *path, bool follow_final,
                                  ext2_traverse_check_fn check, void *context,
                                  uint32_t *out_inode);
ext2_status_t ext2_lookup_at(ext2_t *fs, uint32_t start_inode, const char *path,
                             uint32_t *out_inode);
ext2_status_t ext2_lookup_at_nofollow(ext2_t *fs, uint32_t start_inode, const char *path,
                                      uint32_t *out_inode);
ext2_status_t ext2_lookup_at_checked(ext2_t *fs, uint32_t start_inode, const char *path,
                                     bool follow_final, ext2_traverse_check_fn check,
                                     void *context, uint32_t *out_inode);
ext2_status_t ext2_resolve_parent_at(ext2_t *fs, uint32_t start_inode, const char *path,
                                     uint32_t *out_parent, const char **out_name);

ext2_status_t ext2_stat(ext2_t *fs, const char *path, ext2_stat_t *out_stat);
ext2_status_t ext2_lstat(ext2_t *fs, const char *path, ext2_stat_t *out_stat);
ext2_status_t ext2_stat_inode(ext2_t *fs, uint32_t inode, ext2_stat_t *out_stat);
ext2_status_t ext2_space(ext2_t *fs, ext2_space_t *out_space);

ext2_status_t ext2_list(ext2_t *fs, const char *dir_path, unsigned index, char *name,
                        size_t name_size, bool *out_is_dir);
ext2_status_t ext2_list_full(ext2_t *fs, const char *dir_path, unsigned index, char *name,
                             size_t name_size, bool *out_is_dir, uint32_t *out_ino,
                             uint8_t *out_type);
ext2_status_t ext2_list_inode(ext2_t *fs, uint32_t dir_inode, unsigned index, char *name,
                              size_t name_size, bool *out_is_dir);
ext2_status_t ext2_list_inode_full(ext2_t *fs, uint32_t dir_inode, unsigned index,
                                   char *name, size_t name_size, bool *out_is_dir,
                                   uint32_t *out_ino, uint8_t *out_type);
ext2_status_t ext2_list_child(ext2_t *fs, uint32_t dir_inode, unsigned index,
                              uint32_t *out_inode);

ext2_status_t ext2_read_file(ext2_t *fs, const char *path, void *data, size_t capacity,
                             size_t *out_length);
ext2_status_t ext2_read_at(ext2_t *fs, const char *path, size_t offset, void *data,
                           size_t capacity, size_t *out_read);
ext2_status_t ext2_read_inode_at(ext2_t *fs, uint32_t inode_index, size_t offset,
                                 void *data, size_t capacity, size_t *out_read);
ext2_status_t ext2_write_file(ext2_t *fs, const char *path, const void *data, size_t length);
ext2_status_t ext2_append_file(ext2_t *fs, const char *path, const void *data, size_t length);
/* Streaming append: leaves metadata dirty for the caller's later ext2_commit()
 * instead of syncing after every chunk. Use only when a commit is guaranteed. */
ext2_status_t ext2_append_file_deferred(ext2_t *fs, const char *path, const void *data,
                                        size_t length);
ext2_status_t ext2_append_inode(ext2_t *fs, uint32_t inode_index, const void *data,
                                size_t length, size_t *out_size);
ext2_status_t ext2_write_at(ext2_t *fs, const char *path, size_t offset, const void *data,
                            size_t length);
ext2_status_t ext2_write_inode_at(ext2_t *fs, uint32_t inode_index, size_t offset,
                                  const void *data, size_t length);
ext2_status_t ext2_truncate_to(ext2_t *fs, const char *path, size_t length);
ext2_status_t ext2_truncate_inode(ext2_t *fs, uint32_t inode_index, size_t length);

ext2_status_t ext2_mkdir(ext2_t *fs, const char *path);
ext2_status_t ext2_mkdir_mode(ext2_t *fs, const char *path, uint16_t mode);
ext2_status_t ext2_mkdir_at(ext2_t *fs, uint32_t parent, const char *name, uint16_t mode);
ext2_status_t ext2_create_at(ext2_t *fs, uint32_t parent, const char *name);
ext2_status_t ext2_touch(ext2_t *fs, const char *path);
ext2_status_t ext2_touch_mode(ext2_t *fs, const char *path, uint16_t mode);
ext2_status_t ext2_touch_at(ext2_t *fs, uint32_t parent, const char *name, uint16_t mode);
ext2_status_t ext2_mkfifo(ext2_t *fs, const char *path, uint16_t mode);
ext2_status_t ext2_mkfifo_at(ext2_t *fs, uint32_t parent, const char *name, uint16_t mode);

ext2_status_t ext2_unlink(ext2_t *fs, const char *path);
ext2_status_t ext2_unlink_retain(ext2_t *fs, const char *path);
ext2_status_t ext2_unlink_at(ext2_t *fs, uint32_t parent, const char *name);
ext2_status_t ext2_unlink_retain_at(ext2_t *fs, uint32_t parent, const char *name);
ext2_status_t ext2_release_inode(ext2_t *fs, uint32_t inode_index);
ext2_status_t ext2_free_inode_unlinked(ext2_t *fs, uint32_t inode_index);

ext2_status_t ext2_rename(ext2_t *fs, const char *old_path, const char *new_path);
ext2_status_t ext2_rename_keep_target(ext2_t *fs, const char *old_path, const char *new_path,
                                      bool keep_target);
ext2_status_t ext2_rename_at(ext2_t *fs, uint32_t old_parent, const char *old_name,
                             uint32_t new_parent, const char *new_name, bool keep_target);
ext2_status_t ext2_reparent(ext2_t *fs, uint32_t dir_inode, uint32_t new_parent);

ext2_status_t ext2_symlink(ext2_t *fs, const char *target, const char *link_path);
ext2_status_t ext2_symlink_at(ext2_t *fs, uint32_t parent, const char *name,
                              const char *target);
ext2_status_t ext2_readlink(ext2_t *fs, const char *path, char *buffer, size_t capacity,
                            size_t *out_length);
ext2_status_t ext2_link(ext2_t *fs, const char *existing_path, const char *new_path);
ext2_status_t ext2_link_at(ext2_t *fs, uint32_t target, uint32_t parent, const char *name);

ext2_status_t ext2_chmod(ext2_t *fs, const char *path, uint16_t mode);
ext2_status_t ext2_chmod_inode(ext2_t *fs, uint32_t inode_index, uint16_t mode);
ext2_status_t ext2_chown(ext2_t *fs, const char *path, uint16_t uid, uint16_t gid);
ext2_status_t ext2_chown_inode(ext2_t *fs, uint32_t inode_index, uint16_t uid, uint16_t gid);
ext2_status_t ext2_set_times(ext2_t *fs, const char *path, uint32_t atime, uint32_t mtime);
ext2_status_t ext2_set_times_inode(ext2_t *fs, uint32_t inode_index, uint32_t atime,
                                   uint32_t mtime);

#ifdef WEBDAV_FILING_HOST_TEST
/* Host-test hooks: drive the fixed caches directly to exercise eviction and
 * writeback without depending on a particular WebDAV workload. */
ext2_status_t ext2_test_set_group_bitmap_byte(ext2_t *fs, uint32_t group,
                                              size_t offset, uint8_t value);
void ext2_test_set_inode_hint(ext2_t *fs, uint32_t inode);
/* Same invalidate-and-reload sequence used after journal replay. */
ext2_status_t ext2_test_reload_metadata(ext2_t *fs);
#endif

#ifdef __cplusplus
}
#endif
