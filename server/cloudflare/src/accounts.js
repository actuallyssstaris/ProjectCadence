// Accounts, scores, rankings, replays, friends, clans and moderation: everything
// that lives in D1. The room object calls into here for sessions and admin commands.

export function reply(obj, status = 200) {
  return new Response(JSON.stringify(obj), { status, headers: { "content-type": "application/json" } });
}

const NAME_OK = /^[A-Za-z0-9_\- ]{3,20}$/;
const TAG_OK = /^[A-Za-z0-9]{1,6}$/;
const CLAN_OK = /^[A-Za-z0-9_\- ]{2,24}$/;

async function hash(password, salt) {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(password), "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits(
    { name: "PBKDF2", hash: "SHA-256", salt: new TextEncoder().encode(salt), iterations: 100000 },
    key,
    256,
  );
  return [...new Uint8Array(bits)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function newSession(env, userId) {
  const token = crypto.randomUUID() + crypto.randomUUID().slice(0, 8);
  await env.DB.prepare("INSERT INTO sessions (token, user_id, created) VALUES (?, ?, ?)").bind(token, userId, Date.now()).run();
  return token;
}

export async function userForSession(env, session) {
  if (!session) return null;
  const u = await env.DB.prepare(
    `SELECT users.id, users.name, users.role, users.restricted, users.silenced_until
       FROM sessions JOIN users ON users.id = sessions.user_id WHERE token = ?`,
  )
    .bind(String(session))
    .first();
  if (u && env.ADMIN && u.name.toLowerCase() === String(env.ADMIN).toLowerCase()) u.role = 1;
  return u;
}

const byName = (env, name) =>
  env.DB.prepare("SELECT id, name, created, restricted, role FROM users WHERE name = ?").bind(String(name || "").trim()).first();

/* ---------- map status from osu.ppy.sh, cached ---------- */

let osuToken = null;
let osuTokenUntil = 0;

async function osuApi(env, path) {
  if (!env.OSU_CLIENT_ID || !env.OSU_CLIENT_SECRET) return null;
  if (!osuToken || Date.now() > osuTokenUntil) {
    const r = await fetch("https://osu.ppy.sh/oauth/token", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({
        client_id: Number(env.OSU_CLIENT_ID),
        client_secret: env.OSU_CLIENT_SECRET,
        grant_type: "client_credentials",
        scope: "public",
      }),
    });
    if (!r.ok) return null;
    const t = await r.json();
    osuToken = t.access_token;
    osuTokenUntil = Date.now() + (t.expires_in - 60) * 1000;
  }
  const r = await fetch("https://osu.ppy.sh/api/v2" + path, {
    headers: { authorization: "Bearer " + osuToken, accept: "application/json" },
  });
  if (r.status === 404) return { missing: true };
  return r.ok ? r.json() : null;
}

const STATUS = { graveyard: -2, wip: -1, pending: 0, ranked: 1, approved: 2, qualified: 3, loved: 4 };

// null when we can't tell (no API keys, or osu.ppy.sh didn't answer)
export async function mapStatus(env, md5) {
  const row = await env.DB.prepare("SELECT status, title, beatmap_id, set_id FROM maps WHERE md5 = ?").bind(md5).first();
  // unranked maps can get ranked later: look again after a week
  if (row && (row.status >= 1 || Date.now() - row.checked < 7 * 86400000)) return row;
  const b = await osuApi(env, "/beatmaps/lookup?checksum=" + md5).catch(() => null);
  if (!b) return row || null;
  const status = b.missing ? -3 : STATUS[b.status] ?? 0;
  const title = b.beatmapset ? `${b.beatmapset.artist} - ${b.beatmapset.title} [${b.version}]` : "";
  await env.DB.prepare("INSERT OR REPLACE INTO maps (md5, beatmap_id, set_id, status, title, checked) VALUES (?, ?, ?, ?, ?, ?)")
    .bind(md5, b.id || 0, b.beatmapset_id || 0, status, title.slice(0, 200), Date.now())
    .run();
  return { status, title, beatmap_id: b.id || 0, set_id: b.beatmapset_id || 0 };
}

// pp counts on ranked and approved maps, and on ones we can't check
const PP_MAPS = "(m.status IS NULL OR m.status IN (1, 2))";

/* ---------- score checks ---------- */

const MIN_GAP = 5000; // ms between two uploads from one account
const MAX_SCORE = 4000000;
const MAX_PP = 3500;
const MAX_REPLAY = 1500000;

// the server can't replay a play, but it can turn away numbers no play produces
function implausible(b, n) {
  const score = n("score");
  const acc = Number(b.acc);
  const hits = n("n300") + n("n100") + n("n50") + n("miss");
  if (score <= 0 || score > MAX_SCORE) return "score out of range";
  if (!(acc >= 0 && acc <= 1)) return "accuracy out of range";
  if (n("mode") > 3) return "bad mode";
  if (n("rank") >= 8) return "failed plays aren't uploaded";
  if (!(Number(b.pp) >= 0 && Number(b.pp) <= MAX_PP)) return "pp out of range";
  if (hits > 20000) return "bad hit counts";
  if (n("max_combo") > 60000) return "combo out of range";
  // osu!: judgement counts and accuracy have to roughly agree (slider ticks and
  // tails nudge accuracy, hence the slack). mania sends no Perfect count and
  // catch counts droplets elsewhere, so they skip this.
  if (n("mode") === 0) {
    if (hits === 0) return "bad hit counts";
    const from = (300 * n("n300") + 100 * n("n100") + 50 * n("n50")) / (300 * hits);
    if (Math.abs(from - acc) > 0.15) return "accuracy doesn't match the judgements";
    if (n("max_combo") > hits * 6 + 50) return "combo doesn't fit the map";
  }
  if (n("mode") !== 2 && acc >= 1 && (n("miss") || n("n100") || n("n50"))) return "accuracy doesn't match the judgements";
  return null;
}

/* ---------- rankings ---------- */

// osu!'s weighting: each next best counts 95% of the one before, plus a small
// bonus for how many maps you've played
async function standings(env, mode) {
  const rows = (
    await env.DB.prepare(
      `SELECT s.user_id, s.pp, s.acc, s.score, u.name
         FROM scores s JOIN users u ON u.id = s.user_id LEFT JOIN maps m ON m.md5 = s.map_md5
        WHERE s.mode = ? AND u.restricted = 0 AND s.held = 0 AND ${PP_MAPS}
        ORDER BY s.user_id, s.pp DESC`,
    )
      .bind(mode)
      .all()
  ).results;
  const users = new Map();
  for (const r of rows) {
    let u = users.get(r.user_id);
    if (!u) users.set(r.user_id, (u = { id: r.user_id, name: r.name, n: 0, pp: 0, accw: 0, w: 0, ranked_score: 0 }));
    const w = Math.pow(0.95, u.n);
    if (u.n < 100) {
      u.pp += r.pp * w;
      u.accw += r.acc * w;
      u.w += w;
    }
    u.ranked_score += r.score;
    u.n++;
  }
  const list = [...users.values()].map((u) => ({
    id: u.id,
    name: u.name,
    pp: Math.round(u.pp + 416.6667 * (1 - Math.pow(0.9994, u.n))),
    acc: u.w ? u.accw / u.w : 0,
    ranked_score: u.ranked_score,
  }));
  list.sort((a, b) => b.pp - a.pp);
  list.forEach((u, i) => (u.rank = i + 1));
  return list;
}

async function clanOf(env, userId) {
  return env.DB.prepare("SELECT c.id, c.name, c.tag, c.owner FROM clan_members cm JOIN clans c ON c.id = cm.clan_id WHERE cm.user_id = ?")
    .bind(userId)
    .first();
}

/* ---------- admin commands, typed in chat ---------- */

// "!restrict name", "!unrestrict name", "!silence name minutes", "!approve name";
// returns what to say back, or null when the text isn't a command
export async function adminCommand(env, user, text) {
  const m = /^!(restrict|unrestrict|silence|unsilence|approve)\s+(\S+)(?:\s+(\d+))?/.exec(text);
  if (!m) return null;
  if (!user || user.role !== 1) return "only admins can do that";
  const target = await byName(env, m[2]);
  if (!target) return `nobody's called ${m[2]}`;
  switch (m[1]) {
    case "restrict":
    case "unrestrict":
      await env.DB.prepare("UPDATE users SET restricted = ? WHERE id = ?").bind(m[1] === "restrict" ? 1 : 0, target.id).run();
      return `${target.name} is ${m[1] === "restrict" ? "restricted" : "back"}`;
    case "silence":
    case "unsilence": {
      const mins = m[1] === "silence" ? Number(m[3] || 10) : 0;
      await env.DB.prepare("UPDATE users SET silenced_until = ? WHERE id = ?").bind(mins ? Date.now() + mins * 60000 : 0, target.id).run();
      return mins ? `${target.name} is silenced for ${mins} minutes` : `${target.name} can talk again`;
    }
    case "approve": {
      const r = await env.DB.prepare("UPDATE scores SET held = 0 WHERE user_id = ? AND held = 1").bind(target.id).run();
      return `approved ${r.meta.changes} of ${target.name}'s scores`;
    }
  }
  return null;
}

/* ---------- the calls ---------- */

export async function accounts(request, env, url) {
  const body = request.method === "POST" ? await request.json().catch(() => ({})) : {};
  const q = (k) => url.searchParams.get(k) || "";
  const modeOf = (v) => Math.min(3, Math.max(0, Math.floor(Number(v) || 0)));

  switch (url.pathname) {
    case "/api/account/register": {
      const name = String(body.name || "").trim();
      const password = String(body.password || "");
      if (!NAME_OK.test(name)) return reply({ error: "names are 3 to 20 letters, digits, spaces, _ or -" }, 400);
      if (password.length < 6) return reply({ error: "the password needs at least 6 characters" }, 400);
      if (await byName(env, name)) return reply({ error: "that name is taken" }, 409);
      const salt = crypto.randomUUID();
      const res = await env.DB.prepare("INSERT INTO users (name, pass_hash, salt, created) VALUES (?, ?, ?, ?)")
        .bind(name, await hash(password, salt), salt, Date.now())
        .run();
      return reply({ session: await newSession(env, res.meta.last_row_id), name });
    }
    case "/api/account/login": {
      const user = await env.DB.prepare("SELECT id, name, pass_hash, salt FROM users WHERE name = ?").bind(String(body.name || "").trim()).first();
      if (!user || (await hash(String(body.password || ""), user.salt)) !== user.pass_hash)
        return reply({ error: "wrong name or password" }, 401);
      const clan = await clanOf(env, user.id);
      return reply({ session: await newSession(env, user.id), name: user.name, clan: clan ? clan.tag : "" });
    }
    case "/api/account/password":
    case "/api/account/delete": {
      const me = await userForSession(env, body.session);
      if (!me) return reply({ error: "sign in again" }, 401);
      const row = await env.DB.prepare("SELECT pass_hash, salt FROM users WHERE id = ?").bind(me.id).first();
      if ((await hash(String(body.password || ""), row.salt)) !== row.pass_hash) return reply({ error: "wrong password" }, 401);
      if (url.pathname.endsWith("/password")) {
        const next = String(body.new_password || "");
        if (next.length < 6) return reply({ error: "the password needs at least 6 characters" }, 400);
        const salt = crypto.randomUUID();
        // every other session ends with the old password
        await env.DB.batch([
          env.DB.prepare("UPDATE users SET pass_hash = ?, salt = ? WHERE id = ?").bind(await hash(next, salt), salt, me.id),
          env.DB.prepare("DELETE FROM sessions WHERE user_id = ?").bind(me.id),
        ]);
        return reply({ session: await newSession(env, me.id), name: me.name });
      }
      const clan = await clanOf(env, me.id);
      await env.DB.batch([
        env.DB.prepare("DELETE FROM sessions WHERE user_id = ?").bind(me.id),
        env.DB.prepare("DELETE FROM scores WHERE user_id = ?").bind(me.id),
        env.DB.prepare("DELETE FROM replays WHERE user_id = ?").bind(me.id),
        env.DB.prepare("DELETE FROM stats WHERE user_id = ?").bind(me.id),
        env.DB.prepare("DELETE FROM friends WHERE user_id = ? OR friend_id = ?").bind(me.id, me.id),
        env.DB.prepare("DELETE FROM clan_members WHERE user_id = ?").bind(me.id),
        env.DB.prepare("DELETE FROM users WHERE id = ?").bind(me.id),
      ]);
      if (clan) await leaveClan(env, me.id, clan);
      return reply({ ok: true });
    }

    case "/api/scores/submit": {
      const user = await userForSession(env, body.session);
      if (!user) return reply({ error: "sign in again" }, 401);
      const md5 = String(body.md5 || "");
      if (!/^[0-9a-f]{32}$/.test(md5)) return reply({ error: "bad map" }, 400);
      const n = (k) => Math.max(0, Math.floor(Number(body[k]) || 0));
      const score = n("score");
      const mode = n("mode");
      const bad = implausible(body, n);
      if (bad) return reply({ error: bad }, 422);
      const last = await env.DB.prepare("SELECT MAX(played) AS t FROM scores WHERE user_id = ?").bind(user.id).first();
      if (last && last.t && Date.now() - last.t < MIN_GAP) return reply({ error: "slow down" }, 429);

      await env.DB.prepare(
        `INSERT INTO stats (user_id, mode, plays, total_score, max_combo) VALUES (?, ?, 1, ?, ?)
           ON CONFLICT (user_id, mode) DO UPDATE SET plays = plays + 1, total_score = total_score + excluded.total_score,
             max_combo = MAX(max_combo, excluded.max_combo)`,
      )
        .bind(user.id, mode, score, n("max_combo"))
        .run();

      const old = await env.DB.prepare("SELECT score FROM scores WHERE user_id = ? AND map_md5 = ?").bind(user.id, md5).first();
      const best = !old || score > old.score;
      let held = 0;
      if (best) {
        // a jump far past the player's usual pp waits for an admin to look at it
        const pp = Number(body.pp) || 0;
        const top = await env.DB.prepare("SELECT MAX(pp) AS p FROM scores WHERE user_id = ? AND mode = ? AND held = 0").bind(user.id, mode).first();
        if (pp > 300 && pp > (top && top.p ? top.p : 0) * 1.5 + 100) held = 1;
        const stmts = [
          env.DB.prepare(
            `INSERT OR REPLACE INTO scores (user_id, map_md5, beatmap_id, mode, score, acc, max_combo, mods, pp, rank,
               n300, n100, n50, miss, played, held) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
          ).bind(user.id, md5, n("beatmap_id"), mode, score, Math.min(1, Number(body.acc) || 0), n("max_combo"), n("mods"),
            pp, n("rank"), n("n300"), n("n100"), n("n50"), n("miss"), Date.now(), held),
        ];
        const replay = typeof body.replay === "string" ? body.replay : "";
        if (replay && replay.length < MAX_REPLAY * 1.4) {
          const bytes = Uint8Array.from(atob(replay), (c) => c.charCodeAt(0));
          stmts.push(env.DB.prepare("INSERT OR REPLACE INTO replays (user_id, map_md5, data) VALUES (?, ?, ?)").bind(user.id, md5, bytes));
        }
        await env.DB.batch(stmts);
        await mapStatus(env, md5).catch(() => null);
      }
      const ahead = await env.DB.prepare(
        "SELECT COUNT(*) AS c FROM scores s JOIN users u ON u.id = s.user_id WHERE map_md5 = ? AND score > ? AND u.restricted = 0 AND s.held = 0",
      )
        .bind(md5, best ? score : old.score)
        .first();
      return reply({ best, place: ahead.c + 1, held: !!held });
    }
    case "/api/scores/top": {
      const md5 = q("md5");
      const rows = await env.DB.prepare(
        `SELECT users.name, score, acc, max_combo, mods, pp, rank, n300, n100, n50, miss,
                EXISTS (SELECT 1 FROM replays r WHERE r.user_id = scores.user_id AND r.map_md5 = scores.map_md5) AS replay
           FROM scores JOIN users ON users.id = scores.user_id
          WHERE map_md5 = ? AND users.restricted = 0 AND scores.held = 0 ORDER BY score DESC LIMIT 50`,
      )
        .bind(md5)
        .all();
      const map = await env.DB.prepare("SELECT status FROM maps WHERE md5 = ?").bind(md5).first();
      return reply({ scores: rows.results, status: map ? map.status : null });
    }
    case "/api/replay": {
      const user = await byName(env, q("name"));
      if (!user) return reply({ error: "no such player" }, 404);
      const row = await env.DB.prepare("SELECT data FROM replays WHERE user_id = ? AND map_md5 = ?").bind(user.id, q("md5")).first();
      if (!row) return reply({ error: "no replay" }, 404);
      return new Response(new Uint8Array(row.data), { headers: { "content-type": "application/octet-stream" } });
    }
    case "/api/maps/status": {
      const s = await mapStatus(env, q("md5")).catch(() => null);
      return reply({ status: s ? s.status : null, title: s ? s.title : "" });
    }

    case "/api/users/top": {
      const list = await standings(env, modeOf(q("mode")));
      const page = Math.max(0, Math.floor(Number(q("page")) || 0));
      const slice = list.slice(page * 50, page * 50 + 50);
      // play counts and clan tags for the page shown
      for (const u of slice) {
        const st = await env.DB.prepare("SELECT plays FROM stats WHERE user_id = ? AND mode = ?").bind(u.id, modeOf(q("mode"))).first();
        const c = await clanOf(env, u.id);
        u.plays = st ? st.plays : 0;
        u.clan = c ? c.tag : "";
      }
      return reply({ users: slice, total: list.length });
    }
    case "/api/users/profile": {
      const mode = modeOf(q("mode"));
      const user = await byName(env, q("name"));
      if (!user) return reply({ error: "no such player" }, 404);
      const me = await userForSession(env, q("session")).catch(() => null);
      const list = await standings(env, mode);
      const at = list.find((u) => u.id === user.id);
      const st = await env.DB.prepare("SELECT plays, total_score, max_combo FROM stats WHERE user_id = ? AND mode = ?").bind(user.id, mode).first();
      const top = await env.DB.prepare(
        `SELECT s.map_md5 AS md5, s.beatmap_id, s.score, s.acc, s.pp, s.mods, s.rank, s.max_combo, m.title
           FROM scores s LEFT JOIN maps m ON m.md5 = s.map_md5
          WHERE s.user_id = ? AND s.mode = ? AND s.held = 0 ORDER BY s.pp DESC LIMIT 10`,
      )
        .bind(user.id, mode)
        .all();
      const clan = await clanOf(env, user.id);
      const friend = me ? await env.DB.prepare("SELECT 1 AS f FROM friends WHERE user_id = ? AND friend_id = ?").bind(me.id, user.id).first() : null;
      return reply({
        id: user.id,
        name: user.name,
        joined: user.created,
        restricted: !!user.restricted,
        rank: at ? at.rank : 0,
        pp: at ? at.pp : 0,
        acc: at ? at.acc : 0,
        ranked_score: at ? at.ranked_score : 0,
        plays: st ? st.plays : 0,
        total_score: st ? st.total_score : 0,
        max_combo: st ? st.max_combo : 0,
        clan: clan ? clan.tag : "",
        clan_name: clan ? clan.name : "",
        friend: !!friend,
        top: top.results,
      });
    }

    case "/api/friends/add":
    case "/api/friends/remove": {
      const me = await userForSession(env, body.session);
      if (!me) return reply({ error: "sign in again" }, 401);
      const other = await byName(env, body.name);
      if (!other || other.id === me.id) return reply({ error: "no such player" }, 404);
      if (url.pathname.endsWith("/add"))
        await env.DB.prepare("INSERT OR IGNORE INTO friends (user_id, friend_id) VALUES (?, ?)").bind(me.id, other.id).run();
      else await env.DB.prepare("DELETE FROM friends WHERE user_id = ? AND friend_id = ?").bind(me.id, other.id).run();
      return reply({ ok: true, friend: url.pathname.endsWith("/add") });
    }
    case "/api/friends": {
      const me = await userForSession(env, q("session"));
      if (!me) return reply({ error: "sign in again" }, 401);
      const rows = await env.DB.prepare("SELECT u.name FROM friends f JOIN users u ON u.id = f.friend_id WHERE f.user_id = ? ORDER BY u.name")
        .bind(me.id)
        .all();
      return reply({ friends: rows.results.map((r) => r.name) });
    }

    case "/api/clans/create": {
      const me = await userForSession(env, body.session);
      if (!me) return reply({ error: "sign in again" }, 401);
      const name = String(body.name || "").trim();
      const tag = String(body.tag || "").trim();
      if (!CLAN_OK.test(name)) return reply({ error: "clan names are 2 to 24 letters, digits, spaces, _ or -" }, 400);
      if (!TAG_OK.test(tag)) return reply({ error: "tags are 1 to 6 letters or digits" }, 400);
      if (await clanOf(env, me.id)) return reply({ error: "leave your clan first" }, 409);
      const taken = await env.DB.prepare("SELECT id FROM clans WHERE name = ? OR tag = ?").bind(name, tag).first();
      if (taken) return reply({ error: "that name or tag is taken" }, 409);
      const r = await env.DB.prepare("INSERT INTO clans (name, tag, owner, created) VALUES (?, ?, ?, ?)").bind(name, tag, me.id, Date.now()).run();
      await env.DB.prepare("INSERT INTO clan_members (user_id, clan_id) VALUES (?, ?)").bind(me.id, r.meta.last_row_id).run();
      return reply({ ok: true, tag, name });
    }
    case "/api/clans/join": {
      const me = await userForSession(env, body.session);
      if (!me) return reply({ error: "sign in again" }, 401);
      if (await clanOf(env, me.id)) return reply({ error: "leave your clan first" }, 409);
      const clan = await env.DB.prepare("SELECT id, name, tag FROM clans WHERE tag = ?").bind(String(body.tag || "").trim()).first();
      if (!clan) return reply({ error: "no clan has that tag" }, 404);
      await env.DB.prepare("INSERT INTO clan_members (user_id, clan_id) VALUES (?, ?)").bind(me.id, clan.id).run();
      return reply({ ok: true, tag: clan.tag, name: clan.name });
    }
    case "/api/clans/leave": {
      const me = await userForSession(env, body.session);
      if (!me) return reply({ error: "sign in again" }, 401);
      const clan = await clanOf(env, me.id);
      if (!clan) return reply({ error: "you're not in a clan" }, 409);
      await env.DB.prepare("DELETE FROM clan_members WHERE user_id = ?").bind(me.id).run();
      await leaveClan(env, me.id, clan);
      return reply({ ok: true });
    }
    case "/api/clans/top": {
      // a clan's pp is its members' added up
      const list = await standings(env, modeOf(q("mode")));
      const pp = new Map(list.map((u) => [u.id, u.pp]));
      const rows = (await env.DB.prepare("SELECT c.id, c.name, c.tag, cm.user_id FROM clans c JOIN clan_members cm ON cm.clan_id = c.id").all()).results;
      const clans = new Map();
      for (const r of rows) {
        let c = clans.get(r.id);
        if (!c) clans.set(r.id, (c = { name: r.name, tag: r.tag, members: 0, pp: 0 }));
        c.members++;
        c.pp += pp.get(r.user_id) || 0;
      }
      const out = [...clans.values()].sort((a, b) => b.pp - a.pp).slice(0, 50);
      out.forEach((c, i) => (c.rank = i + 1));
      return reply({ clans: out });
    }
    case "/api/clans/info": {
      const clan = await env.DB.prepare("SELECT id, name, tag, owner, created FROM clans WHERE tag = ?").bind(q("tag")).first();
      if (!clan) return reply({ error: "no clan has that tag" }, 404);
      const members = await env.DB.prepare("SELECT u.name FROM clan_members cm JOIN users u ON u.id = cm.user_id WHERE cm.clan_id = ? ORDER BY u.name")
        .bind(clan.id)
        .all();
      return reply({ name: clan.name, tag: clan.tag, created: clan.created, members: members.results.map((m) => m.name) });
    }
  }
  return reply({ error: "no such call" }, 404);
}

// whoever leaves, the clan carries on with someone else in charge, or goes
async function leaveClan(env, userId, clan) {
  const next = await env.DB.prepare("SELECT user_id FROM clan_members WHERE clan_id = ? LIMIT 1").bind(clan.id).first();
  if (!next) await env.DB.prepare("DELETE FROM clans WHERE id = ?").bind(clan.id).run();
  else if (clan.owner === userId) await env.DB.prepare("UPDATE clans SET owner = ? WHERE id = ?").bind(next.user_id, clan.id).run();
}
