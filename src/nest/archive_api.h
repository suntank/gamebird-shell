#pragma once
#if __has_include(<archive.h>) && __has_include(<archive_entry.h>)
#include <archive.h>
#include <archive_entry.h>
#else
// Stable libarchive C ABI fallback, matching the shell's SQLite runtime approach.
// Appliances normally install libarchive-dev when building from source.
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
extern "C" {
struct archive;
struct archive_entry;
archive* archive_read_new();
int archive_read_support_format_zip(archive*);
int archive_read_support_filter_all(archive*);
int archive_read_open_filename(archive*, const char*, std::size_t);
int archive_read_next_header(archive*, archive_entry**);
ssize_t archive_read_data(archive*, void*, std::size_t);
int archive_read_free(archive*);
const char* archive_error_string(archive*);
const char* archive_entry_pathname(archive_entry*);
const char* archive_entry_symlink(archive_entry*);
const char* archive_entry_hardlink(archive_entry*);
mode_t archive_entry_filetype(archive_entry*);
mode_t archive_entry_perm(archive_entry*);
std::int64_t archive_entry_size(archive_entry*);
}
#define ARCHIVE_OK 0
#define ARCHIVE_EOF 1
#define AE_IFREG 0100000
#define AE_IFDIR 0040000
#endif
