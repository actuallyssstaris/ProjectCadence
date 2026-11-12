using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using Osu5.Objects;

namespace Osu5.Beatmaps
{
    // taiko and mania converts, ported from ppy/osu (MIT): TaikoBeatmapConverter,
    // ManiaBeatmapConverter and the legacy pattern generators. catch needs
    // nothing, CatchPlayer already handles osu! objects
    public static class MapConvert
    {
        public static void To(Beatmap map, int mode)
        {
            // taiko maps go through the same converter in lazer: short sliders
            // split, drum rolls get their taiko length
            if (map.Mode == 1)
                toTaiko(map);
            if (map.Mode != 0 || mode <= 0 || mode > 3)
                return;

            if (mode == 1)
                toTaiko(map);
            else if (mode == 3)
                new Mania(map).Run();
            map.Mode = mode;
        }

        // stable's precision-adjusted beat length; taiko and mania clamp to 10000
        static double adjustedBeat(Beatmap map, OsuObject h, double clampTop)
        {
            double beat = map.ControlPoints.TimingAt(h.StartTime).BeatLength;
            if (h is not Slider s)
                return beat;
            double asBeat = -100 / s.SliderVelocity;
            double mult = asBeat < 0 ? Math.Clamp((float)-asBeat, 10, (float)clampTop) / 100.0 : 1;
            return beat * mult;
        }

        // ---- taiko: short sliders become a run of hits instead of a drum roll ----

        const float velocity_multiplier = 1.4f;
        const float base_scoring_distance = 100;

        static void toTaiko(Beatmap map)
        {
            var objects = new List<OsuObject>(map.HitObjects.Count);
            var diff = map.Difficulty;
            foreach (var h in map.HitObjects)
            {
                if (h is not Slider s)
                {
                    objects.Add(h);
                    continue;
                }

                // not simplified on purpose: lazer keeps stable's float error
                int spans = s.SpanCount;
                double distance = s.Path.ExpectedDistance ?? s.Path.Distance;
                distance *= velocity_multiplier;
                distance *= spans;

                var timing = map.ControlPoints.TimingAt(h.StartTime);
                double beatLength = adjustedBeat(map, h, 10000);
                double scoringPointDistance = base_scoring_distance * (diff.SliderMultiplier * velocity_multiplier) / diff.SliderTickRate;
                double taikoVelocity = scoringPointDistance * diff.SliderTickRate;
                int taikoDuration = (int)(distance / taikoVelocity * beatLength);
                double osuVelocity = taikoVelocity * (1000f / beatLength);
                if (map.FormatVersion >= 8)
                    beatLength = timing.BeatLength;
                double tickSpacing = Math.Min(beatLength / diff.SliderTickRate, (double)taikoDuration / spans);

                if (!(tickSpacing > 0 && distance / osuVelocity * 1000 < 2 * beatLength))
                {
                    s.RollDuration = taikoDuration;
                    objects.Add(h);
                    continue;
                }

                var samples = s.NodeSamples.Count > 0 ? s.NodeSamples : new List<Samples> { s.Samples };
                int i = 0;
                for (double t = h.StartTime; t <= h.StartTime + taikoDuration + tickSpacing / 8; t += tickSpacing)
                {
                    objects.Add(new HitCircle { StartTime = t, Position = h.Position, Samples = samples[i] });
                    i = (i + 1) % samples.Count;
                    if (Math.Abs(tickSpacing) < 1e-6)
                        break;
                }
            }
            map.HitObjects = objects;
        }

        // key count, from how much of the map is
        // sliders and spinners, and from CS / OD
        public static int ManiaKeys(Beatmap map)
        {
            int total = map.HitObjects.Count, longs = 0;
            foreach (var h in map.HitObjects)
                if (h is Slider || h is Spinner)
                    longs++;
            double cs = Math.Round(map.Difficulty.CircleSize), od = Math.Round(map.Difficulty.OverallDifficulty);
            if (total > 0)
            {
                double share = (double)longs / total;
                if (share < 0.2)
                    return 7;
                if (share < 0.3 || cs >= 5)
                    return od > 5 ? 7 : 6;
                if (share > 0.6)
                    return od > 4 ? 5 : 4;
            }
            return Math.Max(4, Math.Min((int)od + 1, 7));
        }

        // ---- mania ----

        // stable's xorshift, seeded from the map's settings so a convert is always the same
        sealed class LegacyRandom
        {
            const double int_to_real = 1.0 / (int.MaxValue + 1.0);
            uint x, y = 842502087, z = 3579807591, w = 273326509;

            public LegacyRandom(int seed) => x = (uint)seed;

            uint nextUInt()
            {
                uint t = x ^ (x << 11);
                x = y;
                y = z;
                z = w;
                return w = w ^ (w >> 19) ^ t ^ (t >> 8);
            }

            public int Next() => (int)(0x7FFFFFFF & nextUInt());
            public double NextDouble() => int_to_real * Next();
            public int Next(int lo, int hi) => (int)(lo + NextDouble() * (hi - lo));
        }

        [Flags]
        enum PatternType
        {
            None = 0,
            ForceStack = 1,
            ForceNotStack = 1 << 1,
            KeepSingle = 1 << 2,
            LowProbability = 1 << 3,
            Alternate = 1 << 4,
            ForceSigSlider = 1 << 5,
            ForceNotSlider = 1 << 6,
            Gathered = 1 << 7,
            Mirror = 1 << 8,
            Reverse = 1 << 9,
            Cycle = 1 << 10,
            Stair = 1 << 11,
            ReverseStair = 1 << 12,
        }

        sealed class Note
        {
            public int Column;
            public double Start, End;
            public Samples Samples;
        }

        sealed class Pattern
        {
            public readonly List<Note> Notes = new List<Note>();
            readonly HashSet<int> columns = new HashSet<int>();

            public bool Has(int column) => columns.Contains(column);
            public int ColumnCount => columns.Count;

            public void Add(Note n)
            {
                Notes.Add(n);
                columns.Add(n.Column);
            }

            public void Add(Pattern other)
            {
                foreach (var n in other.Notes)
                    Add(n);
            }

            public void Clear()
            {
                Notes.Clear();
                columns.Clear();
            }
        }

        sealed class NotEnoughColumns : Exception
        {
        }

        sealed class Mania
        {
            readonly Beatmap map;
            readonly LegacyRandom random;
            readonly int columns, randomStart;
            readonly double conversionDifficulty;

            Pattern lastPattern = new Pattern();
            readonly Queue<double> recent = new Queue<double>();
            double density = int.MaxValue, lastTime;
            Vector2 lastPosition;
            PatternType lastStair = PatternType.Stair;

            public Mania(Beatmap m)
            {
                map = m;
                var d = m.Difficulty;
                random = new LegacyRandom((int)MathF.Round(d.DrainRate + d.CircleSize) * 20 + (int)(d.OverallDifficulty * 41.2) + (int)MathF.Round(d.ApproachRate));
                columns = ManiaKeys(m);
                randomStart = columns == 8 ? 1 : 0;

                double breaks = 0;
                foreach (var b in m.Breaks)
                    breaks += b.Duration;
                var objs = m.HitObjects;
                int drain = (int)(((objs.Count > 0 ? objs[^1].StartTime : 0) - (objs.Count > 0 ? objs[0].StartTime : 0) - breaks) / 1000);
                if (drain == 0)
                    drain = 10000;
                double cd = ((d.DrainRate + Math.Clamp(d.ApproachRate, 4, 7)) / 1.5 + (double)objs.Count / drain * 9f) / 38f * 5f / 1.15;
                conversionDifficulty = Math.Min(cd, 12);
            }

            public void Run()
            {
                var output = new List<Note>();
                foreach (var h in map.HitObjects)
                {
                    try
                    {
                        convert(h, output);
                    }
                    catch (NotEnoughColumns)
                    {
                        // lazer gives up on such a map; we just drop the object
                    }
                }
                output.Sort((a, b) => a.Start.CompareTo(b.Start));

                var objects = new List<OsuObject>(output.Count);
                foreach (var n in output)
                {
                    var pos = new Vector2((n.Column + 0.5f) * 512f / columns, 192);
                    if (n.End > n.Start)
                        objects.Add(new HoldNote { StartTime = n.Start, End = n.End, Position = pos, Samples = n.Samples });
                    else
                        objects.Add(new HitCircle { StartTime = n.Start, Position = pos, Samples = n.Samples });
                }
                map.HitObjects = objects;
                map.Difficulty.CircleSize = columns;
            }

            void addDensity(double t)
            {
                recent.Enqueue(t);
                while (recent.Count > 7)
                    recent.Dequeue();
                if (recent.Count >= 2)
                    density = (recent.Last() - recent.Peek()) / recent.Count;
            }

            void convert(OsuObject h, List<Note> output)
            {
                switch (h)
                {
                    case Slider s:
                    {
                        var g = new SliderGen(this, s);
                        for (int i = 0; i <= g.SpanCount; i++)
                        {
                            double t = s.StartTime + g.SegmentDuration * i;
                            lastTime = t;
                            lastPosition = s.Position;
                            addDensity(t);
                        }
                        foreach (var p in g.Generate())
                        {
                            lastPattern = p;
                            output.AddRange(p.Notes);
                        }
                        break;
                    }
                    case Spinner sp:
                    {
                        var p = new SpinnerGen(this, sp).Generate();
                        lastTime = sp.EndTime;
                        lastPosition = new Vector2(256, 192);
                        addDensity(sp.EndTime);
                        output.AddRange(p.Notes);
                        break;
                    }
                    default:
                    {
                        addDensity(h.StartTime);
                        var g = new CircleGen(this, h, lastTime, lastPosition, density, lastStair);
                        lastTime = h.StartTime;
                        lastPosition = h.Position;
                        var p = g.Generate();
                        lastStair = g.StairType;
                        lastPattern = p;
                        output.AddRange(p.Notes);
                        break;
                    }
                }
            }

            // ---- shared by the generators (LegacyPatternGenerator) ----

            abstract class Gen
            {
                protected readonly Mania M;
                protected readonly OsuObject H;
                protected readonly Pattern Previous;

                protected Gen(Mania m, OsuObject h)
                {
                    M = m;
                    H = h;
                    Previous = m.lastPattern;
                }

                protected int Columns => M.columns;
                protected int RandomStart => M.randomStart;
                protected double ConversionDifficulty => M.conversionDifficulty;
                protected LegacyRandom Random => M.random;

                protected bool HasSound(HitSound s) => (H.Samples.Sound & s) != 0;

                protected int GetColumn(float position, bool allowSpecial = false)
                {
                    if (allowSpecial && Columns == 8)
                        return Math.Clamp((int)MathF.Floor(position / (512f / 7)), 0, 6) + 1;
                    return Math.Clamp((int)MathF.Floor(position / (512f / Columns)), 0, Columns - 1);
                }

                protected int GetRandomNoteCount(double p2, double p3, double p4 = 0, double p5 = 0, double p6 = 0)
                {
                    double val = Random.NextDouble();
                    if (val >= 1 - p6) return 6;
                    if (val >= 1 - p5) return 5;
                    if (val >= 1 - p4) return 4;
                    if (val >= 1 - p3) return 3;
                    return val >= 1 - p2 ? 2 : 1;
                }

                protected int GetRandomColumn(int? lo = null, int? hi = null) => Random.Next(lo ?? RandomStart, hi ?? Columns);

                protected int FindAvailableColumn(int initial, params Pattern[] patterns) => FindAvailableColumn(initial, null, null, null, null, patterns);

                protected int FindAvailableColumn(int initial, int? lo, int? hi, Func<int, int>? next, Func<int, bool>? validation, params Pattern[] patterns)
                {
                    int lower = lo ?? RandomStart, upper = hi ?? Columns;
                    next ??= _ => GetRandomColumn(lower, upper);

                    bool valid(int c)
                    {
                        if (validation != null && !validation(c))
                            return false;
                        foreach (var p in patterns)
                            if (p.Has(c))
                                return false;
                        return true;
                    }

                    if (valid(initial))
                        return initial;
                    bool any = false;
                    for (int i = lower; i < upper && !any; i++)
                        any = valid(i);
                    if (!any)
                        throw new NotEnoughColumns();
                    do
                        initial = next(initial);
                    while (!valid(initial));
                    return initial;
                }
            }

            // ---- circles (HitCirclePatternGenerator) ----

            sealed class CircleGen : Gen
            {
                public PatternType StairType;
                readonly PatternType type;

                public CircleGen(Mania m, OsuObject h, double prevTime, Vector2 prevPos, double density, PatternType lastStair) : base(m, h)
                {
                    StairType = lastStair;
                    double beat = m.map.ControlPoints.TimingAt(h.StartTime).BeatLength;
                    bool kiai = m.map.ControlPoints.KiaiAt(h.StartTime);
                    float posSep = (h.Position - prevPos).Length();
                    double timeSep = h.StartTime - prevTime;

                    if (timeSep <= 80)
                        type |= PatternType.ForceNotStack | PatternType.KeepSingle;
                    else if (timeSep <= 95)
                        type |= PatternType.ForceNotStack | PatternType.KeepSingle | lastStair;
                    else if (timeSep <= 105)
                        type |= PatternType.ForceNotStack | PatternType.LowProbability;
                    else if (timeSep <= 125)
                        type |= PatternType.ForceNotStack;
                    else if (timeSep <= 135 && posSep < 20)
                        type |= PatternType.Cycle | PatternType.KeepSingle;
                    else if (timeSep <= 150 && posSep < 20)
                        type |= PatternType.ForceStack | PatternType.LowProbability;
                    else if (posSep < 20 && density >= beat / 2.5)
                        type |= PatternType.Reverse | PatternType.LowProbability;
                    else if (density < beat / 2.5 || kiai)
                    {
                        // high density, nothing extra
                    }
                    else
                        type |= PatternType.LowProbability;

                    if (!type.HasFlag(PatternType.KeepSingle))
                    {
                        if (HasSound(HitSound.Finish) && Columns != 8)
                            type |= PatternType.Mirror;
                        else if (HasSound(HitSound.Clap))
                            type |= PatternType.Gathered;
                    }
                }

                public Pattern Generate()
                {
                    var p = core();
                    foreach (var n in p.Notes)
                    {
                        if (type.HasFlag(PatternType.Stair) && n.Column == Columns - 1)
                            StairType = PatternType.ReverseStair;
                        if (type.HasFlag(PatternType.ReverseStair) && n.Column == RandomStart)
                            StairType = PatternType.Stair;
                    }
                    return p;
                }

                Pattern core()
                {
                    var pattern = new Pattern();
                    if (Columns == 1)
                    {
                        add(pattern, 0);
                        return pattern;
                    }
                    int lastColumn = Previous.Notes.Count > 0 ? Previous.Notes[0].Column : 0;

                    if (type.HasFlag(PatternType.Reverse) && Previous.Notes.Count > 0)
                    {
                        for (int i = RandomStart; i < Columns; i++)
                            if (Previous.Has(i))
                                add(pattern, RandomStart + Columns - i - 1);
                        return pattern;
                    }
                    if (type.HasFlag(PatternType.Cycle) && Previous.Notes.Count == 1 && (Columns != 8 || lastColumn != 0) &&
                        (Columns % 2 == 0 || lastColumn != Columns / 2))
                    {
                        add(pattern, RandomStart + Columns - lastColumn - 1);
                        return pattern;
                    }
                    if (type.HasFlag(PatternType.ForceStack) && Previous.Notes.Count > 0)
                    {
                        for (int i = RandomStart; i < Columns; i++)
                            if (Previous.Has(i))
                                add(pattern, i);
                        return pattern;
                    }
                    if (Previous.Notes.Count == 1)
                    {
                        if (type.HasFlag(PatternType.Stair))
                        {
                            int target = lastColumn + 1;
                            if (target == Columns)
                                target = RandomStart;
                            add(pattern, target);
                            return pattern;
                        }
                        if (type.HasFlag(PatternType.ReverseStair))
                        {
                            int target = lastColumn - 1;
                            if (target == RandomStart - 1)
                                target = Columns - 1;
                            add(pattern, target);
                            return pattern;
                        }
                    }
                    if (type.HasFlag(PatternType.KeepSingle))
                        return randomNotes(1);
                    if (type.HasFlag(PatternType.Mirror))
                    {
                        if (ConversionDifficulty > 6.5)
                            return mirrored(0.12, 0.38, 0.12);
                        if (ConversionDifficulty > 4)
                            return mirrored(0.12, 0.17, 0);
                        return mirrored(0.12, 0, 0);
                    }
                    bool low = type.HasFlag(PatternType.LowProbability);
                    if (ConversionDifficulty > 6.5)
                        return low ? randomPattern(0.78, 0.42, 0, 0) : randomPattern(1, 0.62, 0, 0);
                    if (ConversionDifficulty > 4)
                        return low ? randomPattern(0.35, 0.08, 0, 0) : randomPattern(0.52, 0.15, 0, 0);
                    if (ConversionDifficulty > 2)
                        return low ? randomPattern(0.18, 0, 0, 0) : randomPattern(0.45, 0, 0, 0);
                    return randomPattern(0, 0, 0, 0);
                }

                Pattern randomNotes(int count)
                {
                    var pattern = new Pattern();
                    bool stacking = !type.HasFlag(PatternType.ForceNotStack);
                    if (!stacking)
                        count = Math.Min(count, Columns - RandomStart - Previous.ColumnCount);
                    int next = GetColumn(H.Position.X, true);

                    int nextColumn(int last)
                    {
                        if (type.HasFlag(PatternType.Gathered))
                        {
                            last++;
                            if (last == Columns)
                                last = RandomStart;
                            return last;
                        }
                        return GetRandomColumn();
                    }

                    for (int i = 0; i < count; i++)
                    {
                        next = stacking
                            ? FindAvailableColumn(next, null, null, nextColumn, null, pattern)
                            : FindAvailableColumn(next, null, null, nextColumn, null, pattern, Previous);
                        add(pattern, next);
                    }
                    return pattern;
                }

                bool special => HasSound(HitSound.Clap) && HasSound(HitSound.Finish);

                Pattern randomPattern(double p2, double p3, double p4, double p5)
                {
                    var pattern = new Pattern();
                    pattern.Add(randomNotes(noteCount(p2, p3, p4, p5)));
                    if (RandomStart > 0 && special)
                        add(pattern, 0);
                    return pattern;
                }

                Pattern mirrored(double centre, double p2, double p3)
                {
                    if (type.HasFlag(PatternType.ForceNotStack))
                        return randomPattern(1 / 2f + p2 / 2, p2, (p2 + p3) / 2, p3);
                    var pattern = new Pattern();
                    int count = noteCountMirrored(centre, p2, p3, out bool addCentre);
                    int limit = (Columns % 2 == 0 ? Columns : Columns - 1) / 2;
                    int next = GetRandomColumn(null, limit);
                    for (int i = 0; i < count; i++)
                    {
                        next = FindAvailableColumn(next, null, limit, null, null, pattern);
                        add(pattern, next);
                        add(pattern, RandomStart + Columns - next - 1);
                    }
                    if (addCentre)
                        add(pattern, Columns / 2);
                    if (RandomStart > 0 && special)
                        add(pattern, 0);
                    return pattern;
                }

                int noteCount(double p2, double p3, double p4, double p5)
                {
                    switch (Columns)
                    {
                        case 2: p2 = 0; p3 = 0; p4 = 0; p5 = 0; break;
                        case 3: p2 = Math.Min(p2, 0.1); p3 = 0; p4 = 0; p5 = 0; break;
                        case 4: p2 = Math.Min(p2, 0.23); p3 = Math.Min(p3, 0.04); p4 = 0; p5 = 0; break;
                        case 5: p3 = Math.Min(p3, 0.15); p4 = Math.Min(p4, 0.03); p5 = 0; break;
                    }
                    if (HasSound(HitSound.Clap))
                        p2 = 1;
                    return GetRandomNoteCount(p2, p3, p4, p5);
                }

                int noteCountMirrored(double centre, double p2, double p3, out bool addCentre)
                {
                    switch (Columns)
                    {
                        case 2: centre = 0; p2 = 0; p3 = 0; break;
                        case 3: centre = Math.Min(centre, 0.03); p2 = 0; p3 = 0; break;
                        case 4: centre = 0; p2 = 1 - Math.Max((1 - p2) * 2, 0.8); p3 = 0; break;
                        case 5: centre = Math.Min(centre, 0.03); p3 = 0; break;
                        case 6: centre = 0; p2 = 1 - Math.Max((1 - p2) * 2, 0.5); p3 = 1 - Math.Max((1 - p3) * 2, 0.85); break;
                    }
                    p2 = Math.Clamp(p2, 0, 1);
                    p3 = Math.Clamp(p3, 0, 1);
                    double centreVal = Random.NextDouble();
                    int count = GetRandomNoteCount(p2, p3);
                    addCentre = Columns % 2 != 0 && count != 3 && centreVal > 1 - centre;
                    return count;
                }

                void add(Pattern p, int column) => p.Add(new Note { Column = column, Start = H.StartTime, End = H.StartTime, Samples = H.Samples });
            }

            // ---- sliders (SliderPatternGenerator) ----

            sealed class SliderGen : Gen
            {
                public readonly int StartTime, EndTime, SegmentDuration, SpanCount;
                readonly Slider s;
                PatternType type;

                public SliderGen(Mania m, Slider slider) : base(m, slider)
                {
                    s = slider;
                    if (!m.map.ControlPoints.KiaiAt(slider.StartTime))
                        type = PatternType.LowProbability;
                    double beat = adjustedBeat(m.map, slider, 10000);
                    SpanCount = slider.SpanCount;
                    StartTime = (int)Math.Round(slider.StartTime);
                    double distance = slider.Path.ExpectedDistance ?? slider.Path.Distance;
                    EndTime = (int)Math.Floor(StartTime + distance * beat * SpanCount * 0.01 / m.map.Difficulty.SliderMultiplier);
                    SegmentDuration = (EndTime - StartTime) / SpanCount;
                }

                public IEnumerable<Pattern> Generate()
                {
                    var original = generate();
                    if (original.Notes.Count == 1)
                    {
                        yield return original;
                        yield break;
                    }
                    var middle = new Pattern();
                    var atEnd = new Pattern();
                    foreach (var n in original.Notes)
                    {
                        if (EndTime != (int)Math.Round(n.End))
                            middle.Add(n);
                        else
                            atEnd.Add(n);
                    }
                    yield return middle;
                    yield return atEnd;
                }

                Pattern generate()
                {
                    if (Columns == 1)
                    {
                        var p = new Pattern();
                        add(p, 0, StartTime, EndTime);
                        return p;
                    }
                    if (SpanCount > 1)
                    {
                        if (SegmentDuration <= 90)
                            return randomHolds(StartTime, 1);
                        if (SegmentDuration <= 120)
                        {
                            type |= PatternType.ForceNotStack;
                            return randomNotes(StartTime, SpanCount + 1);
                        }
                        if (SegmentDuration <= 160)
                            return stair(StartTime);
                        if (SegmentDuration <= 200 && ConversionDifficulty > 3)
                            return randomMultiple(StartTime);
                        double duration = EndTime - StartTime;
                        if (duration >= 4000)
                            return nRandom(StartTime, 0.23, 0, 0);
                        if (SegmentDuration > 400 && SpanCount < Columns - 1 - RandomStart)
                            return tiledHolds(StartTime);
                        return holdAndNotes(StartTime);
                    }
                    if (SegmentDuration <= 110)
                    {
                        if (Previous.ColumnCount < Columns)
                            type |= PatternType.ForceNotStack;
                        else
                            type &= ~PatternType.ForceNotStack;
                        return randomNotes(StartTime, SegmentDuration < 80 ? 1 : 2);
                    }
                    bool low = type.HasFlag(PatternType.LowProbability);
                    if (ConversionDifficulty > 6.5)
                        return low ? nRandom(StartTime, 0.78, 0.3, 0) : nRandom(StartTime, 0.85, 0.36, 0.03);
                    if (ConversionDifficulty > 4)
                        return low ? nRandom(StartTime, 0.43, 0.08, 0) : nRandom(StartTime, 0.56, 0.18, 0);
                    if (ConversionDifficulty > 2.5)
                        return low ? nRandom(StartTime, 0.3, 0, 0) : nRandom(StartTime, 0.37, 0.08, 0);
                    return low ? nRandom(StartTime, 0.17, 0, 0) : nRandom(StartTime, 0.27, 0, 0);
                }

                Pattern randomHolds(int start, int count)
                {
                    var p = new Pattern();
                    int usable = Columns - RandomStart - Previous.ColumnCount;
                    int next = GetRandomColumn();
                    for (int i = 0; i < Math.Min(usable, count); i++)
                    {
                        next = FindAvailableColumn(next, p, Previous);
                        add(p, next, start, EndTime);
                    }
                    for (int i = 0; i < count - usable; i++)
                    {
                        next = FindAvailableColumn(next, p);
                        add(p, next, start, EndTime);
                    }
                    return p;
                }

                Pattern randomNotes(int start, int count)
                {
                    var p = new Pattern();
                    int next = GetColumn(H.Position.X, true);
                    if (type.HasFlag(PatternType.ForceNotStack) && Previous.ColumnCount < Columns)
                        next = FindAvailableColumn(next, Previous);
                    int last = next;
                    for (int i = 0; i < count; i++)
                    {
                        add(p, next, start, start);
                        int avoid = last;
                        next = FindAvailableColumn(next, null, null, null, c => c != avoid);
                        last = next;
                        start += SegmentDuration;
                    }
                    return p;
                }

                Pattern stair(int start)
                {
                    var p = new Pattern();
                    int column = GetColumn(H.Position.X, true);
                    bool up = Random.NextDouble() > 0.5;
                    for (int i = 0; i <= SpanCount; i++)
                    {
                        add(p, column, start, start);
                        start += SegmentDuration;
                        if (up)
                        {
                            if (column >= Columns - 1)
                            {
                                up = false;
                                column--;
                            }
                            else
                                column++;
                        }
                        else
                        {
                            if (column <= RandomStart)
                            {
                                up = true;
                                column++;
                            }
                            else
                                column--;
                        }
                    }
                    return p;
                }

                Pattern randomMultiple(int start)
                {
                    var p = new Pattern();
                    bool legacy = Columns >= 4 && Columns <= 8;
                    int interval = Random.Next(1, Columns - (legacy ? 1 : 0));
                    int next = GetColumn(H.Position.X, true);
                    for (int i = 0; i <= SpanCount; i++)
                    {
                        add(p, next, start, start);
                        next += interval;
                        if (next >= Columns - RandomStart)
                            next = next - Columns - RandomStart + (legacy ? 1 : 0);
                        next += RandomStart;
                        if (Columns > 2)
                            add(p, next, start, start);
                        next = GetRandomColumn();
                        start += SegmentDuration;
                    }
                    return p;
                }

                Pattern nRandom(int start, double p2, double p3, double p4)
                {
                    switch (Columns)
                    {
                        case 2: p2 = 0; p3 = 0; p4 = 0; break;
                        case 3: p2 = Math.Min(p2, 0.1); p3 = 0; p4 = 0; break;
                        case 4: p2 = Math.Min(p2, 0.3); p3 = Math.Min(p3, 0.04); p4 = 0; break;
                        case 5: p2 = Math.Min(p2, 0.34); p3 = Math.Min(p3, 0.1); p4 = Math.Min(p4, 0.03); break;
                    }
                    const HitSound doubles = HitSound.Clap | HitSound.Finish;
                    bool two = !type.HasFlag(PatternType.LowProbability) &&
                               ((H.Samples.Sound & doubles) != 0 || (samplesAt(StartTime).Sound & doubles) != 0);
                    if (two)
                        p2 = 1;
                    return randomHolds(start, GetRandomNoteCount(p2, p3, p4));
                }

                Pattern tiledHolds(int start)
                {
                    var p = new Pattern();
                    int repeat = Math.Min(SpanCount, Columns);
                    int end = start + SegmentDuration * SpanCount;
                    int next = GetColumn(H.Position.X, true);
                    if (type.HasFlag(PatternType.ForceNotStack) && Previous.ColumnCount < Columns)
                        next = FindAvailableColumn(next, Previous);
                    for (int i = 0; i < repeat; i++)
                    {
                        next = FindAvailableColumn(next, p);
                        add(p, next, start, end);
                        start += SegmentDuration;
                    }
                    return p;
                }

                Pattern holdAndNotes(int start)
                {
                    var p = new Pattern();
                    int hold = GetColumn(H.Position.X, true);
                    if (type.HasFlag(PatternType.ForceNotStack) && Previous.ColumnCount < Columns)
                        hold = FindAvailableColumn(hold, Previous);
                    add(p, hold, start, EndTime);

                    int next = GetRandomColumn();
                    int count;
                    if (ConversionDifficulty > 6.5)
                        count = GetRandomNoteCount(0.63, 0);
                    else if (ConversionDifficulty > 4)
                        count = GetRandomNoteCount(Columns < 6 ? 0.12 : 0.45, 0);
                    else if (ConversionDifficulty > 2.5)
                        count = GetRandomNoteCount(Columns < 6 ? 0 : 0.24, 0);
                    else
                        count = 0;
                    count = Math.Min(Columns - 1, count);

                    bool ignoreHead = (samplesAt(start).Sound & (HitSound.Whistle | HitSound.Finish | HitSound.Clap)) == 0;
                    var row = new Pattern();
                    for (int i = 0; i <= SpanCount; i++)
                    {
                        if (!(ignoreHead && start == StartTime))
                        {
                            for (int j = 0; j < count; j++)
                            {
                                next = FindAvailableColumn(next, null, null, null, c => c != hold, row);
                                add(row, next, start, start);
                            }
                        }
                        p.Add(row);
                        row.Clear();
                        start += SegmentDuration;
                    }
                    return p;
                }

                Samples samplesAt(int time)
                {
                    if (s.NodeSamples.Count == 0)
                        return s.Samples;
                    int index = SegmentDuration == 0 ? 0 : (time - StartTime) / SegmentDuration;
                    return index < s.NodeSamples.Count ? s.NodeSamples[Math.Max(0, index)] : s.Samples;
                }

                void add(Pattern p, int column, int start, int end) =>
                    p.Add(new Note { Column = column, Start = start, End = end, Samples = start == end ? samplesAt(start) : H.Samples });
            }

            // ---- spinners (SpinnerPatternGenerator) ----

            sealed class SpinnerGen : Gen
            {
                readonly int end;
                readonly PatternType type;

                public SpinnerGen(Mania m, Spinner sp) : base(m, sp)
                {
                    end = (int)sp.EndTime;
                    type = Previous.ColumnCount == Columns ? PatternType.None : PatternType.ForceNotStack;
                }

                public Pattern Generate()
                {
                    var p = new Pattern();
                    bool hold = end - H.StartTime >= 100;
                    if (Columns == 8 && HasSound(HitSound.Finish) && end - H.StartTime < 1000)
                        add(p, 0, hold);
                    else if (Columns == 8)
                        add(p, column(null), hold);
                    else
                        add(p, column(0), hold);
                    return p;
                }

                int column(int? lo)
                {
                    if ((type & PatternType.ForceNotStack) != 0)
                        return FindAvailableColumn(GetRandomColumn(lo), lo, null, null, null, Previous);
                    return FindAvailableColumn(GetRandomColumn(lo), lo, null, null, null);
                }

                void add(Pattern p, int column, bool hold) =>
                    p.Add(new Note { Column = column, Start = H.StartTime, End = hold ? end : H.StartTime, Samples = H.Samples });
            }
        }
    }
}
