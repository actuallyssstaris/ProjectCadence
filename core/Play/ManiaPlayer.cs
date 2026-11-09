// osu!mania: notes and holds falling down columns. Hit windows are stable's
// (lazer keeps them for maps made for mania), scoring is lazer's standardised one.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Scoring;

namespace Osu5.Play
{
    public sealed class ManiaPlayer : IGame
    {
        sealed class Note
        {
            public int Column;
            public double Time, End;
            public bool Hold;
            public HitResult Head, Tail;
            public bool Holding, Done;
            public Samples Samples;
        }

        struct Popup
        {
            public HitResult Result;
            public double Time;
        }

        public ScoreProcessor Score { get; } = new ScoreProcessor { Ruleset = 3 };
        public Queue<SoundEvent> Sounds { get; } = new Queue<SoundEvent>();

        public const double ScrollTime = 650; // ms a note takes from the top to the hit line
        const float hit_y = 930;

        readonly Beatmap map;
        readonly List<Note> notes = new List<Note>();
        readonly HealthProcessor health;
        readonly int columns;
        readonly float colWidth, stageX;
        readonly double wPerfect, wGreat, wGood, wOk, wMeh, wMiss;
        readonly bool autoplay, noFail, suddenDeath, perfectMod, hidden;
        readonly double endTime;
        readonly List<Popup> popups = new List<Popup>();

        int prevKeys;
        int first;
        PlayState state = PlayState.Intro;

        public int Columns => columns;

        public ManiaPlayer(Beatmap m, Mods mods)
        {
            map = m;
            columns = Math.Clamp((int)Math.Round(m.Difficulty.CircleSize), 1, 10);
            colWidth = Math.Min(110, 1000f / columns);
            stageX = 960 - columns * colWidth / 2;

            double od = m.Difficulty.OverallDifficulty;
            wPerfect = 16.5;
            wGreat = 64.5 - 3 * od;
            wGood = 97.5 - 3 * od;
            wOk = 127.5 - 3 * od;
            wMeh = 151.5 - 3 * od;
            wMiss = 188.5 - 3 * od;

            autoplay = (mods & Mods.Auto) != 0;
            noFail = (mods & Mods.NoFail) != 0;
            suddenDeath = (mods & Mods.SuddenDeath) != 0;
            perfectMod = (mods & Mods.Perfect) != 0;
            hidden = (mods & Mods.Hidden) != 0;
            bool mirror = (mods & Mods.Mirror) != 0;
            Score.Multiplier = ModRules.ScoreMultiplier(mods);

            var maxes = new List<(double Time, HitResult Max)>();
            foreach (var h in m.HitObjects)
            {
                int col = Math.Clamp((int)(h.Position.X * columns / 512), 0, columns - 1);
                if (mirror)
                    col = columns - 1 - col;
                var n = new Note { Column = col, Time = h.StartTime, End = h.EndTime, Hold = h is HoldNote, Samples = h.Samples };
                notes.Add(n);
                maxes.Add((n.Time, HitResult.Perfect));
                if (n.Hold)
                    maxes.Add((n.End, HitResult.Perfect));
            }
            notes.Sort((a, b) => a.Time.CompareTo(b.Time));
            maxes.Sort((a, b) => a.Time.CompareTo(b.Time));

            var maxResults = new List<HitResult>(maxes.Count);
            foreach (var x in maxes) maxResults.Add(x.Max);
            Score.SetMaximums(maxResults);
            health = new HealthProcessor(m, maxes);
            endTime = m.LastObjectEndTime;
        }

        public double StartTime
        {
            get
            {
                double firstNote = notes.Count > 0 ? notes[0].Time : 0;
                return Math.Min(-map.AudioLeadIn, Math.Min(0, firstNote - ScrollTime - 1000));
            }
        }

        HitResult resultFor(double offset, double scale = 1)
        {
            offset = Math.Abs(offset);
            if (offset <= wPerfect * scale) return HitResult.Perfect;
            if (offset <= wGreat * scale) return HitResult.Great;
            if (offset <= wGood * scale) return HitResult.Good;
            if (offset <= wOk * scale) return HitResult.Ok;
            if (offset <= wMeh * scale) return HitResult.Meh;
            if (offset <= wMiss * scale) return HitResult.Miss;
            return HitResult.None;
        }

        void judge(HitResult r, double time, double error)
        {
            Score.Apply(r, HitResult.Perfect, r == HitResult.Miss ? double.NaN : error);
            health.Apply(r);
            popups.Add(new Popup { Result = r, Time = time });
            if (popups.Count > 4)
                popups.RemoveAt(0);
            if (r == HitResult.Miss && Score.MaxCombo > 0 && Score.Combo == 0)
                Sounds.Enqueue(new SoundEvent { Kind = SoundKind.ComboBreak });
        }

        void hitSound(Note n, double time)
        {
            var sp = map.ControlPoints.SampleAt(time);
            Sounds.Enqueue(new SoundEvent
            {
                Kind = SoundKind.Hit,
                Sounds = (int)n.Samples.Sound,
                Bank = n.Samples.Bank != 0 ? n.Samples.Bank : sp.Bank,
                AddBank = n.Samples.AddBank != 0 ? n.Samples.AddBank : (n.Samples.Bank != 0 ? n.Samples.Bank : sp.Bank),
                Volume = n.Samples.Volume != 0 ? n.Samples.Volume : sp.Volume,
                Custom = n.Samples.Custom != 0 ? n.Samples.Custom : sp.CustomIndex,
                Pan = ((n.Column + 0.5f) / columns - 0.5f) * 0.8f,
            });
        }

        int autoKeys(double time)
        {
            int keys = 0;
            for (int i = first; i < notes.Count; i++)
            {
                var n = notes[i];
                if (n.Time > time) break;
                double until = n.Hold ? n.End : n.Time + 40;
                if (time >= n.Time && time < until)
                    keys |= 1 << n.Column;
            }
            return keys;
        }

        public void Update(double time, Vector2 cursor, int keys)
        {
            if (state is PlayState.Failed or PlayState.Finished)
                return;
            if (autoplay)
                keys = autoKeys(time);

            health.Update(time);
            int pressed = keys & ~prevKeys, released = prevKeys & ~keys;

            for (int c = 0; c < columns; c++)
            {
                if ((pressed & (1 << c)) != 0)
                {
                    // the earliest note in the column that's still waiting
                    for (int i = first; i < notes.Count; i++)
                    {
                        var n = notes[i];
                        if (n.Time - time > wMiss) break;
                        if (n.Column != c || n.Head != HitResult.None) continue;
                        var r = resultFor(time - n.Time);
                        if (r == HitResult.None) continue;
                        n.Head = r;
                        judge(r, time, time - n.Time);
                        hitSound(n, time);
                        if (n.Hold && r != HitResult.Miss)
                            n.Holding = true;
                        else
                        {
                            if (n.Hold)
                            {
                                n.Tail = HitResult.Miss;
                                judge(HitResult.Miss, time, 0);
                            }
                            n.Done = true;
                        }
                        break;
                    }
                }

                if ((released & (1 << c)) != 0)
                {
                    for (int i = first; i < notes.Count; i++)
                    {
                        var n = notes[i];
                        if (n.Column != c || !n.Holding) continue;
                        // let go: judged against the end, a bit more loosely than a press
                        var r = resultFor(time - n.End, 1.5);
                        if (r == HitResult.None) r = HitResult.Miss;
                        n.Tail = r;
                        n.Holding = false;
                        n.Done = true;
                        judge(r, time, time - n.End);
                        break;
                    }
                }
            }

            // what went by without a press, and holds kept down to the end
            for (int i = first; i < notes.Count; i++)
            {
                var n = notes[i];
                if (n.Time - time > wMiss) break;
                if (n.Done) continue;
                if (n.Head == HitResult.None && time - n.Time > wMeh)
                {
                    n.Head = HitResult.Miss;
                    judge(HitResult.Miss, time, 0);
                    if (n.Hold)
                    {
                        n.Tail = HitResult.Miss;
                        judge(HitResult.Miss, time, 0);
                    }
                    n.Done = true;
                }
                else if (n.Holding && time >= n.End)
                {
                    n.Tail = n.Head == HitResult.Perfect ? HitResult.Perfect : HitResult.Great;
                    n.Holding = false;
                    n.Done = true;
                    judge(n.Tail, time, 0);
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
                state = notes.Count > 0 && time < notes[0].Time - ScrollTime ? PlayState.Intro : inBreak(time) ? PlayState.Break : PlayState.Playing;

            prevKeys = keys;
        }

        bool inBreak(double time)
        {
            foreach (var b in map.Breaks)
                if (b.HasEffect && time >= b.Start && time <= b.End) return true;
            return false;
        }

        // outer columns white, inner blue, a middle odd one yellow, like most skins
        uint colourFor(int c)
        {
            if (columns % 2 == 1 && c == columns / 2) return 0xFFFFD34D;
            int fromEdge = Math.Min(c, columns - 1 - c);
            return fromEdge % 2 == 0 ? 0xFFE8EEF8 : 0xFF5AB4FF;
        }

        float yAt(double t, double time) => hit_y - (float)((t - time) / ScrollTime) * hit_y;

        static DrawCmd rect(float x, float y, float w, float h, uint colour, float alpha)
            => new DrawCmd { Kind = DrawKind.Rect, X = x, Y = y, Radius = w, P0 = h, Colour = colour, Alpha = alpha };

        public void BuildDrawList(DrawList dl, double time)
        {
            dl.Clear();
            float stageW = columns * colWidth;
            dl.Add(rect(stageX - 4, 0, stageW + 8, 1080, 0xFF0B0C14, 0.9f));
            for (int c = 1; c < columns; c++)
                dl.Add(rect(stageX + c * colWidth - 1, 0, 2, 1080, 0xFFFFFFFF, 0.06f));

            // pressed columns light up from the hit line
            for (int c = 0; c < columns; c++)
                if ((prevKeys & (1 << c)) != 0)
                    dl.Add(rect(stageX + c * colWidth, hit_y - 260, colWidth, 260, colourFor(c), 0.18f));

            for (int i = first; i < notes.Count; i++)
            {
                var n = notes[i];
                float y = yAt(n.Time, time);
                if (y < -40) break;
                if (n.Done && !n.Hold) continue;
                if (n.Done && n.Hold && n.Tail != HitResult.Miss) continue;

                float alpha = 1;
                if (hidden)
                    alpha = Math.Clamp((hit_y * 0.8f - y) / (hit_y * 0.2f), 0, 1);
                float x = stageX + n.Column * colWidth + 4, w = colWidth - 8;
                uint col = colourFor(n.Column);

                if (n.Hold)
                {
                    float yEnd = yAt(n.End, time);
                    float yHead = n.Holding ? hit_y : y;
                    bool dead = n.Head == HitResult.Miss;
                    if (yHead > yEnd)
                        dl.Add(rect(x + w * 0.15f, yEnd, w * 0.7f, yHead - yEnd, col, (dead ? 0.25f : n.Holding ? 0.85f : 0.6f) * alpha));
                    dl.Add(rect(x, yEnd - 8, w, 16, col, (dead ? 0.3f : 0.9f) * alpha));
                    dl.Add(rect(x, yHead - 16, w, 32, col, (dead ? 0.3f : 1) * alpha));
                }
                else
                {
                    dl.Add(rect(x, y - 16, w, 32, col, alpha));
                }
            }

            // hit line and the keys under it
            dl.Add(rect(stageX, hit_y - 3, stageW, 6, 0xFFFFFFFF, 0.85f));
            for (int c = 0; c < columns; c++)
            {
                bool down = (prevKeys & (1 << c)) != 0;
                dl.Add(rect(stageX + c * colWidth + 6, hit_y + 16, colWidth - 12, 110, colourFor(c), down ? 0.9f : 0.25f));
            }

            if (popups.Count > 0)
            {
                var p = popups[^1];
                double age = time - p.Time;
                if (age >= 0 && age < 600)
                {
                    float k = (float)(age / 600);
                    dl.Add(new DrawCmd
                    {
                        Kind = DrawKind.ScreenJudgement, Number = (int)p.Result, X = 960, Y = hit_y - 330,
                        Radius = 48, Alpha = 1 - k * k, P0 = k,
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
            hud.Great = Score.Great + Score.Perfect;
            hud.Ok = Score.Ok + Score.Good;
            hud.Meh = Score.Meh;
            hud.Miss = Score.Miss;
            hud.State = state;
            hud.Rank = (int)Score.Rank;
            hud.UnstableRate = (float)Score.UnstableRate;
            double firstT = notes.Count > 0 ? notes[0].Time : 0;
            hud.SkipTarget = time < firstT - 3000 ? firstT - 2000 : 0;
            hud.Progress = endTime > firstT ? (float)Math.Clamp((time - firstT) / (endTime - firstT), 0, 1) : 0;
        }
    }
}
