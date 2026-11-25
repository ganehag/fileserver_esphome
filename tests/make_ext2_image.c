// SPDX-License-Identifier: MIT
/* Test-fixture only: formats an ext2 image on the host. The WebDAV component
 * itself never formats storage. */

#include "ext2_fs.h"
#include "ext2_ram_blockdev.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: make_ext2_image IMAGE BYTE_COUNT\n");
    return 2;
  }

  char *end = NULL;
  const unsigned long long bytes = strtoull(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0' || bytes == 0 || bytes % EXT2_BLOCK_BYTES != 0) {
    fprintf(stderr, "byte count must be a nonzero multiple of %u\n", EXT2_BLOCK_BYTES);
    return 2;
  }

  uint8_t *const storage = malloc((size_t) bytes);
  if (storage == NULL) {
    fprintf(stderr, "cannot allocate %llu bytes\n", bytes);
    return 1;
  }

  ext2_ram_blockdev_t device;
  if (!ext2_ram_blockdev_init(&device, storage, (size_t) bytes, EXT2_BLOCK_BYTES)) {
    fprintf(stderr, "cannot initialize RAM block device\n");
    free(storage);
    return 1;
  }

  ext2_t fs;
  const ext2_status_t status = ext2_format(&fs, &device.device);
  if (status != EXT2_OK) {
    fprintf(stderr, "ext2_format failed with status %d\n", (int) status);
    free(storage);
    return 1;
  }

  FILE *const out = fopen(argv[1], "wb");
  if (out == NULL) {
    perror(argv[1]);
    free(storage);
    return 1;
  }
  const size_t written = fwrite(storage, 1, (size_t) bytes, out);
  const int close_status = fclose(out);
  free(storage);
  if (written != (size_t) bytes || close_status != 0) {
    fprintf(stderr, "failed to write %s\n", argv[1]);
    return 1;
  }
  return 0;
}
