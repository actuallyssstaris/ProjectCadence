// The website: rankings, clans, profiles and map boards, one page that reads the
// same /api calls the consoles use.

const PAGE = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ProjectCadence</title>
<style>
  :root { --bg: #17151d; --panel: #221f2b; --row: #2b2836; --text: #eee; --dim: #9a96a8; --pink: #ff66aa; --blue: #66ccff; }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.5 system-ui, sans-serif; }
  header { background: var(--panel); padding: 14px 16px; display: flex; gap: 18px; align-items: center; flex-wrap: wrap; }
  header b { color: var(--pink); font-size: 18px; margin-right: 8px; }
  header a { color: var(--dim); text-decoration: none; }
  header a.on, header a:hover { color: var(--text); }
  main { max-width: 960px; margin: 0 auto; padding: 16px; }
  .modes { display: flex; gap: 14px; margin: 4px 0 14px; flex-wrap: wrap; }
  .modes a { color: var(--dim); text-decoration: none; padding-bottom: 3px; }
  .modes a.on { color: var(--text); border-bottom: 2px solid var(--pink); }
  table { width: 100%; border-collapse: separate; border-spacing: 0 4px; }
  td, th { padding: 8px 10px; text-align: left; white-space: nowrap; }
  th { color: var(--dim); font-weight: 500; font-size: 13px; }
  tr.r td { background: var(--row); }
  tr.r td:first-child { border-radius: 6px 0 0 6px; }
  tr.r td:last-child { border-radius: 0 6px 6px 0; }
  .num { text-align: right; }
  .wrap { overflow-x: auto; }
  a.u { color: var(--text); text-decoration: none; }
  a.u:hover { color: var(--blue); }
  .tag { color: var(--blue); margin-right: 6px; }
  .card { background: var(--panel); border-radius: 12px; padding: 18px; margin-bottom: 14px; }
  .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
  .stats div span { display: block; color: var(--dim); font-size: 13px; }
  .stats div b { font-size: 22px; }
  .muted { color: var(--dim); }
  form { display: flex; gap: 8px; margin-bottom: 14px; }
  input { flex: 1; background: var(--row); border: 0; color: var(--text); padding: 8px 10px; border-radius: 6px; }
</style>
</head>
<body>
<header><b>ProjectCadence</b><a href="#/players">players</a><a href="#/clans">clans</a></header>
<main id="app"></main>
<script>
const modes = ["osu!", "taiko", "catch", "mania"];
const app = document.getElementById("app");
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const fmt = (n) => Number(n || 0).toLocaleString("en");
const get = (p) => fetch(p).then((r) => r.json());

function modeTabs(base, mode) {
  return '<div class="modes">' + modes.map((m, i) => '<a class="' + (i == mode ? "on" : "") + '" href="' + base + "/" + i + '">' + m + "</a>").join("") + "</div>";
}

async function players(mode) {
  const d = await get("/api/users/top?mode=" + mode);
  app.innerHTML = '<form onsubmit="location.hash=\\'#/u/\\'+encodeURIComponent(this.q.value);return false"><input name="q" placeholder="find a player"></form>' +
    modeTabs("#/players", mode) + '<div class="wrap"><table><tr><th>#</th><th>player</th><th class="num">accuracy</th><th class="num">plays</th><th class="num">pp</th></tr>' +
    d.users.map((u) => '<tr class="r"><td>' + u.rank + '</td><td>' + (u.clan ? '<span class="tag">[' + esc(u.clan) + "]</span>" : "") +
      '<a class="u" href="#/u/' + encodeURIComponent(u.name) + "/" + mode + '">' + esc(u.name) + '</a></td><td class="num">' + (u.acc * 100).toFixed(2) +
      '%</td><td class="num">' + fmt(u.plays) + '</td><td class="num">' + fmt(u.pp) + "pp</td></tr>").join("") +
    "</table></div>" + (d.users.length ? "" : '<p class="muted">nobody has a ranked score here yet</p>');
}

async function clans(mode) {
  const d = await get("/api/clans/top?mode=" + mode);
  app.innerHTML = modeTabs("#/clans", mode) + '<div class="wrap"><table><tr><th>#</th><th>clan</th><th class="num">members</th><th class="num">pp</th></tr>' +
    d.clans.map((c) => '<tr class="r"><td>' + c.rank + '</td><td><span class="tag">[' + esc(c.tag) + "]</span>" + esc(c.name) +
      '</td><td class="num">' + c.members + '</td><td class="num">' + fmt(c.pp) + "pp</td></tr>").join("") + "</table></div>" +
    (d.clans.length ? "" : '<p class="muted">no clans yet</p>');
}

async function profile(name, mode) {
  const r = await fetch("/api/users/profile?name=" + encodeURIComponent(name) + "&mode=" + mode);
  if (!r.ok) { app.innerHTML = '<p class="muted">no player called ' + esc(name) + "</p>"; return; }
  const u = await r.json();
  app.innerHTML = '<div class="card"><h2 style="margin:0 0 4px">' + (u.clan ? '<span class="tag">[' + esc(u.clan) + "]</span>" : "") + esc(u.name) +
    '</h2><div class="muted">joined ' + new Date(u.joined).toLocaleDateString() + (u.restricted ? " · restricted" : "") + "</div></div>" +
    modeTabs("#/u/" + encodeURIComponent(u.name), mode) +
    '<div class="card stats"><div><span>rank</span><b>' + (u.rank ? "#" + fmt(u.rank) : "-") + "</b></div><div><span>pp</span><b>" + fmt(u.pp) +
    "</b></div><div><span>accuracy</span><b>" + (u.acc * 100).toFixed(2) + "%</b></div><div><span>play count</span><b>" + fmt(u.plays) +
    "</b></div><div><span>ranked score</span><b>" + fmt(u.ranked_score) + "</b></div><div><span>max combo</span><b>" + fmt(u.max_combo) + "</b></div></div>" +
    '<h3>best plays</h3><div class="wrap"><table>' + u.top.map((s) => '<tr class="r"><td>' + esc(s.title || (s.beatmap_id ? "beatmap " + s.beatmap_id : s.md5.slice(0, 10))) +
      '</td><td class="num">' + (s.acc * 100).toFixed(2) + '%</td><td class="num">' + fmt(s.max_combo) + 'x</td><td class="num">' + Math.round(s.pp) + "pp</td></tr>").join("") +
    "</table></div>";
}

function route() {
  const [, page, a, b] = location.hash.split("/");
  for (const l of document.querySelectorAll("header a")) l.className = l.getAttribute("href") === "#/" + (page || "players") ? "on" : "";
  if (page === "u" && a) return profile(decodeURIComponent(a), Number(b) || 0);
  if (page === "clans") return clans(Number(a) || 0);
  return players(Number(a) || 0);
}
addEventListener("hashchange", route);
route();
</script>
</body>
</html>`;

export function page() {
  return new Response(PAGE, { headers: { "content-type": "text/html; charset=utf-8" } });
}
