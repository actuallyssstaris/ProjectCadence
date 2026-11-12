-- problem reports sent from the game
CREATE TABLE IF NOT EXISTS reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created INTEGER NOT NULL,
  ip TEXT NOT NULL DEFAULT '',
  player TEXT NOT NULL DEFAULT '',
  version TEXT NOT NULL DEFAULT '',
  text TEXT NOT NULL DEFAULT '',
  info TEXT NOT NULL DEFAULT '',
  log TEXT NOT NULL DEFAULT '',
  prev_log TEXT NOT NULL DEFAULT '',
  crashed INTEGER NOT NULL DEFAULT 0,
  closed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS reports_open ON reports (closed, created DESC);
