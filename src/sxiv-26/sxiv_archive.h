#ifndef SXIV_ARCHIVE_H
#define SXIV_ARCHIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct arch;

typedef struct arch_entry {
	char *name;              /* Relative name / path inside archive */
	uint64_t offset;         /* Absolute file offset for custom loaders */
	uint64_t comp_size;      /* Compressed size */
	uint64_t uncomp_size;    /* Uncompressed size */
	bool is_compressed;      /* Whether the entry payload is compressed */
	bool is_image;           /* True if entry is an image */
} arch_entry_t;

typedef enum {
	ARCH_TYPE_UNKNOWN = 0,
	ARCH_TYPE_LIBARCHIVE,   /* CBZ, CB7, CBR, CBT, ZIP, 7Z, TAR, etc. */
	ARCH_TYPE_XP3,          /* KiriKiri XP3 */
	ARCH_TYPE_INT,          /* CatSystem 2 INT */
	ARCH_TYPE_BGI,          /* BGI / Ethornell ARC */
} arch_type_t;

typedef struct arch {
	char *path;              /* Canonical file path on disk */
	arch_type_t type;
	arch_entry_t *entries;
	size_t entry_cnt;
	size_t image_cnt;
	int fd;                  /* Open file descriptor */
	void *priv;              /* Backend-specific context */
} arch_t;

/* File identification helpers */
bool is_image_extension(const char *path);
bool is_archive_extension(const char *path);
bool is_archive_file(const char *path);

/* Loader and Destructor APIs */
arch_t* archive_open(const char *path);
void archive_close(arch_t *a);

/* Extract entry data to memory buffer (caller must free *out_buf) */
int archive_extract_entry(arch_t *a, size_t entry_idx, void **out_buf, size_t *out_size);

/* Global registry for safe lifecycle management */
void archive_register(arch_t *a);
void archive_cleanup_all(void);

#endif /* SXIV_ARCHIVE_H */
