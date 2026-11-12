-- for a database made from the first schema: moderation columns, held scores,
-- and the new tables. Run once:
--   npx wrangler d1 execute projectcadence --remote --file migrations/0002_social.sql
ALTER TABLE users ADD COLUMN role INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN restricted INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN silenced_until INTEGER NOT NULL DEFAULT 0;
ALTER TABLE scores ADD COLUMN held INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS scores_by_mode ON scores (mode, user_id, pp DESC);

CREATE TABLE IF NOT EXISTS stats (
  user_id INTEGER NOT NULL,
  mode INTEGER NOT NULL,
  plays INTEGER NOT NULL DEFAULT 0,
  total_score INTEGER NOT NULL DEFAULT 0,
  max_combo INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (user_id, mode)
);

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

CREATE TABLE IF NOT EXISTS maps (
  md5 TEXT PRIMARY KEY,
  beatmap_id INTEGER NOT NULL DEFAULT 0,
  set_id INTEGER NOT NULL DEFAULT 0,
  status INTEGER NOT NULL,
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
