// ProjectCadence server: accounts and global scores (D1), and multiplayer rooms
// (one Durable Object, all in memory). Plain JSON over HTTPS; consoles poll the
// room calls about once a second.
//
// Accounts
//   POST /api/account/register {name, password}             -> {session, name}
//   POST /api/account/login    {name, password}             -> {session, name}
//   POST /api/scores/submit    {session, md5, beatmap_id, mode, score, acc, max_combo,
//                               mods, pp, rank, n300, n100, n50, miss}  -> {best, place}
//   GET  /api/scores/top?md5=                               -> {scores: [...]}
//
// Rooms
//   POST /api/hello            {name, session?}        -> {token, id, now}  (a session names you)
//   GET  /api/rooms?token=                             -> {rooms, online, now}
//   POST /api/rooms            {token, name, password?}
//   GET  /api/room?token=                              -> {room, online, now}
//   POST /api/join             {token, room, password?}
//   POST /api/leave / bye      {token}
//   POST /api/map              {token, beatmap_id, set_id, md5, title, mods}   host
//   POST /api/settings         {token, teams, freemod}                          host
//   POST /api/team             {token, team}           0 red, 1 blue
//   POST /api/mods             {token, mods}           your own, with freemod on
//   POST /api/chat             {token, text}
//   POST /api/ready            {token, ready, has_map}
//   POST /api/start            {token}                                          host
//   POST /api/score            {token, score, combo, max_combo, acc, n300, n100, n50, miss, failed, done}
//
// Rooms live in one object whose name changes when a deploy has to start everyone
// fresh (a running object keeps its old code until it sleeps).

const ROOMS_OBJECT = "main-4";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/api/account/") || url.pathname.startsWith("/api/scores/"))
      return accounts(request, env, url).catch((e) => reply({ error: "server error" }, 500));
    if (!url.pathname.startsWith("/api/"))
      return new Response("ProjectCadence\n", { headers: { "content-type": "text/plain" } });
    return env.ROOMS.get(env.ROOMS.idFromName(ROOMS_OBJECT)).fetch(request);
  },
};

/* ================= accounts and scores ================= */

const NAME_OK = /^[A-Za-z0-9_\- ]{3,20}$/;

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
  return env.DB.prepare("SELECT users.id, users.name FROM sessions JOIN users ON users.id = sessions.user_id WHERE token = ?")
    .bind(String(session))
    .first();
}

async function accounts(request, env, url) {
  const body = request.method === "POST" ? await request.json().catch(() => ({})) : {};

  switch (url.pathname) {
    case "/api/account/register": {
      const name = String(body.name || "").trim();
      const password = String(body.password || "");
      if (!NAME_OK.test(name)) return reply({ error: "names are 3 to 20 letters, digits, spaces, _ or -" }, 400);
      if (password.length < 6) return reply({ error: "the password needs at least 6 characters" }, 400);
      const taken = await env.DB.prepare("SELECT id FROM users WHERE name = ?").bind(name).first();
      if (taken) return reply({ error: "that name is taken" }, 409);
      const salt = crypto.randomUUID();
      const res = await env.DB.prepare("INSERT INTO users (name, pass_hash, salt, created) VALUES (?, ?, ?, ?)")
        .bind(name, await hash(password, salt), salt, Date.now())
        .run();
      const id = res.meta.last_row_id;
      return reply({ session: await newSession(env, id), name });
    }
    case "/api/account/login": {
      const user = await env.DB.prepare("SELECT id, name, pass_hash, salt FROM users WHERE name = ?").bind(String(body.name || "").trim()).first();
      if (!user || (await hash(String(body.password || ""), user.salt)) !== user.pass_hash)
        return reply({ error: "wrong name or password" }, 401);
      return reply({ session: await newSession(env, user.id), name: user.name });
    }
    case "/api/scores/submit": {
      const user = await userForSession(env, body.session);
      if (!user) return reply({ error: "sign in again" }, 401);
      const md5 = String(body.md5 || "");
      if (!/^[0-9a-f]{32}$/.test(md5)) return reply({ error: "bad map" }, 400);
      const n = (k) => Math.max(0, Math.floor(Number(body[k]) || 0));
      const score = n("score");
      const old = await env.DB.prepare("SELECT score FROM scores WHERE user_id = ? AND map_md5 = ?").bind(user.id, md5).first();
      const best = !old || score > old.score;
      if (best) {
        await env.DB.prepare(
          `INSERT OR REPLACE INTO scores (user_id, map_md5, beatmap_id, mode, score, acc, max_combo, mods, pp, rank,
             n300, n100, n50, miss, played) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
        )
          .bind(user.id, md5, n("beatmap_id"), n("mode"), score, Math.min(1, Number(body.acc) || 0), n("max_combo"), n("mods"),
            Number(body.pp) || 0, n("rank"), n("n300"), n("n100"), n("n50"), n("miss"), Date.now())
          .run();
      }
      const ahead = await env.DB.prepare("SELECT COUNT(*) AS c FROM scores WHERE map_md5 = ? AND score > ?")
        .bind(md5, best ? score : old.score)
        .first();
      return reply({ best, place: ahead.c + 1 });
    }
    case "/api/scores/top": {
      const md5 = String(url.searchParams.get("md5") || "");
      const rows = await env.DB.prepare(
        `SELECT users.name, score, acc, max_combo, mods, pp, rank, n300, n100, n50, miss
           FROM scores JOIN users ON users.id = scores.user_id WHERE map_md5 = ? ORDER BY score DESC LIMIT 50`,
      )
        .bind(md5)
        .all();
      return reply({ scores: rows.results });
    }
  }
  return reply({ error: "no such call" }, 404);
}

/* ================= rooms ================= */

const PLAYER_TIMEOUT = 20000; // ms without a poll before someone's dropped
const START_DELAY = 4000;     // time everyone gets to load the map
const MAX_PLAYERS = 16;
const CHAT_KEPT = 40;

export class Rooms {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.players = new Map(); // token -> player
    this.rooms = new Map();   // id -> room
    this.nextRoom = 1;
    this.nextPlayer = 1;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const now = Date.now();
    this.sweep(now);

    let body = {};
    if (request.method === "POST") {
      try {
        body = await request.json();
      } catch {
        return reply({ error: "bad json" }, 400);
      }
    }
    const token = body.token || url.searchParams.get("token") || "";
    const me = this.players.get(token);
    if (me) me.seen = now;

    switch (url.pathname) {
      case "/api/hello": {
        let name = String(body.name || "player").slice(0, 24).replace(/[^\x20-\x7e]/g, "?");
        let account = false;
        // signed-in players get their account name, so nobody can wear it
        const user = await userForSession(this.env, body.session).catch(() => null);
        if (user) {
          name = user.name;
          account = true;
        }
        const p = { id: this.nextPlayer++, name, account, token: crypto.randomUUID(), seen: now, room: 0 };
        this.players.set(p.token, p);
        return reply({ token: p.token, id: p.id, name, now });
      }
      case "/api/rooms":
        if (request.method === "GET")
          return reply({ rooms: [...this.rooms.values()].map(summary), online: this.players.size, now });
        if (!me) return reply({ error: "say hello first" }, 401);
        return this.create(me, body, now);
    }

    if (!me) return reply({ error: "unknown player, say hello again" }, 401);
    const room = this.rooms.get(me.room);

    switch (url.pathname) {
      case "/api/room":
        return reply({ room: room ? view(room) : null, online: this.players.size, now });
      case "/api/join": {
        const target = this.rooms.get(Number(body.room));
        if (!target) return reply({ error: "that room is gone" }, 404);
        if (target.members.length >= MAX_PLAYERS) return reply({ error: "room is full" }, 409);
        if (target.password && String(body.password || "") !== target.password) return reply({ error: "wrong room password" }, 403);
        this.leave(me);
        me.room = target.id;
        target.members.push(member(me, target));
        say(target, null, `${me.name} joined`);
        return reply({ room: view(target), now });
      }
      case "/api/leave":
        this.leave(me);
        return reply({ ok: true, now });
      case "/api/bye":
        this.leave(me);
        this.players.delete(me.token);
        return reply({ ok: true, now });
    }

    if (!room) return reply({ error: "not in a room" }, 409);
    const m = room.members.find((x) => x.id === me.id);
    const host = room.host === me.id;

    switch (url.pathname) {
      case "/api/map":
        if (!host) return reply({ error: "only the host picks" }, 403);
        if (room.state === "playing") return reply({ error: "wait for the round to end" }, 409);
        room.map = {
          beatmap_id: Number(body.beatmap_id) || 0,
          set_id: Number(body.set_id) || 0,
          md5: String(body.md5 || "").slice(0, 32),
          title: String(body.title || "").slice(0, 120),
        };
        room.mods = Number(body.mods) || 0;
        room.state = "lobby";
        for (const x of room.members) x.ready = false;
        return reply({ room: view(room), now });
      case "/api/settings":
        if (!host) return reply({ error: "only the host changes that" }, 403);
        if (room.state === "playing") return reply({ error: "wait for the round to end" }, 409);
        room.teams = !!body.teams;
        room.freemod = !!body.freemod;
        for (const x of room.members) x.ready = false;
        say(room, null, `${room.teams ? "teams" : "everyone for themselves"}, ${room.freemod ? "free mods" : "the host's mods"}`);
        return reply({ room: view(room), now });
      case "/api/team":
        m.team = Number(body.team) === 1 ? 1 : 0;
        return reply({ room: view(room), now });
      case "/api/mods":
        m.mods = Number(body.mods) || 0;
        return reply({ room: view(room), now });
      case "/api/chat": {
        const text = String(body.text || "").replace(/[^\x20-\x7e]/g, "").trim().slice(0, 120);
        if (text) say(room, me.name, text);
        return reply({ room: view(room), now });
      }
      case "/api/ready":
        m.ready = !!body.ready;
        m.has_map = body.has_map !== false;
        return reply({ room: view(room), now });
      case "/api/start":
        if (!host) return reply({ error: "only the host starts" }, 403);
        if (!room.map) return reply({ error: "pick a map first" }, 409);
        if (room.members.some((x) => !x.ready && x.id !== room.host)) return reply({ error: "not everyone is ready" }, 409);
        room.state = "playing";
        room.round++;
        room.start_at = now + START_DELAY;
        for (const x of room.members) Object.assign(x, fresh());
        return reply({ room: view(room), now });
      case "/api/score":
        if (room.state !== "playing") return reply({ room: view(room), now });
        for (const k of ["score", "combo", "max_combo", "n300", "n100", "n50", "miss"]) m[k] = Number(body[k]) || 0;
        m.acc = Number(body.acc) || 0;
        m.failed = !!body.failed;
        m.done = !!body.done;
        if (room.members.every((x) => x.done)) this.finish(room);
        return reply({ room: view(room), now });
    }
    return reply({ error: "no such call" }, 404);
  }

  create(me, body, now) {
    this.leave(me);
    const room = {
      id: this.nextRoom++,
      name: String(body.name || `${me.name}'s room`).slice(0, 40),
      password: String(body.password || "").slice(0, 32),
      host: me.id,
      members: [],
      map: null,
      mods: 0,
      teams: false,
      freemod: false,
      state: "lobby",
      round: 0,
      start_at: 0,
      chat: [],
      next_msg: 1,
    };
    me.room = room.id;
    room.members.push(member(me, room));
    this.rooms.set(room.id, room);
    return reply({ room: view(room), now });
  }

  finish(room) {
    room.state = "results";
    for (const x of room.members) x.ready = false;
    if (room.teams) {
      const total = [0, 0];
      for (const x of room.members) total[x.team] += x.score;
      say(room, null, total[0] === total[1] ? "the teams tied" : `${total[0] > total[1] ? "red" : "blue"} team wins, ${Math.max(...total)} to ${Math.min(...total)}`);
    }
  }

  leave(p) {
    const room = this.rooms.get(p.room);
    p.room = 0;
    if (!room) return;
    room.members = room.members.filter((x) => x.id !== p.id);
    if (room.members.length === 0) {
      this.rooms.delete(room.id);
      return;
    }
    say(room, null, `${p.name} left`);
    if (room.host === p.id) {
      room.host = room.members[0].id;
      say(room, null, `${room.members[0].name} is the host now`);
    }
    // a round can't wait on someone who left
    if (room.state === "playing" && room.members.every((x) => x.done)) this.finish(room);
  }

  sweep(now) {
    for (const p of this.players.values()) {
      if (now - p.seen > PLAYER_TIMEOUT) {
        this.leave(p);
        this.players.delete(p.token);
      }
    }
  }
}

function say(room, from, text) {
  room.chat.push({ id: room.next_msg++, from: from || "", text, at: Date.now() });
  if (room.chat.length > CHAT_KEPT) room.chat.shift();
}

function fresh() {
  return { score: 0, combo: 0, max_combo: 0, acc: 1, n300: 0, n100: 0, n50: 0, miss: 0, failed: false, done: false };
}

function member(p, room) {
  // new players join the smaller team
  const blue = room.members.filter((x) => x.team === 1).length;
  const team = blue < room.members.length - blue ? 1 : 0;
  return { id: p.id, name: p.name, account: p.account, ready: false, has_map: true, team, mods: 0, ...fresh() };
}

function summary(r) {
  return { id: r.id, name: r.name, players: r.members.length, state: r.state, locked: !!r.password, map: r.map ? r.map.title : "" };
}

function view(r) {
  return {
    ...summary(r),
    host: r.host,
    mods: r.mods,
    teams: r.teams,
    freemod: r.freemod,
    map: r.map,
    round: r.round,
    start_at: r.start_at,
    members: r.members,
    chat: r.chat,
  };
}

function reply(obj, status = 200) {
  return new Response(JSON.stringify(obj), { status, headers: { "content-type": "application/json" } });
}
