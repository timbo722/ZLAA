/*
 * romfs.h - present an extracted RomFS folder as a RomFS image.
 *
 * The game reads its RomFS (all its assets) as one block of bytes and parses the RomFS
 * format itself. The player's dump is an extracted folder, so at startup we build the
 * image's tables (header, directory and file entries, hash tables) in memory, and serve
 * file contents straight from the host files when the game reads them.
 * Format: switchbrew.org/wiki/RomFS
 */
#ifndef ROMFS_H
#define ROMFS_H

#include <stdint.h>

int romfs_open_dir(const char *host_dir);                 /* 0 on success */
uint64_t romfs_size(void);
int romfs_read(uint64_t offset, void *dst, uint64_t size); /* 0 on success */
const char *romfs_file_at(uint64_t offset);   /* host path of the file there, or NULL (for traces) */

#endif
