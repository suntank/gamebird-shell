#include "db/db.h"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

#include "core/logging.h"

namespace gb::db {

namespace {

class Statement final {
 public:
  Statement(sqlite3* db, const std::string& sql) : db_(db) {
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
    ok_ = (rc == SQLITE_OK);
  }

  ~Statement() {
    if (stmt_) {
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }

  [[nodiscard]] bool Ok() const { return ok_ && stmt_ != nullptr; }
  [[nodiscard]] sqlite3_stmt* Get() const { return stmt_; }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
  bool ok_ = false;
};

std::string Join(const std::vector<std::string>& lines) {
  std::ostringstream oss;
  for (const auto& line : lines) {
    oss << line << '\n';
  }
  return oss.str();
}

std::string SchemaSql() {
  return Join({
      "CREATE TABLE IF NOT EXISTS systems (",
      "  id TEXT PRIMARY KEY,",
      "  name TEXT NOT NULL,",
      "  rom_extensions TEXT NOT NULL,",
      "  launch_type TEXT NOT NULL,",
      "  launch_template TEXT NOT NULL",
      ");",
      "",
      "CREATE TABLE IF NOT EXISTS games (",
      "  id INTEGER PRIMARY KEY,",
      "  system_id TEXT NOT NULL REFERENCES systems(id),",
      "  path TEXT NOT NULL UNIQUE,",
      "  filename TEXT NOT NULL,",
      "  title TEXT NOT NULL,",
      "  sort_title TEXT NOT NULL,",
      "  size_bytes INTEGER NOT NULL,",
      "  mtime INTEGER NOT NULL,",
      "  sha1 TEXT,",
      "  is_favorite INTEGER NOT NULL DEFAULT 0,",
      "  is_hidden INTEGER NOT NULL DEFAULT 0,",
      "  last_played INTEGER,",
      "  is_present INTEGER NOT NULL DEFAULT 1",
      ");",
      "",
      "CREATE INDEX IF NOT EXISTS idx_games_system ON games(system_id);",
      "CREATE INDEX IF NOT EXISTS idx_games_present ON games(is_present);",
      "",
      "CREATE TABLE IF NOT EXISTS game_metadata (",
      "  game_id INTEGER PRIMARY KEY REFERENCES games(id),",
      "  release_year INTEGER,",
      "  publisher TEXT,",
      "  developer TEXT,",
      "  genre TEXT,",
      "  players INTEGER,",
      "  description TEXT,",
      "  source TEXT,",
      "  source_id TEXT,",
      "  updated_at INTEGER",
      ");",
      "",
      "CREATE TABLE IF NOT EXISTS assets (",
      "  game_id INTEGER PRIMARY KEY REFERENCES games(id),",
      "  box_art_path TEXT,",
      "  thumb_path TEXT,",
      "  screenshot_path TEXT,",
      "  updated_at INTEGER",
      ");",
      "",
      "CREATE TABLE IF NOT EXISTS jobs (",
      "  id INTEGER PRIMARY KEY,",
      "  type TEXT NOT NULL,",
      "  status TEXT NOT NULL,",
      "  payload_json TEXT NOT NULL,",
      "  error TEXT,",
      "  created_at INTEGER NOT NULL,",
      "  updated_at INTEGER NOT NULL",
      ");",
      "",
      "CREATE TABLE IF NOT EXISTS launch_overrides (",
      "  scope_type TEXT NOT NULL,",
      "  scope_id TEXT NOT NULL,",
      "  core_path TEXT,",
      "  audio_latency INTEGER,",
      "  video_width INTEGER,",
      "  video_height INTEGER,",
      "  PRIMARY KEY(scope_type, scope_id)",
      ");",
      "",
      "CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status);",
      "CREATE INDEX IF NOT EXISTS idx_launch_overrides_scope ON launch_overrides(scope_type,scope_id);",
  });
}

std::int64_t NowUnixSeconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

}  // namespace

Database::~Database() { Close(); }

bool Database::Open(const std::string& db_path) {
  Close();

  db_path_ = db_path;
  std::error_code ec;
  const auto parent = std::filesystem::path(db_path_).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }

  const int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK || !db_) {
    SetError("sqlite open failed for " + db_path_);
    Close();
    return false;
  }

  if (!Exec("PRAGMA foreign_keys=ON;")) {
    Close();
    return false;
  }
  if (!Exec("PRAGMA busy_timeout=3000;")) {
    Close();
    return false;
  }

  return true;
}

void Database::Close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool Database::InitSchema() {
  if (!db_) {
    SetError("database is not open");
    return false;
  }

  if (!Exec(SchemaSql())) {
    return false;
  }

  if (!EnsureGamesPresenceColumn()) {
    return false;
  }

  Exec("PRAGMA user_version=1;");
  return true;
}

bool Database::EnsureGamesPresenceColumn() {
  Statement stmt(db_, "PRAGMA table_info(games);");
  if (!stmt.Ok()) {
    SetError("failed to query games schema");
    return false;
  }

  bool has_is_present = false;
  while (sqlite3_step(stmt.Get()) == SQLITE_ROW) {
    const auto* name_txt = sqlite3_column_text(stmt.Get(), 1);
    const std::string name =
        name_txt ? reinterpret_cast<const char*>(name_txt) : "";
    if (name == "is_present") {
      has_is_present = true;
      break;
    }
  }

  if (!has_is_present) {
    if (!Exec("ALTER TABLE games ADD COLUMN is_present INTEGER NOT NULL DEFAULT 1;")) {
      return false;
    }
  }

  return true;
}

bool Database::BeginIncrementalScan() {
  if (!Exec("BEGIN IMMEDIATE TRANSACTION;")) {
    return false;
  }
  if (!Exec("UPDATE games SET is_present=0;")) {
    Exec("ROLLBACK;");
    return false;
  }
  return true;
}

bool Database::UpsertSystem(const SystemRecord& system) {
  static const std::string kSql =
      "INSERT INTO systems(id,name,rom_extensions,launch_type,launch_template) "
      "VALUES(?,?,?,?,?) "
      "ON CONFLICT(id) DO UPDATE SET "
      "name=excluded.name, "
      "rom_extensions=excluded.rom_extensions, "
      "launch_type=excluded.launch_type, "
      "launch_template=excluded.launch_template;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare UpsertSystem");
    return false;
  }

  sqlite3_bind_text(stmt.Get(), 1, system.id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 2, system.name.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 3, system.rom_extensions.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 4, system.launch_type.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 5, system.launch_template.c_str(), -1,
                    SQLITE_TRANSIENT);

  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute UpsertSystem");
    return false;
  }

  return true;
}

bool Database::UpsertGame(const GameRecord& game) {
  static const std::string kSql =
      "INSERT INTO games(system_id,path,filename,title,sort_title,size_bytes,mtime,is_present) "
      "VALUES(?,?,?,?,?,?,?,1) "
      "ON CONFLICT(path) DO UPDATE SET "
      "system_id=excluded.system_id, "
      "filename=excluded.filename, "
      "title=excluded.title, "
      "sort_title=excluded.sort_title, "
      "size_bytes=excluded.size_bytes, "
      "mtime=excluded.mtime, "
      "is_present=1;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare UpsertGame");
    return false;
  }

  sqlite3_bind_text(stmt.Get(), 1, game.system_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 2, game.path.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 3, game.filename.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 4, game.title.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 5, game.sort_title.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.Get(), 6, game.size_bytes);
  sqlite3_bind_int64(stmt.Get(), 7, game.mtime);

  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute UpsertGame");
    return false;
  }

  return true;
}

bool Database::EndIncrementalScan(const bool hide_missing) {
  if (hide_missing) {
    if (!Exec("UPDATE games SET is_hidden=1 WHERE is_present=0;")) {
      Exec("ROLLBACK;");
      return false;
    }
  }

  if (!Exec("COMMIT;")) {
    Exec("ROLLBACK;");
    return false;
  }
  return true;
}

bool Database::ListSystems(std::vector<SystemSummary>& out) {
  out.clear();

  static const std::string kSql =
      "SELECT s.id, s.name, "
      "COALESCE(SUM(CASE WHEN g.is_present=1 AND g.is_hidden=0 THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN g.is_present=1 AND g.is_hidden=0 AND g.is_favorite=1 "
      "THEN 1 ELSE 0 END), 0) "
      "FROM systems s "
      "LEFT JOIN games g ON g.system_id = s.id "
      "GROUP BY s.id, s.name "
      "ORDER BY s.name COLLATE NOCASE;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare ListSystems");
    return false;
  }

  while (sqlite3_step(stmt.Get()) == SQLITE_ROW) {
    const auto* id_txt = sqlite3_column_text(stmt.Get(), 0);
    const auto* name_txt = sqlite3_column_text(stmt.Get(), 1);
    SystemSummary row;
    row.id = id_txt ? reinterpret_cast<const char*>(id_txt) : "";
    row.name = name_txt ? reinterpret_cast<const char*>(name_txt) : "";
    row.game_count = sqlite3_column_int(stmt.Get(), 2);
    row.favorite_count = sqlite3_column_int(stmt.Get(), 3);
    out.push_back(std::move(row));
  }

  return true;
}

bool Database::ListGamesBySystem(const std::string& system_id,
                                 const bool include_hidden,
                                 std::vector<GameSummary>& out) {
  out.clear();

  const std::string sql =
      include_hidden
          ? "SELECT id, title, filename, is_favorite, is_hidden "
            "FROM games WHERE system_id=?1 AND is_present=1 "
            "ORDER BY sort_title COLLATE NOCASE;"
          : "SELECT id, title, filename, is_favorite, is_hidden "
            "FROM games WHERE system_id=?1 AND is_present=1 AND is_hidden=0 "
            "ORDER BY sort_title COLLATE NOCASE;";

  Statement stmt(db_, sql);
  if (!stmt.Ok()) {
    SetError("failed to prepare ListGamesBySystem");
    return false;
  }

  sqlite3_bind_text(stmt.Get(), 1, system_id.c_str(), -1, SQLITE_TRANSIENT);

  while (sqlite3_step(stmt.Get()) == SQLITE_ROW) {
    const auto* title_txt = sqlite3_column_text(stmt.Get(), 1);
    const auto* file_txt = sqlite3_column_text(stmt.Get(), 2);
    GameSummary row;
    row.id = sqlite3_column_int(stmt.Get(), 0);
    row.title = title_txt ? reinterpret_cast<const char*>(title_txt) : "";
    row.filename = file_txt ? reinterpret_cast<const char*>(file_txt) : "";
    row.is_favorite = sqlite3_column_int(stmt.Get(), 3) != 0;
    row.is_hidden = sqlite3_column_int(stmt.Get(), 4) != 0;
    out.push_back(std::move(row));
  }

  return true;
}

bool Database::ToggleGameFavorite(const int game_id, bool& new_value) {
  static const std::string kUpdateSql =
      "UPDATE games SET is_favorite=CASE WHEN is_favorite=0 THEN 1 ELSE 0 END "
      "WHERE id=?1;";
  static const std::string kSelectSql =
      "SELECT is_favorite FROM games WHERE id=?1;";

  Statement update(db_, kUpdateSql);
  if (!update.Ok()) {
    SetError("failed to prepare ToggleGameFavorite update");
    return false;
  }
  sqlite3_bind_int(update.Get(), 1, game_id);
  if (sqlite3_step(update.Get()) != SQLITE_DONE) {
    SetError("failed to execute ToggleGameFavorite update");
    return false;
  }

  Statement select(db_, kSelectSql);
  if (!select.Ok()) {
    SetError("failed to prepare ToggleGameFavorite select");
    return false;
  }
  sqlite3_bind_int(select.Get(), 1, game_id);
  if (sqlite3_step(select.Get()) != SQLITE_ROW) {
    SetError("failed to fetch ToggleGameFavorite value");
    return false;
  }
  new_value = sqlite3_column_int(select.Get(), 0) != 0;
  return true;
}

bool Database::ToggleGameHidden(const int game_id, bool& new_value) {
  static const std::string kUpdateSql =
      "UPDATE games SET is_hidden=CASE WHEN is_hidden=0 THEN 1 ELSE 0 END "
      "WHERE id=?1;";
  static const std::string kSelectSql =
      "SELECT is_hidden FROM games WHERE id=?1;";

  Statement update(db_, kUpdateSql);
  if (!update.Ok()) {
    SetError("failed to prepare ToggleGameHidden update");
    return false;
  }
  sqlite3_bind_int(update.Get(), 1, game_id);
  if (sqlite3_step(update.Get()) != SQLITE_DONE) {
    SetError("failed to execute ToggleGameHidden update");
    return false;
  }

  Statement select(db_, kSelectSql);
  if (!select.Ok()) {
    SetError("failed to prepare ToggleGameHidden select");
    return false;
  }
  sqlite3_bind_int(select.Get(), 1, game_id);
  if (sqlite3_step(select.Get()) != SQLITE_ROW) {
    SetError("failed to fetch ToggleGameHidden value");
    return false;
  }
  new_value = sqlite3_column_int(select.Get(), 0) != 0;
  return true;
}

bool Database::GetLaunchInfo(const int game_id, LaunchInfo& out) {
  static const std::string kSql =
      "SELECT g.id, g.system_id, g.title, g.path, s.launch_type, s.launch_template "
      "FROM games g "
      "JOIN systems s ON s.id = g.system_id "
      "WHERE g.id=?1 AND g.is_present=1;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare GetLaunchInfo");
    return false;
  }

  sqlite3_bind_int(stmt.Get(), 1, game_id);
  if (sqlite3_step(stmt.Get()) != SQLITE_ROW) {
    SetError("launch target not found");
    return false;
  }

  const auto* sys_txt = sqlite3_column_text(stmt.Get(), 1);
  const auto* title_txt = sqlite3_column_text(stmt.Get(), 2);
  const auto* path_txt = sqlite3_column_text(stmt.Get(), 3);
  const auto* type_txt = sqlite3_column_text(stmt.Get(), 4);
  const auto* tmpl_txt = sqlite3_column_text(stmt.Get(), 5);

  out.game_id = sqlite3_column_int(stmt.Get(), 0);
  out.system_id = sys_txt ? reinterpret_cast<const char*>(sys_txt) : "";
  out.title = title_txt ? reinterpret_cast<const char*>(title_txt) : "";
  out.rom_path = path_txt ? reinterpret_cast<const char*>(path_txt) : "";
  out.launch_type = type_txt ? reinterpret_cast<const char*>(type_txt) : "";
  out.launch_template = tmpl_txt ? reinterpret_cast<const char*>(tmpl_txt) : "";

  return true;
}

bool Database::GetSystemLaunchInfo(const std::string& system_id, LaunchInfo& out) {
  static const std::string kSql =
      "SELECT id, launch_type, launch_template "
      "FROM systems WHERE id=?1;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare GetSystemLaunchInfo");
    return false;
  }

  sqlite3_bind_text(stmt.Get(), 1, system_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.Get()) != SQLITE_ROW) {
    SetError("system launch target not found");
    return false;
  }

  const auto* sys_txt = sqlite3_column_text(stmt.Get(), 0);
  const auto* type_txt = sqlite3_column_text(stmt.Get(), 1);
  const auto* tmpl_txt = sqlite3_column_text(stmt.Get(), 2);

  out = LaunchInfo{};
  out.system_id = sys_txt ? reinterpret_cast<const char*>(sys_txt) : "";
  out.launch_type = type_txt ? reinterpret_cast<const char*>(type_txt) : "";
  out.launch_template = tmpl_txt ? reinterpret_cast<const char*>(tmpl_txt) : "";
  return true;
}

bool Database::GetLaunchOverride(const std::string& scope_type,
                                 const std::string& scope_id,
                                 LaunchOverride& out) {
  static const std::string kSql =
      "SELECT scope_type, scope_id, core_path, audio_latency, video_width, video_height "
      "FROM launch_overrides WHERE scope_type=?1 AND scope_id=?2;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare GetLaunchOverride");
    return false;
  }
  sqlite3_bind_text(stmt.Get(), 1, scope_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 2, scope_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_ROW) {
    if (rc == SQLITE_DONE) {
      // Missing override is an expected state; keep it queryable via LastError
      // without emitting an error log.
      last_error_ = "launch override not found";
      return false;
    }
    SetError("failed to execute GetLaunchOverride");
    return false;
  }

  const auto* type_txt = sqlite3_column_text(stmt.Get(), 0);
  const auto* id_txt = sqlite3_column_text(stmt.Get(), 1);
  const auto* core_txt = sqlite3_column_text(stmt.Get(), 2);

  out.scope_type = type_txt ? reinterpret_cast<const char*>(type_txt) : "";
  out.scope_id = id_txt ? reinterpret_cast<const char*>(id_txt) : "";
  out.core_path = core_txt ? reinterpret_cast<const char*>(core_txt) : "";
  out.audio_latency = sqlite3_column_int(stmt.Get(), 3);
  out.video_width = sqlite3_column_int(stmt.Get(), 4);
  out.video_height = sqlite3_column_int(stmt.Get(), 5);
  return true;
}

bool Database::UpsertLaunchOverride(const LaunchOverride& override_row) {
  static const std::string kSql =
      "INSERT INTO launch_overrides(scope_type,scope_id,core_path,audio_latency,video_width,video_height) "
      "VALUES(?1,?2,?3,?4,?5,?6) "
      "ON CONFLICT(scope_type,scope_id) DO UPDATE SET "
      "core_path=excluded.core_path, "
      "audio_latency=excluded.audio_latency, "
      "video_width=excluded.video_width, "
      "video_height=excluded.video_height;";
  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare UpsertLaunchOverride");
    return false;
  }
  sqlite3_bind_text(stmt.Get(), 1, override_row.scope_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 2, override_row.scope_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 3, override_row.core_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.Get(), 4, override_row.audio_latency);
  sqlite3_bind_int(stmt.Get(), 5, override_row.video_width);
  sqlite3_bind_int(stmt.Get(), 6, override_row.video_height);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute UpsertLaunchOverride");
    return false;
  }
  return true;
}

bool Database::DeleteLaunchOverride(const std::string& scope_type,
                                    const std::string& scope_id) {
  static const std::string kSql =
      "DELETE FROM launch_overrides WHERE scope_type=?1 AND scope_id=?2;";
  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare DeleteLaunchOverride");
    return false;
  }
  sqlite3_bind_text(stmt.Get(), 1, scope_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 2, scope_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute DeleteLaunchOverride");
    return false;
  }
  return true;
}

bool Database::UpdateLastPlayed(const int game_id, const std::int64_t unix_seconds) {
  static const std::string kSql =
      "UPDATE games SET last_played=?1 WHERE id=?2;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare UpdateLastPlayed");
    return false;
  }

  sqlite3_bind_int64(stmt.Get(), 1, unix_seconds);
  sqlite3_bind_int(stmt.Get(), 2, game_id);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute UpdateLastPlayed");
    return false;
  }

  return true;
}

bool Database::EnqueueJob(const std::string& type,
                          const std::string& payload_json,
                          int* out_job_id) {
  static const std::string kSql =
      "INSERT INTO jobs(type,status,payload_json,error,created_at,updated_at) "
      "VALUES(?1,'queued',?2,NULL,?3,?3);";
  static const std::string kIdSql = "SELECT last_insert_rowid();";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare EnqueueJob");
    return false;
  }

  const auto now = NowUnixSeconds();
  sqlite3_bind_text(stmt.Get(), 1, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 2, payload_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.Get(), 3, now);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute EnqueueJob");
    return false;
  }

  if (out_job_id) {
    Statement id_stmt(db_, kIdSql);
    if (!id_stmt.Ok() || sqlite3_step(id_stmt.Get()) != SQLITE_ROW) {
      SetError("failed to fetch inserted job id");
      return false;
    }
    *out_job_id = static_cast<int>(sqlite3_column_int64(id_stmt.Get(), 0));
  }

  return true;
}

bool Database::ClaimNextQueuedJob(JobRecord& out) {
  out = JobRecord{};

  if (!Exec("BEGIN IMMEDIATE TRANSACTION;")) {
    return false;
  }

  Statement select_stmt(
      db_,
      "SELECT id,type,status,payload_json,error,created_at,updated_at "
      "FROM jobs WHERE status='queued' ORDER BY id LIMIT 1;");
  if (!select_stmt.Ok()) {
    Exec("ROLLBACK;");
    SetError("failed to prepare ClaimNextQueuedJob select");
    return false;
  }

  if (sqlite3_step(select_stmt.Get()) != SQLITE_ROW) {
    Exec("ROLLBACK;");
    return false;
  }

  out.id = sqlite3_column_int(select_stmt.Get(), 0);
  {
    const auto* type_txt = sqlite3_column_text(select_stmt.Get(), 1);
    const auto* status_txt = sqlite3_column_text(select_stmt.Get(), 2);
    const auto* payload_txt = sqlite3_column_text(select_stmt.Get(), 3);
    const auto* err_txt = sqlite3_column_text(select_stmt.Get(), 4);
    out.type = type_txt ? reinterpret_cast<const char*>(type_txt) : "";
    out.status = status_txt ? reinterpret_cast<const char*>(status_txt) : "";
    out.payload_json = payload_txt ? reinterpret_cast<const char*>(payload_txt) : "";
    out.error = err_txt ? reinterpret_cast<const char*>(err_txt) : "";
  }
  out.created_at = sqlite3_column_int64(select_stmt.Get(), 5);
  out.updated_at = sqlite3_column_int64(select_stmt.Get(), 6);

  Statement update_stmt(
      db_,
      "UPDATE jobs SET status='running', updated_at=?1 WHERE id=?2;");
  if (!update_stmt.Ok()) {
    Exec("ROLLBACK;");
    SetError("failed to prepare ClaimNextQueuedJob update");
    return false;
  }
  sqlite3_bind_int64(update_stmt.Get(), 1, NowUnixSeconds());
  sqlite3_bind_int(update_stmt.Get(), 2, out.id);
  if (sqlite3_step(update_stmt.Get()) != SQLITE_DONE) {
    Exec("ROLLBACK;");
    SetError("failed to execute ClaimNextQueuedJob update");
    return false;
  }

  out.status = "running";
  out.updated_at = NowUnixSeconds();

  if (!Exec("COMMIT;")) {
    Exec("ROLLBACK;");
    return false;
  }

  return true;
}

bool Database::MarkJobOk(const int job_id) {
  static const std::string kSql =
      "UPDATE jobs SET status='ok', error=NULL, updated_at=?1 WHERE id=?2;";
  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare MarkJobOk");
    return false;
  }
  sqlite3_bind_int64(stmt.Get(), 1, NowUnixSeconds());
  sqlite3_bind_int(stmt.Get(), 2, job_id);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute MarkJobOk");
    return false;
  }
  return true;
}

bool Database::MarkJobError(const int job_id, const std::string& error) {
  static const std::string kSql =
      "UPDATE jobs SET status='error', error=?1, updated_at=?2 WHERE id=?3;";
  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare MarkJobError");
    return false;
  }
  sqlite3_bind_text(stmt.Get(), 1, error.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.Get(), 2, NowUnixSeconds());
  sqlite3_bind_int(stmt.Get(), 3, job_id);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute MarkJobError");
    return false;
  }
  return true;
}

int Database::CountJobsByStatus(const std::string& status) {
  static const std::string kSql = "SELECT COUNT(*) FROM jobs WHERE status=?1;";
  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    return 0;
  }
  sqlite3_bind_text(stmt.Get(), 1, status.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.Get()) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int(stmt.Get(), 0);
}

bool Database::ListGamesNeedingMetadata(std::vector<MetadataCandidate>& out,
                                        const int limit) {
  out.clear();

  static const std::string kSql =
      "SELECT g.id, g.system_id, g.title, g.filename "
      "FROM games g "
      "LEFT JOIN game_metadata m ON m.game_id = g.id "
      "WHERE g.is_present=1 "
      "AND (m.game_id IS NULL OR m.updated_at IS NULL OR m.source IS NULL) "
      "ORDER BY g.id "
      "LIMIT ?1;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare ListGamesNeedingMetadata");
    return false;
  }
  sqlite3_bind_int(stmt.Get(), 1, limit > 0 ? limit : 1000);

  while (sqlite3_step(stmt.Get()) == SQLITE_ROW) {
    MetadataCandidate row;
    row.game_id = sqlite3_column_int(stmt.Get(), 0);
    {
      const auto* sys_txt = sqlite3_column_text(stmt.Get(), 1);
      const auto* title_txt = sqlite3_column_text(stmt.Get(), 2);
      const auto* file_txt = sqlite3_column_text(stmt.Get(), 3);
      row.system_id = sys_txt ? reinterpret_cast<const char*>(sys_txt) : "";
      row.title = title_txt ? reinterpret_cast<const char*>(title_txt) : "";
      row.filename = file_txt ? reinterpret_cast<const char*>(file_txt) : "";
    }
    out.push_back(std::move(row));
  }

  return true;
}

bool Database::UpsertGameMetadata(const MetadataUpdate& update) {
  static const std::string kSql =
      "INSERT INTO game_metadata(game_id,release_year,publisher,developer,genre,players,"
      "description,source,source_id,updated_at) "
      "VALUES(?1,?2,NULL,NULL,?3,?4,?5,?6,NULL,?7) "
      "ON CONFLICT(game_id) DO UPDATE SET "
      "release_year=excluded.release_year, "
      "genre=excluded.genre, "
      "players=excluded.players, "
      "description=excluded.description, "
      "source=excluded.source, "
      "updated_at=excluded.updated_at;";

  Statement stmt(db_, kSql);
  if (!stmt.Ok()) {
    SetError("failed to prepare UpsertGameMetadata");
    return false;
  }

  const auto now = NowUnixSeconds();
  sqlite3_bind_int(stmt.Get(), 1, update.game_id);
  if (update.release_year > 0) {
    sqlite3_bind_int(stmt.Get(), 2, update.release_year);
  } else {
    sqlite3_bind_null(stmt.Get(), 2);
  }
  sqlite3_bind_text(stmt.Get(), 3, update.genre.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.Get(), 4, update.players);
  sqlite3_bind_text(stmt.Get(), 5, update.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Get(), 6, update.source.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.Get(), 7, now);
  if (sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    SetError("failed to execute UpsertGameMetadata");
    return false;
  }
  return true;
}

ScanSummary Database::ReadSummary() const {
  ScanSummary out;
  QuerySingleInt("SELECT COUNT(*) FROM games;", out.total_games);
  QuerySingleInt("SELECT COUNT(*) FROM games WHERE is_present=1;", out.present_games);
  QuerySingleInt("SELECT COUNT(*) FROM games WHERE is_present=0;", out.missing_games);
  return out;
}

bool Database::IsOpen() const { return db_ != nullptr; }

const std::string& Database::LastError() const { return last_error_; }

bool Database::Exec(const std::string& sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
  if (rc == SQLITE_OK) {
    return true;
  }

  std::string msg = "sqlite exec failed";
  if (err) {
    msg += ": ";
    msg += err;
    sqlite3_free(err);
  }
  SetError(msg);
  return false;
}

bool Database::QuerySingleInt(const std::string& sql, int& value) const {
  Statement stmt(db_, sql);
  if (!stmt.Ok()) {
    return false;
  }

  if (sqlite3_step(stmt.Get()) == SQLITE_ROW) {
    value = sqlite3_column_int(stmt.Get(), 0);
    return true;
  }

  return false;
}

void Database::SetError(const std::string& message) {
  last_error_ = message;
  if (db_) {
    last_error_ += " (";
    last_error_ += sqlite3_errmsg(db_);
    last_error_ += ")";
  }
  core::Log(core::LogLevel::Error, last_error_);
}

}  // namespace gb::db
