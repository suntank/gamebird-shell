#pragma once

#include <cstdint>

extern "C" {

struct sqlite3;
struct sqlite3_stmt;

int sqlite3_open_v2(const char* filename,
                    sqlite3** ppDb,
                    int flags,
                    const char* zVfs);
int sqlite3_close(sqlite3*);
const char* sqlite3_errmsg(sqlite3*);
int sqlite3_exec(sqlite3*,
                 const char* sql,
                 int (*callback)(void*, int, char**, char**),
                 void* arg,
                 char** errmsg);
void sqlite3_free(void*);

int sqlite3_prepare_v2(sqlite3* db,
                       const char* zSql,
                       int nByte,
                       sqlite3_stmt** ppStmt,
                       const char** pzTail);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_finalize(sqlite3_stmt* pStmt);
int sqlite3_reset(sqlite3_stmt* pStmt);

int sqlite3_bind_int(sqlite3_stmt*, int, int);
int sqlite3_bind_int64(sqlite3_stmt*, int, std::int64_t);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int sqlite3_bind_null(sqlite3_stmt*, int);

int sqlite3_column_int(sqlite3_stmt*, int iCol);
std::int64_t sqlite3_column_int64(sqlite3_stmt*, int iCol);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int iCol);

}  // extern "C"

namespace gb::db {

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
constexpr int SQLITE_OPEN_FULLMUTEX = 0x00010000;
#define SQLITE_TRANSIENT ((void (*)(void*)) - 1)

}  // namespace gb::db
