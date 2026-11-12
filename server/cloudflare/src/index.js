// ProjectCadence server: accounts, scores, rankings and clans in D1 (accounts.js),
// multiplayer rooms, presence and spectating in one Durable Object (below), and
// a small website (web.js). Plain JSON over HTTPS.
//
// Accounts and scores
//   POST /api/account/register | login   {name, password}                -> {session, name}
//   POST /api/account/password           {session, password, new_password} -> {session}
//   POST /api/account/delete             {session, password}
//   POST /api/scores/submit   {session, md5, beatmap_id, mode, score, acc, max_combo, mods, pp, rank,
//                              n300, n100, n50, miss, replay?}            -> {best, place, held}
//   GET  /api/scores/top?md5=                                             -> {scores, status}
//   GET  /api/replay?md5=&name=                                           -> LZMA'd frame text
//   GET  /api/maps/status?md5=                                            -> {status, title}
//   GET  /api/users/top?mode=&page=     GET /api/users/profile?name=&mode=&session=
//   POST /api/friends/add | remove {session, name}     GET /api/friends?session=
//   POST /api/clans/create {session, name, tag} | join {session, tag} | leave {session}
//   GET  /api/clans/top?mode=   GET /api/clans/info?tag=
//
// Rooms (the room object)
//   POST /api/hello            {name, session?}        -> {token, id, now}  (a session names you)
//   GET  /api/rooms?token=                             -> {rooms, lobby, online, watchers, now}
//   POST /api/rooms            {token, name, password?}
//   GET  /api/room?token=                              -> {room, lobby, online, watchers, now}
//   POST /api/join             {token, room, password?}
//   POST /api/leave / bye      {token}
//   POST /api/map              {token, beatmap_id, set_id, md5, title, mods}   host
//   POST /api/settings         {token, teams, freemod, win}                     host
//   POST /api/host | kick      {token, member}                                  host
//   POST /api/team             {token, team}           0 red, 1 blue
//   POST /api/mods             {token, mods}           your own, with freemod on
//   POST /api/chat | lobby     {token, text}           room / everyone; admins can type !commands
//   POST /api/ready            {token, ready, has_map}
//   POST /api/start            {token}                                          host
//   POST /api/score            {token, score, combo, max_combo, acc, n300, n100, n50, miss, failed, done}
//   POST /api/status           {token, action, text, md5, mode, mods, beatmap_id}
//   GET  /api/online?token=                            -> {players}
//   POST /api/spectate         {token, id}             0 stops
//   POST /api/frames           {token, frames: [[t, x, y, keys]...], action}   while someone watches
//   GET  /api/watch?token=&after=                      -> {frames, seq, action}
//
// Rooms live in one object whose name changes when a deploy has to start everyone
// fresh (a running object keeps its old code until it sleeps).

import { accounts, adminCommand, reply, userForSession } from "./accounts.js";
import { page } from "./web.js";

const ROOMS_OBJECT = "main-5";
const ROOM_CALLS = new Set([
  "/api/hello", "/api/rooms", "/api/room", "/api/join", "/api/leave", "/api/bye", "/api/map", "/api/settings", "/api/host",
  "/api/kick", "/api/team", "/api/mods", "/api/chat", "/api/lobby", "/api/ready", "/api/start", "/api/score", "/api/status",
  "/api/online", "/api/spectate", "/api/frames", "/api/watch",
]);

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (ROOM_CALLS.has(url.pathname)) return env.ROOMS.get(env.ROOMS.idFromName(ROOMS_OBJECT)).fetch(request);
    if (url.pathname.startsWith("/api/"))
      return accounts(request, env, url).catch((e) => reply({ error: "server error" }, 500));
    return page();
  },
};

/* ================= rooms ================= */

const PLAYER_TIMEOUT = 20000; // ms without a poll before someone's dropped
const START_DELAY = 4000;     // time everyone gets to load the map
const MAX_PLAYERS = 16;
const CHAT_KEPT = 40;
const FRAMES_KEPT = 6000;     // about two minutes of a play, for late spectators

const clean = (t) => String(t || "").replace(/[^\x20-\x7e]/g, "").trim().slice(0, 120);

export class Rooms {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.players = new Map(); // token -> player
    this.rooms = new Map();   // id -> room
    this.nextRoom = 1;
    this.nextPlayer = 1;
    this.lobby = { chat: [], next_msg: 1 };
  }

  byId(id) {
    for (const p of this.players.values()) if (p.id === id) return p;
    return null;
  }

  watchers(p) {
    let n = 0;
    for (const x of this.players.values()) if (x.watching === p.id) n++;
    return n;
  }

  // chat lines, with the silence and admin commands handled first
  async speak(me, target, text) {
    text = clean(text);
    if (!text) return null;
    if (me.user && me.user.silenced_until > Date.now()) return "you're silenced for now";
    if (text.startsWith("!")) {
      const said = await adminCommand(this.env, me.user, text).catch(() => "that didn't work");
      if (said) {
        say(target, null, said);
        // a fresh restrict or silence applies to people already online
        for (const p of this.players.values()) if (p.user) p.user = (await userForSession(this.env, p.session).catch(() => null)) || p.user;
        return null;
      }
    }
    say(target, me.name, text);
    return null;
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
        // signed-in players get their account name, so nobody can wear it
        const user = await userForSession(this.env, body.session).catch(() => null);
        if (user) name = user.name;
        const p = {
          id: this.nextPlayer++, name, account: !!user, user, session: body.session || "", token: crypto.randomUUID(),
          seen: now, room: 0, status: { action: 0, text: "", md5: "", mode: 0, mods: 0, beatmap_id: 0 },
          watching: 0, frames: [], seq: 0, action: 0,
        };
        this.players.set(p.token, p);
        return reply({ token: p.token, id: p.id, name, now });
      }
      case "/api/rooms":
        if (request.method === "GET")
          return reply({
            rooms: [...this.rooms.values()].map(summary), lobby: this.lobby.chat, online: this.players.size,
            watchers: me ? this.watchers(me) : 0, now,
          });
        if (!me) return reply({ error: "say hello first" }, 401);
        return this.create(me, body, now);
    }

    if (!me) return reply({ error: "unknown player, say hello again" }, 401);
    const room = this.rooms.get(me.room);

    switch (url.pathname) {
      case "/api/room":
        return reply({ room: room ? view(room) : null, lobby: this.lobby.chat, online: this.players.size, watchers: this.watchers(me), now });
      case "/api/lobby": {
        const err = await this.speak(me, this.lobby, body.text);
        return reply(err ? { error: err, lobby: this.lobby.chat, now } : { lobby: this.lobby.chat, now });
      }
      case "/api/status":
        me.status = {
          action: Number(body.action) || 0, text: clean(body.text), md5: String(body.md5 || "").slice(0, 32),
          mode: Number(body.mode) || 0, mods: Number(body.mods) || 0, beatmap_id: Number(body.beatmap_id) || 0,
        };
        // a new play starts the spectators' buffer over
        if (me.status.action === 2) {
          me.frames = [];
          me.action = 1;
        }
        return reply({ ok: true, watchers: this.watchers(me), now });
      case "/api/online":
        return reply({
          players: [...this.players.values()].map((p) => ({ id: p.id, name: p.name, account: p.account, room: p.room, ...p.status })),
          now,
        });
      case "/api/spectate": {
        const id = Number(body.id) || 0;
        const target = id ? this.byId(id) : null;
        if (id && !target) return reply({ error: "they went offline" }, 404);
        me.watching = target ? target.id : 0;
        return reply({ ok: true, now });
      }
      case "/api/frames": {
        // [t, x, y, keys] each, numbered so watchers can ask for what's new
        const frames = Array.isArray(body.frames) ? body.frames.slice(0, 2000) : [];
        for (const f of frames) if (Array.isArray(f) && f.length === 4) me.frames.push([++me.seq, ...f.map(Number)]);
        if (me.frames.length > FRAMES_KEPT) me.frames.splice(0, me.frames.length - FRAMES_KEPT);
        if (body.action) me.action = Number(body.action);
        return reply({ ok: true, watchers: this.watchers(me), now });
      }
      case "/api/watch": {
        const target = this.byId(me.watching);
        if (!target) return reply({ error: "they went offline", frames: [], seq: 0, action: 7, now });
        const after = Number(url.searchParams.get("after")) || 0;
        const frames = target.frames.filter((f) => f[0] > after).slice(0, 3000);
        return reply({ frames, seq: frames.length ? frames[frames.length - 1][0] : after, action: target.action, now });
      }
      case "/api/join": {
        const target = this.rooms.get(Number(body.room));
        if (!target) return reply({ error: "that room is gone" }, 404);
        if (target.members.length >= MAX_PLAYERS) return reply({ error: "room is full" }, 409);
        if (target.password && String(body.password || "") !== target.password) return reply({ error: "wrong room password" }, 403);
        if (target.kicked.has(me.id)) return reply({ error: "the host sent you out of that room" }, 403);
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
        if (body.win !== undefined) room.win = Math.min(2, Math.max(0, Number(body.win) || 0));
        for (const x of room.members) x.ready = false;
        say(room, null, `${room.teams ? "teams" : "everyone for themselves"}, ${room.freemod ? "free mods" : "the host's mods"}, win by ${WIN[room.win]}`);
        return reply({ room: view(room), now });
      case "/api/host":
      case "/api/kick": {
        if (!host) return reply({ error: "only the host can" }, 403);
        const target = room.members.find((x) => x.id === Number(body.member));
        if (!target || target.id === me.id) return reply({ room: view(room), now });
        if (url.pathname === "/api/host") {
          room.host = target.id;
          say(room, null, `${target.name} is the host now`);
        } else {
          room.kicked.add(target.id);
          const p = this.byId(target.id);
          if (p) this.leave(p);
          say(room, null, `${target.name} was sent out`);
        }
        return reply({ room: view(room), now });
      }
      case "/api/team":
        m.team = Number(body.team) === 1 ? 1 : 0;
        return reply({ room: view(room), now });
      case "/api/mods":
        m.mods = Number(body.mods) || 0;
        return reply({ room: view(room), now });
      case "/api/chat": {
        const err = await this.speak(me, room, body.text);
        return reply(err ? { error: err, room: view(room), now } : { room: view(room), now });
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
      win: 0,
      kicked: new Set(),
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
    const value = (x) => (room.win === 1 ? x.acc : room.win === 2 ? x.max_combo : x.score);
    if (room.teams) {
      const total = [0, 0], count = [0, 0];
      for (const x of room.members) {
        total[x.team] += value(x);
        count[x.team]++;
      }
      // accuracy is averaged per team, the rest added up
      if (room.win === 1) for (const t of [0, 1]) total[t] = count[t] ? total[t] / count[t] : 0;
      const show = (v) => (room.win === 1 ? (v * 100).toFixed(2) + "%" : Math.round(v));
      say(room, null, total[0] === total[1] ? "the teams tied" : `${total[0] > total[1] ? "red" : "blue"} team wins, ${show(Math.max(...total))} to ${show(Math.min(...total))}`);
    } else if (room.members.length > 1) {
      const best = [...room.members].sort((a, b) => value(b) - value(a))[0];
      say(room, null, `${best.name} wins on ${WIN[room.win]}`);
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

const WIN = ["score", "accuracy", "combo"];

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
    win: r.win,
    map: r.map,
    round: r.round,
    start_at: r.start_at,
    members: r.members,
    chat: r.chat,
  };
}
