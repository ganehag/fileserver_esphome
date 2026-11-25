// SPDX-License-Identifier: MIT
#pragma once

/* Mode bits used by the extracted ext2 implementation. */

#define EXT2_S_IFMT 0xF000u
#define EXT2_S_IFDIR 0x4000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFLNK 0xA000u
#define EXT2_S_IFCHR 0x2000u
#define EXT2_S_IFBLK 0x6000u
#define EXT2_S_IFIFO 0x1000u
#define EXT2_S_IFSOCK 0xC000u

#define EXT2_S_ISDIR(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFDIR)
#define EXT2_S_ISREG(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFREG)
#define EXT2_S_ISLNK(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFLNK)
#define EXT2_S_ISCHR(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFCHR)
#define EXT2_S_ISBLK(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFBLK)
#define EXT2_S_ISFIFO(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFIFO)
#define EXT2_S_ISSOCK(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFSOCK)
