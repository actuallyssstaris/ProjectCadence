// ProjectCadence multiplayer rooms. (The object name changes when a deploy has to
// start everyone fresh: old objects keep running the old code until they sleep.) Plain JSON over HTTPS, polled about once a
// second by each console; everything is in memory and goes when the object sleeps.
//
//   POST /api/hello            {name}                  -> {token, id, now}
//   GET  /api/rooms                                    -> {rooms: [...], now}
//   POST /api/rooms            {token, name}           -> room
//   GET  /api/room?token=                              -> room (yours)
//   POST /api/join             {token, room}           -> room
//   POST /api/leave            {token}
//   POST /api/bye              {token}                 leaving the server altogether
//   POST /api/map              {token, beatmap_id, set_id, md5, title, mods}   host only
//   POST /api/ready            {token, ready, has_map}
//   POST /api/start            {token}                 host only, everyone ready
//   POST /api/score            {token, score, combo, max_combo, acc, n300, n100, n50, miss, failed, done}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (!url.pathname.startsWith("/api/"))
      return new Response("ProjectCadence rooms\n", { headers: { "content-type": "text/plain" } });
    const stub = env.ROOMS.get(env.ROOMS.idFromName("main-3"));
    return stub.fetch(request);
  },
};

const PLAYER_TIMEOUT = 20000; // ms without a poll before someone's dropped
const START_DELAY = 4000;     // time everyone gets to load the map
const MAX_PLAYERS = 16;

export class Rooms {
  constructor(state) {
    this.state = state;
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
        const name = String(body.name || "player").slice(0, 24).replace(/[^\x20-\x7e]/g, "?");
        const p = { id: this.nextPlayer++, name, token: crypto.randomUUID(), seen: now, room: 0 };
        this.players.set(p.token, p);
        return reply({ token: p.token, id: p.id, now });
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
        this.leave(me);
        me.room = target.id;
        target.members.push(member(me));
        return reply({ room: view(target), now });
      }
      case "/api/leave":
        this.leave(me);
        return reply({ ok: true, now });
      case "/api/bye":
        // gone for good: out of the count now rather than after the timeout
        this.leave(me);
        this.players.delete(me.token);
        return reply({ ok: true, now });
    }

    if (!room) return reply({ error: "not in a room" }, 409);
    const m = room.members.find((x) => x.id === me.id);

    switch (url.pathname) {
      case "/api/map":
        if (room.host !== me.id) return reply({ error: "only the host picks" }, 403);
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
      case "/api/ready":
        m.ready = !!body.ready;
        m.has_map = body.has_map !== false;
        return reply({ room: view(room), now });
      case "/api/start":
        if (room.host !== me.id) return reply({ error: "only the host starts" }, 403);
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
        if (room.members.every((x) => x.done)) {
          room.state = "results";
          for (const x of room.members) x.ready = false;
        }
        return reply({ room: view(room), now });
    }
    return reply({ error: "no such call" }, 404);
  }

  create(me, body, now) {
    this.leave(me);
    const room = {
      id: this.nextRoom++,
      name: String(body.name || `${me.name}'s room`).slice(0, 40),
      host: me.id,
      members: [],
      map: null,
      mods: 0,
      state: "lobby",
      round: 0,
      start_at: 0,
    };
    me.room = room.id;
    room.members.push(member(me));
    this.rooms.set(room.id, room);
    return reply({ room: view(room), now });
  }

  leave(p) {
    const room = this.rooms.get(p.room);
    p.room = 0;
    if (!room) return;
    room.members = room.members.filter((x) => x.id !== p.id);
    if (room.members.length === 0) this.rooms.delete(room.id);
    else if (room.host === p.id) room.host = room.members[0].id;
    // a round can't wait on someone who left
    if (room.state === "playing" && room.members.every((x) => x.done)) room.state = "results";
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

function fresh() {
  return { score: 0, combo: 0, max_combo: 0, acc: 1, n300: 0, n100: 0, n50: 0, miss: 0, failed: false, done: false };
}

function member(p) {
  return { id: p.id, name: p.name, ready: false, has_map: true, ...fresh() };
}

function summary(r) {
  return { id: r.id, name: r.name, players: r.members.length, state: r.state, map: r.map ? r.map.title : "" };
}

function view(r) {
  return { ...summary(r), host: r.host, mods: r.mods, map: r.map, round: r.round, start_at: r.start_at, members: r.members };
}

function reply(obj, status = 200) {
  return new Response(JSON.stringify(obj), { status, headers: { "content-type": "application/json" } });
}
