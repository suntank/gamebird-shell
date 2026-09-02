CREATE TABLE IF NOT EXISTS systems (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  rom_extensions TEXT NOT NULL,
  launch_type TEXT NOT NULL,
  launch_template TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS games (
  id INTEGER PRIMARY KEY,
  system_id TEXT NOT NULL REFERENCES systems(id),
  library_root TEXT,
  path TEXT NOT NULL UNIQUE,
  filename TEXT NOT NULL,
  title TEXT NOT NULL,
  sort_title TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  mtime INTEGER NOT NULL,
  sha1 TEXT,
  is_favorite INTEGER NOT NULL DEFAULT 0,
  is_hidden INTEGER NOT NULL DEFAULT 0,
  last_played INTEGER,
  is_present INTEGER NOT NULL DEFAULT 1
);

CREATE INDEX IF NOT EXISTS idx_games_system ON games(system_id);
CREATE INDEX IF NOT EXISTS idx_games_present ON games(is_present);
CREATE INDEX IF NOT EXISTS idx_games_library_root ON games(library_root);

CREATE TABLE IF NOT EXISTS library_roots (
  root_path TEXT PRIMARY KEY,
  status TEXT NOT NULL,
  error TEXT NOT NULL DEFAULT '',
  device_id INTEGER NOT NULL DEFAULT 0,
  last_scan_at INTEGER NOT NULL,
  files_seen INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS game_metadata (
  game_id INTEGER PRIMARY KEY REFERENCES games(id),
  release_year INTEGER,
  publisher TEXT,
  developer TEXT,
  genre TEXT,
  players INTEGER,
  description TEXT,
  source TEXT,
  source_id TEXT,
  updated_at INTEGER
);

CREATE TABLE IF NOT EXISTS assets (
  game_id INTEGER PRIMARY KEY REFERENCES games(id),
  box_art_path TEXT,
  thumb_path TEXT,
  screenshot_path TEXT,
  updated_at INTEGER
);

CREATE TABLE IF NOT EXISTS jobs (
  id INTEGER PRIMARY KEY,
  type TEXT NOT NULL,
  status TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  error TEXT,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status);

CREATE TABLE IF NOT EXISTS launch_overrides (
  scope_type TEXT NOT NULL,
  scope_id TEXT NOT NULL,
  core_path TEXT,
  audio_latency INTEGER,
  video_width INTEGER,
  video_height INTEGER,
  PRIMARY KEY(scope_type, scope_id)
);

CREATE INDEX IF NOT EXISTS idx_launch_overrides_scope
ON launch_overrides(scope_type, scope_id);
