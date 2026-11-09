// osu!catch: fruit falls, a catcher moves along the bottom to meet it. Sizes,
// speeds and the hyper-dash rule follow osu!'s catch ruleset
// (https://github.com/ppy/osu), MIT licence; the drawing is our own.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Scoring;
using Osu5.Util;

namespace Osu5.Play
{
    public sealed class CatchPlayer : IGame
    {
        enum Kind { Fruit, Droplet, Tiny, Banana }

        sealed class Thing
        {
            public Kind Kind;
            public double Time;
            public float X;
            public int Combo;          // colour index
            public bool Done, Caught;
            public bool Hyper;         // catching it flings you to the next one
            public float HyperTo;
            public double HyperAt;
            public Samples Samples;
        }

        public ScoreProcessor Score { get; } = new ScoreProcessor { Ruleset = 2 };
        public Queue<SoundEvent> Sounds { get; } = new Queue<SoundEvent>();

        const float walk_speed = 0.5f, dash_speed = 1.0f; // playfield px per ms
        const float play_x = 320, play_scale = 2.5f, catcher_y = 920;
        const int key_left = 1, key_right = 2, key_dash = 4;

        readonly Beatmap map;
        readonly List<Thing> things = new List<Thing>();
        readonly HealthProcessor health;
        readonly float halfWidth;      // half the catch range, playfield px
        readonly float fruitRadius;
        readonly double preempt;
        readonly bool autoplay, noFail, suddenDeath, perfectMod, hidden;
        readonly double endTime;
        static readonly uint[] colours = { 0xFFFF6B6B, 0xFFFFC34D, 0xFF7CD67C, 0xFF5AB4FF, 0xFFC08CFF };

        float catcherX = 256;
        bool dashing, facingLeft;
        float hyperMult = 1, hyperTarget;
        bool hyperOn;
        double lastTime = double.NaN;
        int first, autoNext;
        PlayState state = PlayState.Intro;

        public CatchPlayer(Beatmap m, Mods mods)
        {
            map = m;
            float scale = 1 - 0.7f * (m.Difficulty.CircleSize - 5) / 5;
            halfWidth = 106.75f * Math.Abs(scale) * 0.8f / 2;
            fruitRadius = 32 * scale;
            preempt = Difficulty.Range(m.Difficulty.ApproachRate, 1800, 1200, 450);

            autoplay = (mods & Mods.Auto) != 0;
            noFail = (mods & Mods.NoFail) != 0;
            suddenDeath = (mods & Mods.SuddenDeath) != 0;
            perfectMod = (mods & Mods.Perfect) != 0;
            hidden = (mods & Mods.Hidden) != 0;
            Score.Multiplier = ModRules.ScoreMultiplier(mods);

            var rng = new Random(1337);
            foreach (var h in m.HitObjects)
            {
                int combo = h.ComboIndex;
                switch (h)
                {
                    case Slider s:
                        addStream(s, combo);
                        break;
                    case Spinner sp:
                        // bananas, a burst of them across the screen
                        int n = Math.Max(1, (int)(sp.Duration / 100));
                        for (int i = 0; i < n; i++)
                            things.Add(new Thing { Kind = Kind.Banana, Time = sp.StartTime + i * sp.Duration / n, X = (float)rng.NextDouble() * 512, Samples = sp.Samples });
                        break;
                    default:
                        things.Add(new Thing { Kind = Kind.Fruit, Time = h.StartTime, X = h.Position.X, Combo = combo, Samples = h.Samples });
                        break;
                }
            }
            things.Sort((a, b) => a.Time.CompareTo(b.Time));
            markHyperDashes();

            var maxes = new List<(double Time, HitResult Max)>();
            foreach (var t in things)
            {
                var r = maxFor(t.Kind);
                if (r != HitResult.None)
                    maxes.Add((t.Time, r));
            }
            var maxResults = new List<HitResult>(maxes.Count);
            foreach (var x in maxes) maxResults.Add(x.Max);
            Score.SetMaximums(maxResults);
            health = new HealthProcessor(m, maxes);
            endTime = m.LastObjectEndTime;
        }

        static HitResult maxFor(Kind k) => k switch
        {
            Kind.Fruit => HitResult.Great,
            Kind.Droplet => HitResult.LargeTickHit,
            Kind.Tiny => HitResult.SmallTickHit,
            _ => HitResult.None,
        };

        // a slider is a juice stream: fruit at the ends and turns, droplets on the
        // ticks, tiny droplets in between
        void addStream(Slider s, int combo)
        {
            float xAt(double t) => Math.Clamp(s.Position.X + s.CurvePositionAt(Math.Clamp((t - s.StartTime) / s.Duration, 0, 1)).X, 0, 512);

            var points = new List<(double Time, Kind Kind)> { (s.StartTime, Kind.Fruit) };
            foreach (var n in s.Nested)
            {
                if (n.Kind == NestedKind.Head) continue;
                points.Add((n.Kind == NestedKind.Tick ? n.Time : n.Kind == NestedKind.Tail ? s.EndTime : n.Time,
                            n.Kind == NestedKind.Tick ? Kind.Droplet : Kind.Fruit));
            }
            points.Sort((a, b) => a.Time.CompareTo(b.Time));

            for (int i = 0; i < points.Count; i++)
            {
                var (t, k) = points[i];
                things.Add(new Thing { Kind = k, Time = t, X = xAt(t), Combo = combo, Samples = s.Samples });
                if (i + 1 >= points.Count) break;
                double gap = points[i + 1].Time - t;
                int tiny = (int)Math.Ceiling(gap / 100) - 1; // about one every 100 ms
                for (int j = 1; j <= tiny; j++)
                {
                    double tt = t + gap * j / (tiny + 1);
                    things.Add(new Thing { Kind = Kind.Tiny, Time = tt, X = xAt(tt), Combo = combo });
                }
            }
        }

        // where even a dash can't make it in time, the fruit before gets a hyper-dash
        void markHyperDashes()
        {
            var palpable = things.FindAll(t => t.Kind is Kind.Fruit or Kind.Droplet);
            float half = halfWidth;
            int lastDirection = 0;
            float lastExcess = half;
            for (int i = 0; i + 1 < palpable.Count; i++)
            {
                var cur = palpable[i];
                var next = palpable[i + 1];
                int dir = next.X > cur.X ? 1 : -1;
                double time = next.Time - cur.Time - 1000.0 / 60 / 4;
                float distance = Math.Abs(next.X - cur.X) - (lastDirection == dir ? lastExcess : half);
                double spare = time * dash_speed - distance;
                if (spare < 0)
                {
                    cur.Hyper = true;
                    cur.HyperTo = next.X;
                    cur.HyperAt = next.Time;
                    lastExcess = half;
                }
                else
                {
                    lastExcess = (float)Math.Clamp(spare, 0, half);
                }
                lastDirection = dir;
            }
        }

        public double StartTime
        {
            get
            {
                double firstT = things.Count > 0 ? things[0].Time : 0;
                return Math.Min(-map.AudioLeadIn, Math.Min(0, firstT - preempt - 1000));
            }
        }

        void judge(Thing t, bool caught, double time)
        {
            t.Done = true;
            t.Caught = caught;
            HitResult r = t.Kind switch
            {
                Kind.Fruit => caught ? HitResult.Great : HitResult.Miss,
                Kind.Droplet => caught ? HitResult.LargeTickHit : HitResult.LargeTickMiss,
                Kind.Tiny => caught ? HitResult.SmallTickHit : HitResult.SmallTickMiss,
                _ => caught ? HitResult.LargeBonus : HitResult.None,
            };
            if (r == HitResult.None) return;
            Score.Apply(r, t.Kind == Kind.Banana ? r : maxFor(t.Kind));
            health.Apply(r);
            if (caught && t.Kind is Kind.Fruit or Kind.Banana)
            {
                var sp = map.ControlPoints.SampleAt(t.Time);
                Sounds.Enqueue(new SoundEvent
                {
                    Kind = t.Kind == Kind.Banana ? SoundKind.SpinnerBonus : SoundKind.Hit,
                    Sounds = (int)t.Samples.Sound,
                    Bank = t.Samples.Bank != 0 ? t.Samples.Bank : sp.Bank,
                    AddBank = t.Samples.AddBank != 0 ? t.Samples.AddBank : sp.Bank,
                    Volume = sp.Volume,
                    Pan = (catcherX / 512 - 0.5f) * 0.8f,
                });
            }
            if (!caught && r is HitResult.Miss or HitResult.LargeTickMiss && Score.MaxCombo > 0 && Score.Combo == 0)
                Sounds.Enqueue(new SoundEvent { Kind = SoundKind.ComboBreak });

            if (caught && t.Hyper)
            {
                // flung towards the next fruit, fast enough to just make it
                double dt = Math.Max(1, t.HyperAt - time);
                hyperTarget = t.HyperTo;
                hyperMult = (float)Math.Max(1, Math.Abs(t.HyperTo - catcherX) / (dt * dash_speed));
                hyperOn = true;
            }
        }

        void autoMove(double time, double dt)
        {
            // glide to whatever's next, arriving on time
            while (autoNext < things.Count && (things[autoNext].Time < time || things[autoNext].Kind == Kind.Banana))
                autoNext++;
            if (autoNext >= things.Count) return;
            var next = things[autoNext];
            double left = next.Time - time;
            float want = next.X - catcherX;
            if (left <= dt) catcherX = next.X;
            else catcherX += want * (float)(dt / left);
            dashing = Math.Abs(want) / Math.Max(1, left) > walk_speed;
        }

        public void Update(double time, Vector2 cursor, int keys)
        {
            if (state is PlayState.Failed or PlayState.Finished)
                return;
            double dt = double.IsNaN(lastTime) ? 0 : Math.Clamp(time - lastTime, 0, 50);
            lastTime = time;
            health.Update(time);

            if (autoplay)
                autoMove(time, dt);
            else
            {
                int dir = ((keys & key_right) != 0 ? 1 : 0) - ((keys & key_left) != 0 ? 1 : 0);
                dashing = (keys & key_dash) != 0;
                if (dir != 0)
                {
                    facingLeft = dir < 0;
                    float speed = dashing ? dash_speed : walk_speed;
                    if (hyperOn && Math.Sign(hyperTarget - catcherX) == dir)
                        speed = dash_speed * hyperMult;
                    float move = dir * speed * (float)dt;
                    if (hyperOn && Math.Sign(hyperTarget - catcherX) != Math.Sign(hyperTarget - (catcherX + move)))
                    {
                        move = hyperTarget - catcherX; // the fling stops where it was aimed
                        hyperOn = false;
                    }
                    catcherX = Math.Clamp(catcherX + move, 0, 512);
                }
            }

            // whatever reached the plate this frame
            for (int i = first; i < things.Count; i++)
            {
                var t = things[i];
                if (t.Time > time) break;
                if (t.Done) continue;
                judge(t, Math.Abs(t.X - catcherX) <= halfWidth, time);
            }
            while (first < things.Count && things[first].Done && time > things[first].Time + 1000)
                first++;

            bool modFail = (suddenDeath && Score.Miss > 0) || (perfectMod && Score.Accuracy < 1);
            if ((health.Failed && !noFail || modFail) && !autoplay)
            {
                state = PlayState.Failed;
                Score.Failed = true;
            }
            else if (first >= things.Count && time > endTime + 500 || things.Count == 0)
                state = PlayState.Finished;
            else
                state = things.Count > 0 && time < things[0].Time - preempt ? PlayState.Intro : inBreak(time) ? PlayState.Break : PlayState.Playing;
        }

        bool inBreak(double time)
        {
            foreach (var b in map.Breaks)
                if (b.HasEffect && time >= b.Start && time <= b.End) return true;
            return false;
        }

        static DrawCmd rect(float x, float y, float w, float h, uint colour, float alpha)
            => new DrawCmd { Kind = DrawKind.Rect, X = x, Y = y, Radius = w, P0 = h, Colour = colour, Alpha = alpha };

        static DrawCmd disc(float x, float y, float r, uint colour, float alpha, bool rim)
            => new DrawCmd { Kind = DrawKind.Disc, X = x, Y = y, Radius = r, Colour = colour, Alpha = alpha, Flags = rim ? 1 : 0 };

        public void BuildDrawList(DrawList dl, double time)
        {
            dl.Clear();
            float left = play_x, width = 512 * play_scale;
            dl.Add(rect(left, 0, width, 1080, 0xFF0B0C14, 0.55f));

            for (int i = first; i < things.Count; i++)
            {
                var t = things[i];
                if (t.Time - time > preempt) break;
                if (t.Done) continue;
                float y = catcher_y - (float)((t.Time - time) / preempt) * catcher_y;
                float x = left + t.X * play_scale;
                float alpha = 1;
                if (hidden)
                    alpha = Math.Clamp((catcher_y * 0.75f - y) / (catcher_y * 0.25f), 0, 1);
                float r = fruitRadius * play_scale / 2;
                switch (t.Kind)
                {
                    case Kind.Fruit:
                        if (t.Hyper)
                            dl.Add(disc(x, y, r * 1.35f, 0xFFFF3030, 0.5f * alpha, false));
                        dl.Add(disc(x, y, r, colours[t.Combo % colours.Length], alpha, true));
                        break;
                    case Kind.Droplet:
                        dl.Add(disc(x, y, r * 0.5f, colours[t.Combo % colours.Length], alpha, true));
                        break;
                    case Kind.Tiny:
                        dl.Add(disc(x, y, r * 0.25f, colours[t.Combo % colours.Length], alpha, false));
                        break;
                    case Kind.Banana:
                        dl.Add(disc(x, y, r * 0.8f, 0xFFFFE066, alpha, true));
                        break;
                }
            }

            // the catcher: a plate, red while dashing, glowing in a hyper-dash
            float cx = left + catcherX * play_scale, cw = halfWidth * 2 * play_scale;
            if (hyperOn)
                dl.Add(rect(cx - cw / 2 - 10, catcher_y - 14, cw + 20, 48, 0xFFFF3030, 0.35f));
            dl.Add(rect(cx - cw / 2, catcher_y, cw, 14, dashing ? 0xFFFF6B6B : 0xFFE8EEF8, 1));
            dl.Add(rect(cx - cw / 2 + 10, catcher_y + 14, cw - 20, 60, 0xFF2A2E44, 0.95f));
            dl.Add(rect(facingLeft ? cx - cw / 2 + 14 : cx + cw / 2 - 34, catcher_y + 30, 20, 20, 0xFFE8EEF8, 0.9f));
        }

        public void FillHud(ref Hud hud, double time)
        {
            hud.Score = Score.TotalScore;
            hud.Combo = Score.Combo;
            hud.MaxCombo = Score.MaxCombo;
            hud.Accuracy = (float)Score.Accuracy;
            hud.Health = (float)health.Health;
            hud.Great = Score.Great;
            hud.Ok = Score.LargeTickHits;
            hud.Meh = Score.SmallTickHits;
            hud.Miss = Score.Miss + Score.LargeTickMisses;
            hud.State = state;
            hud.Rank = (int)Score.Rank;
            hud.UnstableRate = 0;
            double firstT = things.Count > 0 ? things[0].Time : 0;
            hud.SkipTarget = time < firstT - preempt - 2500 ? firstT - preempt - 1500 : 0;
            hud.Progress = endTime > firstT ? (float)Math.Clamp((time - firstT) / (endTime - firstT), 0, 1) : 0;
        }
    }
}
