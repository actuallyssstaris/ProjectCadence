// osu!taiko: notes scroll in from the right to a drum. Hit windows and the swell
// rate follow osu!'s taiko ruleset (https://github.com/ppy/osu), MIT licence.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Scoring;
using Osu5.Util;

namespace Osu5.Play
{
    public sealed class TaikoPlayer : IGame
    {
        enum Kind { Don, Kat, Roll, Swell }

        sealed class Note
        {
            public Kind Kind;
            public bool Big;
            public double Time, End;
            public double Speed;      // px per ms on screen
            public HitResult Result;
            public bool Done;
            public int Hits, Needed;  // swell: hits so far / to clear; roll: ticks taken / max
            public double HitTime;    // for the strong-hit bonus
            public Samples Samples;
        }

        struct Popup
        {
            public HitResult Result;
            public double Time;
        }

        public ScoreProcessor Score { get; } = new ScoreProcessor { Ruleset = 1 };
        public Queue<SoundEvent> Sounds { get; } = new Queue<SoundEvent>();

        const float lane_y = 330, hit_x = 400;
        const int don_keys = 0b0011, kat_keys = 0b1100;

        readonly Beatmap map;
        readonly List<Note> notes = new List<Note>();
        readonly HealthProcessor health;
        readonly double wGreat, wOk, wMiss;
        readonly bool autoplay, noFail, suddenDeath, perfectMod, hidden;
        readonly double endTime;
        readonly List<Popup> popups = new List<Popup>();

        int prevKeys, first;
        int autoSide;
        PlayState state = PlayState.Intro;

        public TaikoPlayer(Beatmap m, Mods mods)
        {
            map = m;
            double od = m.Difficulty.OverallDifficulty;
            wGreat = Math.Floor(Difficulty.Range(od, 50, 35, 20)) - 0.5;
            wOk = Math.Floor(Difficulty.Range(od, 120, 80, 50)) - 0.5;
            wMiss = Math.Floor(Difficulty.Range(od, 135, 95, 70)) - 0.5;

            autoplay = (mods & Mods.Auto) != 0;
            noFail = (mods & Mods.NoFail) != 0;
            suddenDeath = (mods & Mods.SuddenDeath) != 0;
            perfectMod = (mods & Mods.Perfect) != 0;
            hidden = (mods & Mods.Hidden) != 0;
            Score.Multiplier = ModRules.ScoreMultiplier(mods);

            double swellRate = Difficulty.Range(od, 3, 5, 7.5);
            var maxes = new List<(double Time, HitResult Max)>();
            // lazer's time range at 16:9: stable scrolls 693 of 853 px in 6.93s at
            // sv 1, and the 1.4 taiko speed-up comes off that
            const double time_range = (16.0 / 9 * 480 - 160) / 100 * 1000 / 1.4;
            foreach (var h in m.HitObjects)
            {
                var sv = m.ControlPoints.DifficultyAt(h.StartTime).SliderVelocity;
                double beat = m.ControlPoints.TimingAt(h.StartTime).BeatLength;
                var n = new Note
                {
                    Time = h.StartTime,
                    End = h.EndTime,
                    Big = (h.Samples.Sound & HitSound.Finish) != 0,
                    Speed = (1920 - hit_x) * m.Difficulty.SliderMultiplier * (1000 / beat) * sv / time_range,
                    Samples = h.Samples,
                };
                switch (h)
                {
                    case Slider s:
                        n.Kind = Kind.Roll;
                        if (!double.IsNaN(s.RollDuration))
                            n.End = n.Time + s.RollDuration;
                        // a tick every quarter beat (third on 3 tick rate maps), to the end
                        double spacing = beat / (m.Difficulty.SliderTickRate == 3 ? 3 : 4);
                        n.Needed = 0;
                        for (double t = n.Time; t < n.End + spacing / 2; t += spacing)
                            n.Needed++;
                        n.Needed = Math.Max(1, n.Needed);
                        break;
                    case Spinner:
                        n.Kind = Kind.Swell;
                        n.Needed = Math.Max(1, (int)((n.End - n.Time) / 1000 * swellRate));
                        break;
                    default:
                        n.Kind = (h.Samples.Sound & (HitSound.Whistle | HitSound.Clap)) != 0 ? Kind.Kat : Kind.Don;
                        maxes.Add((n.Time, HitResult.Great));
                        break;
                }
                notes.Add(n);
            }
            notes.Sort((a, b) => a.Time.CompareTo(b.Time));
            maxes.Sort((a, b) => a.Time.CompareTo(b.Time));

            var maxResults = new List<HitResult>(maxes.Count);
            foreach (var x in maxes) maxResults.Add(x.Max);
            Score.SetMaximums(maxResults);
            health = new HealthProcessor(m, maxes);
            endTime = m.LastObjectEndTime;
        }

        // dons and kats in time order (true = kat), for the star rating
        public List<(double Time, bool Kat)> Hits()
        {
            var list = new List<(double, bool)>();
            foreach (var n in notes)
                if (n.Kind is Kind.Don or Kind.Kat)
                    list.Add((n.Time, n.Kind == Kind.Kat));
            return list;
        }

        public double GreatWindow => wGreat;

        public double StartTime
        {
            get
            {
                double firstNote = notes.Count > 0 ? notes[0].Time : 0;
                return Math.Min(-map.AudioLeadIn, Math.Min(0, firstNote - 2500));
            }
        }

        void judge(Note n, HitResult r, double time, double error)
        {
            n.Result = r;
            n.Done = true;
            n.HitTime = time;
            Score.Apply(r, HitResult.Great, r == HitResult.Miss ? double.NaN : error);
            health.Apply(r);
            popups.Add(new Popup { Result = r, Time = time });
            if (popups.Count > 4)
                popups.RemoveAt(0);
            if (r == HitResult.Miss && Score.MaxCombo > 0 && Score.Combo == 0)
                Sounds.Enqueue(new SoundEvent { Kind = SoundKind.ComboBreak });
        }

        void bonus(HitResult r)
        {
            Score.Apply(r, r);
            health.Apply(r);
        }

        void drum(bool kat, double time, bool big)
        {
            var sp = map.ControlPoints.SampleAt(time);
            Sounds.Enqueue(new SoundEvent
            {
                Kind = SoundKind.Hit,
                // don is the plain hit, kat the clap; a big note adds the finish
                Sounds = (kat ? (int)HitSound.Clap : 0) | (big ? (int)HitSound.Finish : 0),
                Bank = sp.Bank,
                AddBank = sp.Bank,
                Volume = sp.Volume,
                Pan = -0.3f,
            });
        }

        int autoKeys(double time)
        {
            for (int i = first; i < notes.Count; i++)
            {
                var n = notes[i];
                if (n.Time > time) break;
                if (n.Kind is Kind.Roll or Kind.Swell)
                {
                    if (time > n.End) continue;
                    // a hit every 60 ms, sides alternating; swells alternate colours too
                    int step = (int)((time - n.Time) / 60);
                    if ((time - n.Time) % 60 > 30) return 0;
                    bool katTurn = n.Kind == Kind.Swell && step % 2 == 1;
                    int bit = step % 2 == 0 ? 0 : 1;
                    return katTurn ? (bit == 0 ? 0b0100 : 0b1000) : 1 << bit;
                }
                if (!n.Done && time >= n.Time)
                {
                    int keys = n.Kind == Kind.Kat ? (autoSide == 0 ? 0b0100 : 0b1000) : (autoSide == 0 ? 0b0001 : 0b0010);
                    if (n.Big) keys = n.Kind == Kind.Kat ? kat_keys : don_keys;
                    autoSide ^= 1;
                    return keys;
                }
            }
            return 0;
        }

        public void Update(double time, Vector2 cursor, int keys)
        {
            if (state is PlayState.Failed or PlayState.Finished)
                return;
            if (autoplay)
                keys = autoKeys(time);

            health.Update(time);
            int pressed = keys & ~prevKeys;

            for (int bit = 0; bit < 4; bit++)
            {
                if ((pressed & (1 << bit)) == 0)
                    continue;
                bool kat = bit >= 2;
                press(kat, time, (pressed & (kat ? kat_keys : don_keys)) == (kat ? kat_keys : don_keys));
                // both sides in the same frame are one strong hit, not two
                if ((pressed & (kat ? kat_keys : don_keys)) == (kat ? kat_keys : don_keys))
                    bit |= 1;
            }

            for (int i = first; i < notes.Count; i++)
            {
                var n = notes[i];
                if (n.Time - time > wMiss) break;
                if (n.Done) continue;
                if (n.Kind is Kind.Don or Kind.Kat && time - n.Time > wOk)
                    judge(n, HitResult.Miss, time, 0);
                else if (n.Kind == Kind.Roll && time > n.End)
                    n.Done = true;
                else if (n.Kind == Kind.Swell && time > n.End)
                {
                    n.Done = true;
                    if (n.Hits >= n.Needed) bonus(HitResult.LargeBonus);
                }
            }
            while (first < notes.Count && notes[first].Done && time > notes[first].End + 1000)
                first++;

            bool modFail = (suddenDeath && Score.Miss > 0) || (perfectMod && Score.Accuracy < 1);
            if ((health.Failed && !noFail || modFail) && !autoplay)
            {
                state = PlayState.Failed;
                Score.Failed = true;
            }
            else if (first >= notes.Count && time > endTime + 500 || notes.Count == 0)
                state = PlayState.Finished;
            else
                state = notes.Count > 0 && time < notes[0].Time - 2000 ? PlayState.Intro : inBreak(time) ? PlayState.Break : PlayState.Playing;

            prevKeys = keys;
        }

        void press(bool kat, double time, bool strong)
        {
            for (int i = first; i < notes.Count; i++)
            {
                var n = notes[i];
                if (n.Time - time > wMiss) break;

                if (n.Kind == Kind.Roll && time >= n.Time && time <= n.End)
                {
                    if (n.Hits < n.Needed)
                    {
                        n.Hits++;
                        bonus(HitResult.SmallBonus);
                    }
                    drum(kat, time, false);
                    return;
                }
                if (n.Kind == Kind.Swell && time >= n.Time && time <= n.End)
                {
                    // swells want the colours to alternate
                    bool wantKat = n.Hits % 2 == 1;
                    if (kat == wantKat && n.Hits < n.Needed)
                    {
                        n.Hits++;
                        bonus(HitResult.SmallBonus);
                    }
                    drum(kat, time, false);
                    return;
                }

                if (n.Kind is Kind.Roll or Kind.Swell)
                    continue;

                if (n.Done)
                {
                    // the other hand landing right after a big note makes it a strong hit
                    if (n.Big && n.Result != HitResult.Miss && time - n.HitTime < 30 && (n.Kind == Kind.Kat) == kat && n.Hits == 0)
                    {
                        n.Hits = 1;
                        bonus(HitResult.LargeBonus);
                        return;
                    }
                    continue;
                }

                double off = time - n.Time;
                if (Math.Abs(off) > wMiss)
                    continue;
                drum(kat, time, n.Big);
                bool right = (n.Kind == Kind.Kat) == kat;
                var r = !right || Math.Abs(off) > wOk ? HitResult.Miss : Math.Abs(off) <= wGreat ? HitResult.Great : HitResult.Ok;
                judge(n, r, time, off);
                if (strong && n.Big && r != HitResult.Miss)
                {
                    n.Hits = 1;
                    bonus(HitResult.LargeBonus);
                }
                return;
            }
            drum(kat, time, false);
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
            dl.Add(rect(0, lane_y - 110, 1920, 220, 0xFF0B0C14, 0.88f));
            dl.Add(rect(0, lane_y - 112, 1920, 3, 0xFFFFFFFF, 0.3f));
            dl.Add(rect(0, lane_y + 109, 1920, 3, 0xFFFFFFFF, 0.3f));

            // the drum on the left, halves lighting up as they're hit
            dl.Add(rect(0, lane_y - 110, 250, 220, 0xFF1A1C2C, 1));
            bool donL = (prevKeys & 1) != 0, donR = (prevKeys & 2) != 0, katL = (prevKeys & 4) != 0, katR = (prevKeys & 8) != 0;
            dl.Add(disc(125, lane_y, 80, katL || katR ? 0xFF4BA8EB : 0xFF2A2E44, 1, true));
            dl.Add(disc(125, lane_y, 56, donL || donR ? 0xFFEB4B4B : 0xFF3A2E3A, 1, false));

            // target
            dl.Add(disc(hit_x, lane_y, 64, 0xFFFFFFFF, 0.15f, true));
            dl.Add(disc(hit_x, lane_y, 44, 0xFF0B0C14, 1, false));

            // far notes first so earlier ones sit on top
            int last = first;
            while (last < notes.Count && hit_x + (notes[last].Time - time) * notes[last].Speed < 2000)
                last++;
            for (int i = last - 1; i >= first; i--)
            {
                var n = notes[i];
                float x = hit_x + (float)((n.Time - time) * n.Speed);
                float alpha = 1;
                if (hidden)
                    alpha = Math.Clamp((x - hit_x - 150) / 250, 0, 1);

                switch (n.Kind)
                {
                    case Kind.Don:
                    case Kind.Kat:
                        if (n.Done) continue;
                        dl.Add(disc(x, lane_y, n.Big ? 66 : 44, n.Kind == Kind.Don ? 0xFFEB4B4B : 0xFF4BA8EB, alpha, true));
                        break;
                    case Kind.Roll:
                    {
                        if (time > n.End) continue;
                        float xEnd = hit_x + (float)((n.End - time) * n.Speed);
                        float xStart = Math.Max(x, hit_x);
                        float r = n.Big ? 60 : 40;
                        if (xEnd > xStart)
                            dl.Add(rect(xStart, lane_y - r, xEnd - xStart, r * 2, 0xFFFCB800, 0.85f * alpha));
                        dl.Add(disc(xEnd, lane_y, r, 0xFFFCB800, 0.85f * alpha, false));
                        dl.Add(disc(xStart, lane_y, r, 0xFFFCB800, alpha, true));
                        break;
                    }
                    case Kind.Swell:
                    {
                        if (time > n.End) continue;
                        bool active = time >= n.Time;
                        float sx = active ? hit_x : x;
                        float k = n.Needed > 0 ? (float)n.Hits / n.Needed : 1;
                        dl.Add(disc(sx, lane_y, active ? 70 + 40 * k : 60, 0xFFFF9F1C, active ? 0.9f : alpha, true));
                        if (active)
                            dl.Add(new DrawCmd { Kind = DrawKind.ScreenJudgement, Number = -(n.Needed - n.Hits), X = hit_x, Y = lane_y - 190, Radius = 40, Alpha = 1 });
                        break;
                    }
                }
            }

            if (popups.Count > 0)
            {
                var p = popups[^1];
                double age = time - p.Time;
                if (age >= 0 && age < 500)
                {
                    float k = (float)(age / 500);
                    dl.Add(new DrawCmd
                    {
                        Kind = DrawKind.ScreenJudgement, Number = (int)p.Result, X = hit_x, Y = lane_y - 170 - k * 20,
                        Radius = 40, Alpha = 1 - k * k, P0 = k,
                    });
                }
            }
        }

        public void FillHud(ref Hud hud, double time)
        {
            hud.Score = Score.TotalScore;
            hud.Combo = Score.Combo;
            hud.MaxCombo = Score.MaxCombo;
            hud.Accuracy = (float)Score.Accuracy;
            hud.Health = (float)health.Health;
            hud.Great = Score.Great;
            hud.Ok = Score.Ok;
            hud.Meh = Score.Meh;
            hud.Miss = Score.Miss;
            hud.State = state;
            hud.Rank = (int)Score.Rank;
            hud.UnstableRate = (float)Score.UnstableRate;
            double firstT = notes.Count > 0 ? notes[0].Time : 0;
            hud.SkipTarget = time < firstT - 3500 ? firstT - 2500 : 0;
            hud.Progress = endTime > firstT ? (float)Math.Clamp((time - firstT) / (endTime - firstT), 0, 1) : 0;
        }
    }
}
