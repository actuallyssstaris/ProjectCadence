-- playlists and the daily challenge
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
