using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Scoring;

namespace Osu5.Play
{
    public sealed class Player
    {
        sealed class ObjState
        {
            public HitResult Result;          // circle / slider head / spinner
            public double ResultTime;
            public bool Done;
            public double DoneTime;

            // sliders
            public int NextNested;
            public bool Tracking;
            public double TrackingChanged;
            public HitResult[]? NestedResults;

            // spinners
            public double Rotation;
            public float LastAngle;
            public bool HasAngle;
            public int SpinTicks;
            public double LastSpinTime;
            public double Rpm;

            public double ShakeTime = double.NegativeInfinity;
        }

        struct Popup
        {
            public Vector2 Pos;
            public HitResult Result;
            public double Time;
        }

        static readonly uint[] default_colours =
        {
            0xFFFFC000, 0xFF00CA00, 0xFF127CFF, 0xFFF21839,
        };

        public const float FollowAreaScale = 2.4f;
        const double max_spin_per_ms = 0.05; // ~477 rpm, stable's cap
        const double popup_duration = 600;
        const double explode_duration = 240;

        public readonly Beatmap Map;
        public readonly HitWindows Windows;
        public readonly ScoreProcessor Score = new ScoreProcessor();
        public readonly HealthProcessor Health;

        public bool NoFail;
        public bool Autoplay;
        public readonly bool Hidden;

        // lazer's Hidden: objects fade in over 40% of the approach and are gone 30% later
        const double hd_fade_in = 0.4;
        const double hd_fade_out = 0.3;

        readonly List<OsuObject> objs;
        readonly ObjState[] states;
        readonly uint[] colours;
        readonly List<Vector2>[] drawPaths;
        readonly List<double>[] drawLengths;
        readonly List<Popup> popups = new List<Popup>();

        public readonly Queue<SoundEvent> Sounds = new Queue<SoundEvent>();

        int firstActive;
        int prevKeys;
        double lastTime = double.NegativeInfinity;
        Vector2 cursor = new Vector2(256, 192);

        public PlayState State { get; private set; } = PlayState.Intro;
        public double FailTime { get; private set; } = double.NaN;
        public double EndTime { get; }

        // skinColours: the skin's combo colours, used when the map has none
        public Player(Beatmap map, Mods mods = Mods.None, uint[]? skinColours = null)
        {
            Map = map;
            objs = map.HitObjects;
            states = new ObjState[objs.Count];
            Windows = new HitWindows(map.Difficulty.OverallDifficulty);

            Autoplay = (mods & Mods.Auto) != 0;
            NoFail = (mods & Mods.NoFail) != 0;
            Hidden = (mods & Mods.Hidden) != 0;
            Score.Multiplier = ModRules.ScoreMultiplier(mods);
            Score.Hidden = Hidden;

            if (Hidden)
            {
                foreach (var h in objs)
                    h.TimeFadeIn = h.TimePreempt * hd_fade_in;
            }

            colours = map.ComboColours.Count > 0 ? map.ComboColours.ToArray()
                    : skinColours is { Length: > 0 } ? skinColours
                    : default_colours;
            drawPaths = new List<Vector2>[objs.Count];
            drawLengths = new List<double>[objs.Count];

            var maxes = new List<(double Time, HitResult Max)>();

            for (int i = 0; i < objs.Count; i++)
            {
                states[i] = new ObjState();

                switch (objs[i])
                {
                    case HitCircle c:
                        maxes.Add((c.StartTime, HitResult.Great));
                        break;

                    case Slider s:
                        states[i].NestedResults = new HitResult[s.Nested.Count];
                        foreach (var n in s.Nested)
                        {
                            switch (n.Kind)
                            {
                                case NestedKind.Head: maxes.Add((n.Time, HitResult.Great)); break;
                                case NestedKind.Tail: maxes.Add((s.TailJudgeTime, HitResult.SliderTailHit)); break;
                                default: maxes.Add((n.Time, HitResult.LargeTickHit)); break;
                            }
                        }

                        buildDrawPath(i, s);
                        break;

                    case Spinner sp:
                        maxes.Add((sp.EndTime, HitResult.Great));
                        break;
                }
            }

            maxes.Sort((a, b) => a.Time.CompareTo(b.Time));

            var maxResults = new List<HitResult>(maxes.Count);
            foreach (var m in maxes) maxResults.Add(m.Max);
            Score.SetMaximums(maxResults);

            Health = new HealthProcessor(map, maxes);
            EndTime = map.LastObjectEndTime;
        }

        // where the host should start the clock
        public double StartTime
        {
            get
            {
                if (objs.Count == 0) return 0;
                double first = objs[0].StartTime - objs[0].TimePreempt;
                return Math.Min(-Map.AudioLeadIn, Math.Min(0, first - 1000));
            }
        }

        // collapse the dense approximated path into something cheap to draw
        void buildDrawPath(int i, Slider s)
        {
            var src = s.Path.Points;
            var pts = new List<Vector2>(Math.Min(src.Count, 256));
            var lens = new List<double>(pts.Capacity);

            double acc = 0;
            for (int k = 0; k < src.Count; k++)
            {
                if (pts.Count > 0)
                {
                    float d = Vector2.Distance(pts[^1], src[k]);
                    if (d < 3f && k != src.Count - 1) continue;
                    acc += d;
                }

                pts.Add(src[k]);
                lens.Add(acc);
            }

            drawPaths[i] = pts;
            drawLengths[i] = lens;
        }

        public void Update(double time, Vector2 cursorPos, int keys)
        {
            cursor = cursorPos;

            if (State is PlayState.Failed or PlayState.Finished)
            {
                lastTime = time;
                return;
            }

            if (Autoplay)
                autoplay(time, ref keys);

            Health.Update(time);

            int pressed = keys & ~prevKeys;
            for (int bit = 0; bit < 2; bit++)
            {
                if ((pressed & (1 << bit)) != 0)
                    click(time);
            }

            for (int i = firstActive; i < objs.Count; i++)
            {
                var h = objs[i];
                if (h.StartTime - h.TimePreempt > time)
                    break;

                updateObject(i, time, keys);
            }

            while (firstActive < objs.Count && states[firstActive].Done && time > states[firstActive].DoneTime + 1000)
                firstActive++;

            if (Health.Failed && !NoFail && !Autoplay)
            {
                State = PlayState.Failed;
                FailTime = time;
                Score.Failed = true;
            }
            else if (allDone() && time > EndTime + 500)
                State = PlayState.Finished;
            else
                State = inBreak(time) ? PlayState.Break : time < (objs.Count > 0 ? objs[0].StartTime - objs[0].TimePreempt : 0) ? PlayState.Intro : PlayState.Playing;

            prevKeys = keys;
            lastTime = time;
        }

        bool allDone()
        {
            for (int i = firstActive; i < objs.Count; i++)
                if (!states[i].Done) return false;
            return true;
        }

        bool inBreak(double time)
        {
            foreach (var b in Map.Breaks)
                if (b.HasEffect && time >= b.Start && time <= b.End) return true;
            return false;
        }

        public double SkipTarget(double time)
        {
            if (objs.Count == 0) return 0;

            double first = objs[0].StartTime;
            if (time < first - 2500)
                return first - 2000;

            foreach (var b in Map.Breaks)
            {
                if (b.HasEffect && time >= b.Start && time < b.End - 2500)
                    return b.End - 2000;
            }

            return 0;
        }

        void click(double time)
        {
            int target = -1;

            for (int i = firstActive; i < objs.Count; i++)
            {
                var h = objs[i];
                if (h.StartTime - HitWindows.MissWindow > time)
                    break;
                if (h is Spinner || states[i].Result != HitResult.None)
                    continue;

                if (Vector2.Distance(cursor, h.StackedPosition) <= h.Radius)
                {
                    target = i;
                    break;
                }
            }

            if (target < 0)
                return;

            var obj = objs[target];
            var st = states[target];

            // note lock: an earlier object that can still be hit blocks this one
            for (int j = firstActive; j < target; j++)
            {
                if (objs[j] is Spinner || states[j].Result != HitResult.None)
                    continue;
                if (time <= objs[j].StartTime + Windows.Meh)
                {
                    st.ShakeTime = time;
                    return;
                }
            }

            double offset = time - obj.StartTime;
            var result = Windows.ResultFor(offset);

            if (result == HitResult.None)
            {
                st.ShakeTime = time;
                return;
            }

            judgeHead(target, result, time, result == HitResult.Miss ? double.NaN : offset);
        }

        void sound(SoundKind kind, Samples s, double time, float x)
        {
            if (Sounds.Count > 64) return;

            var tp = Map.ControlPoints.SampleAt(time + 5);
            int bank = s.Bank != 0 ? s.Bank : tp.Bank;
            int add = s.AddBank != 0 ? s.AddBank : bank;

            Sounds.Enqueue(new SoundEvent
            {
                Kind = kind,
                Sounds = (int)s.Sound,
                Bank = bank,
                AddBank = add,
                Volume = s.Volume != 0 ? s.Volume : tp.Volume,
                Custom = s.Custom != 0 ? s.Custom : tp.CustomIndex,
                Pan = Math.Clamp((x - 256) / 256 * 0.8f, -1, 1),
            });
        }

        void comboCheck(HitResult r, int comboBefore, double time)
        {
            if (HitResults.BreaksCombo(r) && comboBefore >= 20)
                sound(SoundKind.ComboBreak, default, time, 256);
        }

        void judgeHead(int i, HitResult result, double time, double hitError = double.NaN)
        {
            var st = states[i];
            st.Result = result;
            st.ResultTime = time;

            int comboBefore = Score.Combo;
            Score.Apply(result, HitResult.Great, hitError);
            Health.Apply(result);
            comboCheck(result, comboBefore, time);

            var h = objs[i];
            if (HitResults.IsHit(result))
            {
                var samples = h is Slider sl && sl.NodeSamples.Count > 0 ? merge(sl.Samples, sl.NodeSamples[0]) : h.Samples;
                sound(SoundKind.Hit, samples, h is Spinner ? h.EndTime : h.StartTime, h.StackedPosition.X);
            }

            if (result != HitResult.Great)
                popups.Add(new Popup { Pos = objs[i].StackedPosition, Result = result, Time = time });

            if (objs[i] is HitCircle)
            {
                st.Done = true;
                st.DoneTime = time;
            }
        }

        // node additions override the slider's own sound, banks fall back to the slider's
        static Samples merge(Samples body, Samples node)
        {
            var s = body;
            s.Sound = node.Sound;
            if (node.Bank != 0) s.Bank = node.Bank;
            if (node.AddBank != 0) s.AddBank = node.AddBank;
            return s;
        }

        void updateObject(int i, double time, int keys)
        {
            var h = objs[i];
            var st = states[i];
            if (st.Done) return;

            switch (h)
            {
                case HitCircle:
                    if (st.Result == HitResult.None && time > h.StartTime + Windows.Meh)
                        judgeHead(i, HitResult.Miss, time);
                    break;

                case Slider s:
                    updateSlider(i, s, st, time, keys);
                    break;

                case Spinner sp:
                    updateSpinner(i, sp, st, time, keys);
                    break;
            }
        }

        void updateSlider(int i, Slider s, ObjState st, double time, int keys)
        {
            if (st.Result == HitResult.None && time > s.StartTime + Windows.Meh)
                judgeHead(i, HitResult.Miss, time);

            if (time >= s.StartTime)
            {
                double progress = Math.Clamp((time - s.StartTime) / s.Duration, 0, 1);
                var ball = s.StackedPosition + s.CurvePositionAt(progress);
                float area = s.Radius * (st.Tracking ? FollowAreaScale : 1);
                bool tracking = keys != 0 && Vector2.Distance(cursor, ball) <= area;

                if (tracking != st.Tracking)
                {
                    st.Tracking = tracking;
                    st.TrackingChanged = time;
                }
            }

            var results = st.NestedResults!;

            while (st.NextNested < s.Nested.Count)
            {
                var n = s.Nested[st.NextNested];

                if (n.Kind == NestedKind.Head)
                {
                    st.NextNested++;
                    continue;
                }

                double judgeAt = n.Kind == NestedKind.Tail ? s.TailJudgeTime : n.Time;
                if (time < judgeAt)
                    break;

                HitResult r, max;
                if (n.Kind == NestedKind.Tail)
                {
                    max = HitResult.SliderTailHit;
                    r = st.Tracking ? HitResult.SliderTailHit : HitResult.IgnoreMiss;
                }
                else
                {
                    max = HitResult.LargeTickHit;
                    r = st.Tracking ? HitResult.LargeTickHit : HitResult.LargeTickMiss;
                }

                results[st.NextNested] = r;
                int comboBefore = Score.Combo;
                Score.Apply(r, max);
                Health.Apply(r);
                comboCheck(r, comboBefore, judgeAt);

                if (HitResults.IsHit(r))
                {
                    var pos = n.Position + s.StackOffset;
                    if (n.Kind == NestedKind.Tick)
                        sound(SoundKind.SliderTick, s.Samples, n.Time, pos.X);
                    else
                    {
                        int node = n.Kind == NestedKind.Tail ? s.NodeSamples.Count - 1 : n.SpanIndex + 1;
                        var ns = node >= 0 && node < s.NodeSamples.Count ? merge(s.Samples, s.NodeSamples[node]) : s.Samples;
                        sound(SoundKind.Hit, ns, n.Kind == NestedKind.Tail ? s.EndTime : n.Time, pos.X);
                    }
                }

                st.NextNested++;
            }

            if (st.NextNested >= s.Nested.Count && st.Result != HitResult.None && time >= s.EndTime)
            {
                st.Done = true;
                st.DoneTime = s.EndTime;
                st.Tracking = false;
            }
        }

        void updateSpinner(int i, Spinner sp, ObjState st, double time, int keys)
        {
            var centre = new Vector2(256, 192);

            if (time >= sp.StartTime && time <= sp.EndTime)
            {
                var d = cursor - centre;
                float angle = MathF.Atan2(d.Y, d.X);

                if (st.HasAngle && keys != 0)
                {
                    double delta = angle - st.LastAngle;
                    if (delta > Math.PI) delta -= 2 * Math.PI;
                    if (delta < -Math.PI) delta += 2 * Math.PI;

                    double dt = Math.Max(0, time - st.LastSpinTime);
                    double cap = max_spin_per_ms * dt;
                    delta = Math.Clamp(delta, -cap, cap);

                    st.Rotation += Math.Abs(delta);
                    if (dt > 0)
                        st.Rpm = st.Rpm * 0.9 + Math.Abs(delta) / dt * 60000 / (2 * Math.PI) * 0.1;
                }

                st.LastAngle = angle;
                st.LastSpinTime = time;
                st.HasAngle = true;

                int spins = (int)(st.Rotation / (2 * Math.PI));
                while (st.SpinTicks < Math.Min(spins, sp.TotalTicks))
                {
                    var r = st.SpinTicks < sp.SpinsForBonus ? HitResult.SmallBonus : HitResult.LargeBonus;
                    Score.Apply(r, r);
                    Health.Apply(r);
                    if (r == HitResult.LargeBonus)
                        sound(SoundKind.SpinnerBonus, sp.Samples, time, 256);
                    st.SpinTicks++;
                }
            }

            if (time > sp.EndTime)
            {
                double spins = st.Rotation / (2 * Math.PI);
                double p = sp.SpinsRequired == 0 ? 1 : spins / sp.SpinsRequired;

                var r = p >= 1 ? HitResult.Great : p > 0.9 ? HitResult.Ok : p > 0.75 ? HitResult.Meh : HitResult.Miss;
                judgeHead(i, r, time);
                st.Done = true;
                st.DoneTime = sp.EndTime;
            }
        }

        // perfect play, mostly for checking maps on the console without a pad in hand
        void autoplay(double time, ref int keys)
        {
            keys = 0;

            for (int i = firstActive; i < objs.Count; i++)
            {
                var h = objs[i];
                if (h.StartTime > time + 1000) break;
                var st = states[i];
                if (st.Done) continue;

                if (h is Spinner sp && time >= sp.StartTime)
                {
                    double a = time * 0.05;
                    cursor = new Vector2(256, 192) + new Vector2(MathF.Cos((float)a), MathF.Sin((float)a)) * 50;
                    keys = 1;
                    return;
                }

                if (h is Slider s && time >= s.StartTime && st.Result != HitResult.None)
                {
                    cursor = s.StackedPosition + s.CurvePositionAt(Math.Clamp((time - s.StartTime) / s.Duration, 0, 1));
                    keys = 1;
                    return;
                }

                if (st.Result == HitResult.None && h is not Spinner)
                {
                    cursor = h.StackedPosition;
                    if (time >= h.StartTime)
                        keys = (prevKeys & 1) != 0 ? 2 : 1;
                    return;
                }
            }
        }

        public Vector2 Cursor => cursor;

        public void FillHud(ref Hud hud, double time)
        {
            hud.Score = Score.TotalScore;
            hud.Combo = Score.Combo;
            hud.MaxCombo = Score.MaxCombo;
            hud.Accuracy = (float)Score.Accuracy;
            hud.Health = (float)Health.Health;
            hud.Great = Score.Great;
            hud.Ok = Score.Ok;
            hud.Meh = Score.Meh;
            hud.Miss = Score.Miss;
            hud.State = State;
            hud.Rank = (int)Score.Rank;
            hud.UnstableRate = (float)Score.UnstableRate;
            hud.SkipTarget = SkipTarget(time);

            double first = objs.Count > 0 ? objs[0].StartTime : 0;
            hud.Progress = EndTime > first ? (float)Math.Clamp((time - first) / (EndTime - first), 0, 1) : 0;
        }

        uint colourOf(OsuObject h) => colours[((h.ComboIndexWithOffsets - 1) % colours.Length + colours.Length) % colours.Length];

        public void BuildDrawList(DrawList dl, double time)
        {
            dl.Clear();

            int last = firstActive;
            while (last < objs.Count && objs[last].StartTime - objs[last].TimePreempt <= time)
                last++;

            followPoints(dl, time, last);

            // earlier objects sit on top, so draw back to front
            for (int i = last - 1; i >= firstActive; i--)
            {
                switch (objs[i])
                {
                    case HitCircle c: drawCircle(dl, i, c, time); break;
                    case Slider s: drawSlider(dl, i, s, time); break;
                    case Spinner sp: drawSpinner(dl, i, sp, time); break;
                }
            }

            for (int i = last - 1; i >= firstActive; i--)
            {
                var h = objs[i];
                if (h is Spinner || states[i].Result != HitResult.None || time >= h.StartTime)
                    continue;
                // Hidden keeps only the first approach circle, to find the timing
                if (Hidden && i > 0)
                    continue;

                double appear = h.StartTime - h.TimePreempt;
                float t = (float)Math.Clamp((time - appear) / h.TimePreempt, 0, 1);
                float a = (float)Math.Clamp((time - appear) / Math.Min(h.TimeFadeIn * 2, h.TimePreempt), 0, 1) * 0.9f;

                dl.Add(new DrawCmd
                {
                    Kind = DrawKind.Approach,
                    Colour = colourOf(h),
                    X = h.StackedPosition.X,
                    Y = h.StackedPosition.Y,
                    Radius = h.Radius * (4 - 3 * t),
                    Alpha = a,
                });
            }

            for (int p = popups.Count - 1; p >= 0; p--)
            {
                double age = time - popups[p].Time;
                if (age > popup_duration || age < -50)
                {
                    if (age > popup_duration) popups.RemoveAt(p);
                    continue;
                }

                float k = (float)(age / popup_duration);
                dl.Add(new DrawCmd
                {
                    Kind = DrawKind.Judgement,
                    Number = (int)popups[p].Result,
                    X = popups[p].Pos.X,
                    Y = popups[p].Pos.Y,
                    Radius = objs.Count > 0 ? objs[0].Radius : 32,
                    Alpha = 1 - k * k,
                    P0 = k,
                });
            }

            dl.Add(new DrawCmd { Kind = DrawKind.Cursor, X = cursor.X, Y = cursor.Y, Alpha = 1 });
        }

        float fadeIn(OsuObject h, double time)
            => (float)Math.Clamp((time - (h.StartTime - h.TimePreempt)) / h.TimeFadeIn, 0, 1);

        double hiddenFrom(OsuObject h) => h.StartTime - h.TimePreempt + h.TimeFadeIn;

        // circles and slider heads
        float hiddenCircle(OsuObject h, double time)
        {
            if (!Hidden) return 1;
            return 1 - (float)Math.Clamp((time - hiddenFrom(h)) / (h.TimePreempt * hd_fade_out), 0, 1);
        }

        // slider bodies fade out over their whole length, easing out
        float hiddenBody(Slider s, double time)
        {
            if (!Hidden) return 1;
            double from = hiddenFrom(s);
            double k = Math.Clamp((time - from) / Math.Max(1, s.EndTime - from), 0, 1);
            return (float)((1 - k) * (1 - k));
        }

        void circleCmd(DrawList dl, OsuObject h, Vector2 pos, double time, ObjState st, int number, double hitTime)
        {
            float alpha = fadeIn(h, time) * hiddenCircle(h, time);
            float scale = 1;
            int flags = 0;

            if (st.Result != HitResult.None && time >= st.ResultTime)
            {
                double age = time - st.ResultTime;
                if (HitResults.IsHit(st.Result))
                {
                    if (age > explode_duration) return;
                    float k = (float)(age / explode_duration);
                    alpha = 1 - k;
                    scale = 1 + 0.4f * k;
                    flags |= 1;
                }
                else
                {
                    if (age > 100) return;
                    alpha *= 1 - (float)(age / 100);
                }
            }

            float shake = 0;
            double since = time - st.ShakeTime;
            if (since >= 0 && since < 120)
                shake = MathF.Sin((float)since * 0.25f) * 8 * (1 - (float)(since / 120));

            dl.Add(new DrawCmd
            {
                Kind = DrawKind.Circle,
                Colour = colourOf(h),
                Number = number,
                Flags = flags,
                X = pos.X + shake,
                Y = pos.Y,
                Radius = h.Radius * scale,
                Alpha = alpha,
            });
        }

        void drawCircle(DrawList dl, int i, HitCircle c, double time)
        {
            circleCmd(dl, c, c.StackedPosition, time, states[i], c.IndexInCombo + 1, c.StartTime);
        }

        void drawSlider(DrawList dl, int i, Slider s, double time)
        {
            var st = states[i];
            double appear = s.StartTime - s.TimePreempt;
            float alpha = fadeIn(s, time) * hiddenBody(s, time);

            if (time > s.EndTime)
            {
                double k = (time - s.EndTime) / explode_duration;
                if (k >= 1) return;
                alpha *= (float)(1 - k);
            }

            Vector2 off = s.StackOffset + s.Position;

            // snake in over the first third of the approach
            double snake = Math.Clamp((time - appear) / (s.TimePreempt / 3), 0, 1);

            var path = drawPaths[i];
            var lens = drawLengths[i];
            double total = lens.Count > 0 ? lens[^1] : 0;
            double upTo = total * snake;

            int start = dl.Points.Count;
            for (int k = 0; k < path.Count; k++)
            {
                if (lens[k] > upTo)
                {
                    if (k > 0)
                    {
                        double seg = lens[k] - lens[k - 1];
                        float w = seg > 0 ? (float)((upTo - lens[k - 1]) / seg) : 0;
                        dl.Points.Add(off + Vector2.Lerp(path[k - 1], path[k], w));
                    }
                    break;
                }

                dl.Points.Add(off + path[k]);
            }

            int count = dl.Points.Count - start;

            dl.Add(new DrawCmd
            {
                Kind = DrawKind.SliderBody,
                Colour = colourOf(s),
                X = off.X,
                Y = off.Y,
                Radius = s.Radius,
                Alpha = alpha,
                PathStart = start,
                PathCount = count,
                Number = i,
            });

            int span = time < s.StartTime ? 0 : Math.Min(s.SpanCount - 1, (int)((time - s.StartTime) / s.SpanDuration));

            // tail circle
            if (snake >= 1)
            {
                var tail = s.StackedPosition + s.Path.PositionAt(s.SpanCount % 2);
                dl.Add(new DrawCmd { Kind = DrawKind.Circle, Colour = colourOf(s), Number = -1, X = tail.X, Y = tail.Y, Radius = s.Radius, Alpha = alpha });
            }

            // ticks for the current span
            var results = st.NestedResults!;
            for (int k = 0; k < s.Nested.Count; k++)
            {
                var n = s.Nested[k];
                if (n.Kind != NestedKind.Tick || n.SpanIndex != span || results[k] != HitResult.None)
                    continue;
                if (n.PathProgress > snake)
                    continue;

                var p = n.Position + s.StackOffset;
                dl.Add(new DrawCmd { Kind = DrawKind.Tick, X = p.X, Y = p.Y, Radius = s.Radius * 0.15f, Alpha = alpha });
            }

            // reverse arrow at the far end of this span
            if (span < s.SpanCount - 1 && snake >= 1)
            {
                double endProgress = span % 2 == 0 ? 1 : 0;
                var at = s.Path.PositionAt(endProgress);
                var back = s.Path.PositionAt(endProgress == 1 ? 0.98 : 0.02);
                var dir = back - at;
                var p = at + off;

                dl.Add(new DrawCmd
                {
                    Kind = DrawKind.Reverse,
                    X = p.X,
                    Y = p.Y,
                    Radius = s.Radius,
                    Alpha = alpha,
                    P0 = MathF.Atan2(dir.Y, dir.X),
                });
            }

            // head
            if (st.Result == HitResult.None || time - st.ResultTime < explode_duration)
                circleCmd(dl, s, s.StackedPosition, time, st, s.IndexInCombo + 1, s.StartTime);

            if (time >= s.StartTime && time <= s.EndTime)
            {
                double progress = Math.Clamp((time - s.StartTime) / s.Duration, 0, 1);
                var ball = s.StackedPosition + s.CurvePositionAt(progress);
                var ahead = s.StackedPosition + s.CurvePositionAt(Math.Min(1, progress + 0.01));
                var dir = ahead - ball;

                float follow = (float)Math.Clamp((time - st.TrackingChanged) / 180, 0, 1);
                if (!st.Tracking) follow = 1 - follow;

                if (follow > 0)
                {
                    dl.Add(new DrawCmd
                    {
                        Kind = DrawKind.FollowCircle,
                        Colour = colourOf(s),
                        X = ball.X,
                        Y = ball.Y,
                        Radius = s.Radius * (1 + (FollowAreaScale - 1) * follow),
                        Alpha = follow,
                    });
                }

                dl.Add(new DrawCmd
                {
                    Kind = DrawKind.SliderBall,
                    Colour = colourOf(s),
                    X = ball.X,
                    Y = ball.Y,
                    Radius = s.Radius,
                    Alpha = 1,
                    P0 = MathF.Atan2(dir.Y, dir.X),
                    Flags = st.Tracking ? 1 : 0,
                });
            }
        }

        void drawSpinner(DrawList dl, int i, Spinner sp, double time)
        {
            var st = states[i];
            float alpha = (float)Math.Clamp((time - (sp.StartTime - sp.TimeFadeIn)) / sp.TimeFadeIn, 0, 1);

            if (time > sp.EndTime)
            {
                double k = (time - sp.EndTime) / explode_duration;
                if (k >= 1) return;
                alpha *= (float)(1 - k);
            }

            double spins = st.Rotation / (2 * Math.PI);
            float progress = sp.SpinsRequired == 0 ? 1 : (float)Math.Min(1, spins / sp.SpinsRequired);
            float remaining = sp.Duration > 0 ? (float)Math.Clamp(1 - (time - sp.StartTime) / sp.Duration, 0, 1) : 0;

            dl.Add(new DrawCmd
            {
                Kind = DrawKind.Spinner,
                X = 256,
                Y = 192,
                Radius = remaining,
                Alpha = alpha,
                P0 = (float)st.Rotation,
                P1 = progress,
                Number = (int)st.Rpm,
                Flags = st.SpinTicks > sp.SpinsForBonus ? st.SpinTicks - sp.SpinsForBonus : 0,
            });
        }

        void followPoints(DrawList dl, double time, int last)
        {
            int from = Math.Max(0, firstActive - 1);

            for (int i = from; i < Math.Min(last, objs.Count - 1); i++)
            {
                var a = objs[i];
                var b = objs[i + 1];
                if (b.NewCombo || a is Spinner || b is Spinner)
                    continue;

                var p0 = a.StackedEndPosition;
                var p1 = b.StackedPosition;
                float dist = Vector2.Distance(p0, p1);
                if (dist < 80) continue;

                double fadeInStart = a.EndTime - b.TimePreempt;
                double fadeOutEnd = b.StartTime;

                if (time < fadeInStart || time > fadeOutEnd) continue;

                float alpha = (float)Math.Min(1, (time - fadeInStart) / 200);
                if (time > b.StartTime - 200)
                    alpha *= (float)Math.Clamp((b.StartTime - time) / 200, 0, 1);

                // the line grows out of the first object
                float grow = (float)Math.Clamp((time - fadeInStart) / Math.Max(1, a.EndTime - fadeInStart + 1), 0, 1);

                dl.Add(new DrawCmd
                {
                    Kind = DrawKind.FollowPoint,
                    X = p0.X,
                    Y = p0.Y,
                    P0 = p0.X + (p1.X - p0.X) * grow,
                    P1 = p0.Y + (p1.Y - p0.Y) * grow,
                    Radius = a.Radius,
                    Alpha = alpha,
                });
            }
        }
    }
}
