#include "sxiv_archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <locale.h>

static const char *image_exts[] = {
	".jpg", ".jpeg", ".jpe", ".png", ".gif", ".webp",
	".bmp", ".tga", ".tiff", ".tif", ".ico", ".ppm",
	".pgm", ".pbm", ".pnm", ".svg", ".qnt", ".ajp"
};

bool is_image_extension(const char *path)
{
	if (!path)
		return false;
	const char *dot = strrchr(path, '.');
	if (!dot)
		return false;
	for (size_t i = 0; i < sizeof(image_exts)/sizeof(image_exts[0]); i++) {
		if (strcasecmp(dot, image_exts[i]) == 0)
			return true;
	}
	return false;
}

static const char *archive_exts[] = {
	".cbz", ".cb7", ".cbr", ".cbt", ".zip", ".7z",
	".rar", ".tar", ".gz", ".bz2", ".xz", ".zst",
	".xp3", ".int", ".arc", ".pak", ".dat", ".tgz",
	".tbz2", ".txz"
};

bool is_archive_extension(const char *path)
{
	if (!path)
		return false;
	const char *dot = strrchr(path, '.');
	if (!dot)
		return false;
	for (size_t i = 0; i < sizeof(archive_exts)/sizeof(archive_exts[0]); i++) {
		if (strcasecmp(dot, archive_exts[i]) == 0)
			return true;
	}
	return false;
}

bool is_archive_file(const char *path)
{
	struct stat st;
	if (!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 16)
		return false;

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;

	uint8_t magic[16];
	ssize_t r = read(fd, magic, sizeof(magic));
	close(fd);

	if (r < 4)
		return false;

	/* KiriKiri XP3 */
	if (r >= 11 && memcmp(magic, "XP3\r\n \n\x1a\x8b\x67\x01", 11) == 0)
		return true;

	/* CatSystem 2 INT */
	if (memcmp(magic, "KIF\0", 4) == 0)
		return true;

	/* BGI / Ethornell ARC */
	if (r >= 12 && memcmp(magic, "PackFile    ", 12) == 0)
		return true;

	/* PKZIP / CBZ */
	if (magic[0] == 'P' && magic[1] == 'K' &&
	    ((magic[2] == 3 && magic[3] == 4) || (magic[2] == 5 && magic[3] == 6) || (magic[2] == 7 && magic[3] == 8)))
		return true;

	/* 7-Zip / CB7 */
	if (r >= 6 && magic[0] == '7' && magic[1] == 'z' && magic[2] == 0xbc &&
	    magic[3] == 0xaf && magic[4] == 0x27 && magic[5] == 0x1c)
		return true;

	/* RAR / CBR */
	if (r >= 7 && magic[0] == 'R' && magic[1] == 'a' && magic[2] == 'r' && magic[3] == '!')
		return true;

	/* Fallback by known extension */
	return is_archive_extension(path);
}

/* -------------------------------------------------------------
 * Registry for global cleanup of open archives
 * ------------------------------------------------------------- */
static arch_t **open_archives = NULL;
static size_t open_arch_cnt = 0;
static size_t open_arch_cap = 0;

void archive_register(arch_t *a)
{
	if (!a)
		return;
	if (open_arch_cnt == open_arch_cap) {
		open_arch_cap = open_arch_cap == 0 ? 8 : open_arch_cap * 2;
		open_archives = realloc(open_archives, open_arch_cap * sizeof(arch_t*));
	}
	open_archives[open_arch_cnt++] = a;
}

void archive_cleanup_all(void)
{
	for (size_t i = 0; i < open_arch_cnt; i++) {
		archive_close(open_archives[i]);
	}
	free(open_archives);
	open_archives = NULL;
	open_arch_cnt = open_arch_cap = 0;
}

/* -------------------------------------------------------------
 * Destructor: archive_close
 * ------------------------------------------------------------- */
void archive_close(arch_t *a)
{
	if (!a)
		return;

	if (a->entries) {
		for (size_t i = 0; i < a->entry_cnt; i++) {
			free(a->entries[i].name);
		}
		free(a->entries);
		a->entries = NULL;
	}
	if (a->fd >= 0) {
		close(a->fd);
		a->fd = -1;
	}
	free(a->path);
	free(a);
}

/* -------------------------------------------------------------
 * Loader: KiriKiri XP3
 * ------------------------------------------------------------- */
static arch_t* loader_xp3(const char *path, int fd, uint64_t file_size)
{
	uint64_t dir_offset = 0;
	if (pread(fd, &dir_offset, 8, 11) != 8)
		return NULL;
	if (dir_offset < 11 || dir_offset >= file_size)
		return NULL;

	uint8_t header_type = 0;
	if (pread(fd, &header_type, 1, dir_offset) != 1)
		return NULL;

	uint8_t *toc = NULL;
	uint64_t toc_size = 0;

	if (header_type == 0) {
		/* Unpacked header */
		if (pread(fd, &toc_size, 8, dir_offset + 1) != 8 || toc_size > 0x10000000)
			return NULL;
		toc = malloc(toc_size);
		if (!toc || pread(fd, toc, toc_size, dir_offset + 9) != (ssize_t)toc_size) {
			free(toc);
			return NULL;
		}
	} else if (header_type == 1) {
		/* Packed with ZLIB */
		uint64_t packed_size = 0, unpacked_size = 0;
		if (pread(fd, &packed_size, 8, dir_offset + 1) != 8 ||
		    pread(fd, &unpacked_size, 8, dir_offset + 9) != 8 ||
		    packed_size > 0x10000000 || unpacked_size > 0x20000000)
			return NULL;

		uint8_t *packed_buf = malloc(packed_size);
		if (!packed_buf || pread(fd, packed_buf, packed_size, dir_offset + 17) != (ssize_t)packed_size) {
			free(packed_buf);
			return NULL;
		}

		toc = malloc(unpacked_size);
		if (!toc) {
			free(packed_buf);
			return NULL;
		}

		uLongf dest_len = unpacked_size;
		if (uncompress(toc, &dest_len, packed_buf, packed_size) != Z_OK) {
			free(packed_buf);
			free(toc);
			return NULL;
		}
		free(packed_buf);
		toc_size = dest_len;
	} else {
		return NULL;
	}

	arch_t *a = calloc(1, sizeof(arch_t));
	a->path = strdup(path);
	a->type = ARCH_TYPE_XP3;
	a->fd = fd;

	size_t cap = 64;
	a->entries = calloc(cap, sizeof(arch_entry_t));

	/* Parse chunks: 4 bytes magic, 8 bytes size */
	uint64_t pos = 0;
	while (pos + 12 <= toc_size) {
		uint32_t chunk_magic = *(uint32_t*)(toc + pos);
		uint64_t chunk_size = *(uint64_t*)(toc + pos + 4);
		pos += 12;

		if (pos + chunk_size > toc_size)
			break;

		if (chunk_magic == 0x656C6946) { /* "File" */
			uint64_t subpos = pos;
			uint64_t file_end = pos + chunk_size;
			char *entry_name = NULL;
			uint64_t uncomp_size = 0, comp_size = 0, offset = 0;
			bool is_comp = false;

			while (subpos + 12 <= file_end) {
				uint32_t sub_magic = *(uint32_t*)(toc + subpos);
				uint64_t sub_size = *(uint64_t*)(toc + subpos + 4);
				subpos += 12;

				if (subpos + sub_size > file_end)
					break;

				if (sub_magic == 0x6F666E69) { /* "info" */
					if (sub_size >= 22) {
						uncomp_size = *(uint64_t*)(toc + subpos + 4);
						comp_size = *(uint64_t*)(toc + subpos + 12);
						uint16_t name_len = *(uint16_t*)(toc + subpos + 20);
						if (subpos + 22 + name_len * 2 <= file_end) {
							/* Convert UTF-16LE to UTF-8 */
							const uint16_t *wstr = (const uint16_t*)(toc + subpos + 22);
							char *utf8 = malloc(name_len * 3 + 1);
							size_t uidx = 0;
							for (size_t wi = 0; wi < name_len; wi++) {
								uint16_t wc = wstr[wi];
								if (wc < 0x80) {
									utf8[uidx++] = (char)wc;
								} else if (wc < 0x800) {
									utf8[uidx++] = 0xC0 | (wc >> 6);
									utf8[uidx++] = 0x80 | (wc & 0x3F);
								} else {
									utf8[uidx++] = 0xE0 | (wc >> 12);
									utf8[uidx++] = 0x80 | ((wc >> 6) & 0x3F);
									utf8[uidx++] = 0x80 | (wc & 0x3F);
								}
							}
							utf8[uidx] = '\0';
							entry_name = utf8;
						}
					}
				} else if (sub_magic == 0x6D676573) { /* "segm" */
					if (sub_size >= 28) {
						is_comp = (*(uint32_t*)(toc + subpos) != 0);
						offset = *(uint64_t*)(toc + subpos + 4);
						uncomp_size = *(uint64_t*)(toc + subpos + 12);
						comp_size = *(uint64_t*)(toc + subpos + 20);
					}
				}
				subpos += sub_size;
			}

			if (entry_name) {
				if (a->entry_cnt == cap) {
					cap *= 2;
					a->entries = realloc(a->entries, cap * sizeof(arch_entry_t));
				}
				arch_entry_t *e = &a->entries[a->entry_cnt++];
				e->name = entry_name;
				e->offset = offset;
				e->comp_size = comp_size;
				e->uncomp_size = uncomp_size;
				e->is_compressed = is_comp;
				e->is_image = is_image_extension(entry_name);
				if (e->is_image)
					a->image_cnt++;
			}
		}
		pos += chunk_size;
	}

	free(toc);
	return a;
}

/* -------------------------------------------------------------
 * Loader: CatSystem 2 INT
 * ------------------------------------------------------------- */
static arch_t* loader_int(const char *path, int fd, uint64_t file_size)
{
	uint32_t count = 0;
	if (pread(fd, &count, 4, 4) != 4 || count == 0 || count > 100000)
		return NULL;

	/* Try 32-byte names (entry = 40 bytes) and 64-byte names (entry = 72 bytes) */
	size_t name_lens[2] = { 32, 64 };
	for (int try_idx = 0; try_idx < 2; try_idx++) {
		size_t nlen = name_lens[try_idx];
		size_t entry_len = nlen + 8;
		size_t toc_len = count * entry_len;
		if (8 + toc_len > file_size)
			continue;

		uint8_t *toc = malloc(toc_len);
		if (!toc || pread(fd, toc, toc_len, 8) != (ssize_t)toc_len) {
			free(toc);
			continue;
		}

		/* Validate first entry */
		uint32_t first_offset = *(uint32_t*)(toc + nlen);
		uint32_t first_size = *(uint32_t*)(toc + nlen + 4);
		if (first_offset < 8 + toc_len || first_offset + first_size > file_size) {
			free(toc);
			continue;
		}

		arch_t *a = calloc(1, sizeof(arch_t));
		a->path = strdup(path);
		a->type = ARCH_TYPE_INT;
		a->fd = fd;
		a->entries = calloc(count, sizeof(arch_entry_t));
		a->entry_cnt = count;

		for (size_t i = 0; i < count; i++) {
			uint8_t *p = toc + i * entry_len;
			char name_buf[65];
			memcpy(name_buf, p, nlen);
			name_buf[nlen] = '\0';

			arch_entry_t *e = &a->entries[i];
			e->name = strdup(name_buf);
			e->offset = *(uint32_t*)(p + nlen);
			e->uncomp_size = *(uint32_t*)(p + nlen + 4);
			e->comp_size = e->uncomp_size;
			e->is_compressed = false;
			e->is_image = is_image_extension(e->name);
			if (e->is_image)
				a->image_cnt++;
		}
		free(toc);
		return a;
	}
	return NULL;
}

/* -------------------------------------------------------------
 * Loader: BGI / Ethornell ARC
 * ------------------------------------------------------------- */
static arch_t* loader_bgi(const char *path, int fd, uint64_t file_size)
{
	uint32_t count = 0;
	if (pread(fd, &count, 4, 12) != 4 || count == 0 || count > 100000)
		return NULL;

	uint64_t index_size = (uint64_t)count * 32;
	uint64_t base_offset = 16 + index_size;
	if (base_offset > file_size)
		return NULL;

	uint8_t *toc = malloc(index_size);
	if (!toc || pread(fd, toc, index_size, 16) != (ssize_t)index_size) {
		free(toc);
		return NULL;
	}

	arch_t *a = calloc(1, sizeof(arch_t));
	a->path = strdup(path);
	a->type = ARCH_TYPE_BGI;
	a->fd = fd;
	a->entries = calloc(count, sizeof(arch_entry_t));
	a->entry_cnt = count;

	for (size_t i = 0; i < count; i++) {
		uint8_t *p = toc + i * 32;
		char name_buf[17];
		memcpy(name_buf, p, 16);
		name_buf[16] = '\0';

		uint32_t rel_offset = *(uint32_t*)(p + 16);
		uint32_t size = *(uint32_t*)(p + 20);

		arch_entry_t *e = &a->entries[i];
		e->name = strdup(name_buf);
		e->offset = base_offset + rel_offset;
		e->uncomp_size = size;
		e->comp_size = size;
		e->is_compressed = false;
		e->is_image = is_image_extension(e->name);
		if (e->is_image)
			a->image_cnt++;
	}

	free(toc);
	return a;
}

/* -------------------------------------------------------------
 * Loader: Libarchive (CBZ, CB7, CBR, ZIP, 7Z, TAR, etc.)
 * ------------------------------------------------------------- */
static arch_t* loader_libarchive(const char *path)
{
	setlocale(LC_ALL, "");
	struct archive *la = archive_read_new();
	archive_read_support_filter_all(la);
	archive_read_support_format_all(la);

	int open_res = archive_read_open_filename(la, path, 65536);
	if (open_res != ARCHIVE_OK && open_res != ARCHIVE_WARN) {
		archive_read_free(la);
		return NULL;
	}

	size_t cap = 64;
	arch_entry_t *entries = calloc(cap, sizeof(arch_entry_t));
	size_t count = 0;
	size_t img_count = 0;

	struct archive_entry *ae;
	int r;
	while ((r = archive_read_next_header(la, &ae)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
		if (archive_entry_filetype(ae) != AE_IFREG)
			continue;

		const char *pathname = archive_entry_pathname_utf8(ae);
		if (!pathname || pathname[0] == '\0')
			pathname = archive_entry_pathname(ae);
		if (!pathname || pathname[0] == '\0')
			continue;

		if (count == cap) {
			cap *= 2;
			entries = realloc(entries, cap * sizeof(arch_entry_t));
		}
		arch_entry_t *e = &entries[count++];
		e->name = strdup(pathname);
		e->uncomp_size = archive_entry_size(ae);
		e->comp_size = e->uncomp_size;
		e->offset = 0;
		e->is_compressed = false;
		e->is_image = is_image_extension(pathname);
		if (e->is_image)
			img_count++;
	}

	archive_read_free(la);

	if (count == 0) {
		free(entries);
		return NULL;
	}

	arch_t *a = calloc(1, sizeof(arch_t));
	a->path = strdup(path);
	a->type = ARCH_TYPE_LIBARCHIVE;
	a->fd = -1;
	a->entries = entries;
	a->entry_cnt = count;
	a->image_cnt = img_count;

	return a;
}

/* -------------------------------------------------------------
 * Main Archive Open Entrypoint
 * ------------------------------------------------------------- */
arch_t* archive_open(const char *path)
{
	setlocale(LC_ALL, "");
	struct stat st;
	if (!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 16)
		return NULL;

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	uint8_t magic[16];
	ssize_t r = read(fd, magic, sizeof(magic));

	/* Check custom VN formats first */
	if (r >= 11 && memcmp(magic, "XP3\r\n \n\x1a\x8b\x67\x01", 11) == 0) {
		arch_t *a = loader_xp3(path, fd, st.st_size);
		if (a) return a;
	}

	if (r >= 4 && memcmp(magic, "KIF\0", 4) == 0) {
		arch_t *a = loader_int(path, fd, st.st_size);
		if (a) return a;
	}

	if (r >= 12 && memcmp(magic, "PackFile    ", 12) == 0) {
		arch_t *a = loader_bgi(path, fd, st.st_size);
		if (a) return a;
	}

	close(fd);

	/* Try libarchive for standard and comic formats */
	arch_t *a = loader_libarchive(path);
	return a;
}

/* -------------------------------------------------------------
 * Entry Extractor: archive_extract_entry
 * ------------------------------------------------------------- */
int archive_extract_entry(arch_t *a, size_t entry_idx, void **out_buf, size_t *out_size)
{
	if (!a || entry_idx >= a->entry_cnt || !out_buf || !out_size)
		return -1;

	arch_entry_t *e = &a->entries[entry_idx];

	if (a->type == ARCH_TYPE_LIBARCHIVE) {
		setlocale(LC_ALL, "");
		struct archive *la = archive_read_new();
		archive_read_support_filter_all(la);
		archive_read_support_format_all(la);

		int open_res = archive_read_open_filename(la, a->path, 65536);
		if (open_res != ARCHIVE_OK && open_res != ARCHIVE_WARN) {
			archive_read_free(la);
			return -1;
		}

		struct archive_entry *ae;
		int res = -1;
		int r;
		while ((r = archive_read_next_header(la, &ae)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
			const char *pname = archive_entry_pathname_utf8(ae);
			if (!pname || pname[0] == '\0')
				pname = archive_entry_pathname(ae);
			if (pname && strcmp(pname, e->name) == 0) {
				int64_t esz = archive_entry_size(ae);
				if (esz > 0) {
					void *buf = malloc(esz);
					if (buf) {
						la_ssize_t rd = archive_read_data(la, buf, esz);
						if (rd == esz) {
							*out_buf = buf;
							*out_size = esz;
							res = 0;
						} else {
							free(buf);
						}
					}
				} else {
					/* Size unknown: dynamic chunk reading */
					size_t cap = 65536, tot = 0;
					char *buf = malloc(cap);
					while (1) {
						la_ssize_t rd = archive_read_data(la, buf + tot, cap - tot);
						if (rd <= 0) break;
						tot += rd;
						if (tot == cap) {
							cap *= 2;
							buf = realloc(buf, cap);
						}
					}
					*out_buf = buf;
					*out_size = tot;
					res = 0;
				}
				break;
			}
		}
		archive_read_free(la);
		return res;
	}

	if (a->type == ARCH_TYPE_XP3) {
		if (a->fd < 0)
			return -1;

		if (e->is_compressed) {
			uint8_t *comp_buf = malloc(e->comp_size);
			if (!comp_buf)
				return -1;
			if (pread(a->fd, comp_buf, e->comp_size, e->offset) != (ssize_t)e->comp_size) {
				free(comp_buf);
				return -1;
			}
			uint8_t *decomp_buf = malloc(e->uncomp_size);
			if (!decomp_buf) {
				free(comp_buf);
				return -1;
			}
			uLongf dest_len = e->uncomp_size;
			if (uncompress(decomp_buf, &dest_len, comp_buf, e->comp_size) != Z_OK) {
				free(comp_buf);
				free(decomp_buf);
				return -1;
			}
			free(comp_buf);
			*out_buf = decomp_buf;
			*out_size = dest_len;
			return 0;
		} else {
			uint8_t *buf = malloc(e->uncomp_size);
			if (!buf)
				return -1;
			if (pread(a->fd, buf, e->uncomp_size, e->offset) != (ssize_t)e->uncomp_size) {
				free(buf);
				return -1;
			}
			*out_buf = buf;
			*out_size = e->uncomp_size;
			return 0;
		}
	}

	if (a->type == ARCH_TYPE_INT || a->type == ARCH_TYPE_BGI) {
		if (a->fd < 0)
			return -1;
		uint8_t *buf = malloc(e->uncomp_size);
		if (!buf)
			return -1;
		if (pread(a->fd, buf, e->uncomp_size, e->offset) != (ssize_t)e->uncomp_size) {
			free(buf);
			return -1;
		}
		*out_buf = buf;
		*out_size = e->uncomp_size;
		return 0;
	}

	return -1;
}
