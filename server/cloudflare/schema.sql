-- ProjectCadence accounts and scores. Safe to run again on a fresh database;
-- an existing one from before clans and replays needs migrations/0002_social.sql.
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE COLLATE NOCASE,
  pass_hash TEXT NOT NULL,
  salt TEXT NOT NULL,
  created INTEGER NOT NULL,
  role INTEGER NOT NULL DEFAULT 0,          -- 1 admin
  restricted INTEGER NOT NULL DEFAULT 0,    -- hidden from boards and rankings
  silenced_until INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sessions (
  token TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL,
  created INTEGER NOT NULL
);

-- one row per player and map: their best
CREATE TABLE IF NOT EXISTS scores (
  user_id INTEGER NOT NULL,
  map_md5 TEXT NOT NULL,
  beatmap_id INTEGER NOT NULL DEFAULT 0,
  mode INTEGER NOT NULL DEFAULT 0,
  score INTEGER NOT NULL,
  acc REAL NOT NULL,
  max_combo INTEGER NOT NULL,
  mods INTEGER NOT NULL,
  pp REAL NOT NULL DEFAULT 0,
  rank INTEGER NOT NULL,
  n300 INTEGER NOT NULL DEFAULT 0,
  n100 INTEGER NOT NULL DEFAULT 0,
  n50 INTEGER NOT NULL DEFAULT 0,
  miss INTEGER NOT NULL DEFAULT 0,
  played INTEGER NOT NULL,
  held INTEGER NOT NULL DEFAULT 0,          -- waiting for an admin: pp far above the player's usual
  PRIMARY KEY (user_id, map_md5)
);
CREATE INDEX IF NOT EXISTS scores_by_map ON scores (map_md5, score DESC);
CREATE INDEX IF NOT EXISTS scores_by_mode ON scores (mode, user_id, pp DESC);

-- every play counts here, bests or not
CREATE TABLE IF NOT EXISTS stats (
  user_id INTEGER NOT NULL,
  mode INTEGER NOT NULL,
  plays INTEGER NOT NULL DEFAULT 0,
  total_score INTEGER NOT NULL DEFAULT 0,
  max_combo INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (user_id, mode)
);

-- the replay behind each best, as osu!'s LZMA'd frame text
CREATE TABLE IF NOT EXISTS replays (
  user_id INTEGER NOT NULL,
  map_md5 TEXT NOT NULL,
  data BLOB NOT NULL,
  PRIMARY KEY (user_id, map_md5)
);

CREATE TABLE IF NOT EXISTS friends (
  user_id INTEGER NOT NULL,
  friend_id INTEGER NOT NULL,
  PRIMARY KEY (user_id, friend_id)
);

-- what osu.ppy.sh says about a map, when the server has API keys
CREATE TABLE IF NOT EXISTS maps (
  md5 TEXT PRIMARY KEY,
  beatmap_id INTEGER NOT NULL DEFAULT 0,
  set_id INTEGER NOT NULL DEFAULT 0,
  status INTEGER NOT NULL,                  -- osu!'s: -2 graveyard .. 1 ranked, 2 approved, 3 qualified, 4 loved; -3 not on the site
  title TEXT NOT NULL DEFAULT '',
  checked INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS clans (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE COLLATE NOCASE,
  tag TEXT NOT NULL UNIQUE COLLATE NOCASE,
  owner INTEGER NOT NULL,
  created INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS clan_members (
  user_id INTEGER PRIMARY KEY,
  clan_id INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS playlists (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  owner INTEGER NOT NULL,
  name TEXT NOT NULL,
  created INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS playlist_items (
  playlist_id INTEGER NOT NULL,
  pos INTEGER NOT NULL,
  md5 TEXT NOT NULL,
  beatmap_id INTEGER NOT NULL DEFAULT 0,
  set_id INTEGER NOT NULL DEFAULT 0,
  title TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (playlist_id, md5)
);

CREATE TABLE IF NOT EXISTS daily_scores (
  day TEXT NOT NULL,
  user_id INTEGER NOT NULL,
  score INTEGER NOT NULL,
  acc REAL NOT NULL,
  max_combo INTEGER NOT NULL,
  mods INTEGER NOT NULL,
  PRIMARY KEY (day, user_id)
);
