// .osu decoder. Follows osu!'s LegacyBeatmapDecoder and ConvertHitObjectParser
// (https://github.com/ppy/osu, MIT licence) but only keeps what gameplay needs.
// Storyboards, hitsound filenames and the editor section are skipped.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Numerics;
using Osu5.Objects;
using Osu5.Paths;
using Osu5.Play;
using Osu5.Util;

namespace Osu5.Beatmaps
{
    public sealed class BeatmapDecoder
    {
        const int first_lazer_version = 128;
        const int early_version_offset = 24;
        const double max_coord = 131072;

        enum Section { None, General, Editor, Metadata, Difficulty, Events, TimingPoints, Colours, HitObjects, Variables, Unknown }

        Beatmap map = null!;
        int version;
        double offset;
        bool hasApproachRate;
        int defaultBank = 1;
        int defaultVolume = 100;

        OsuObject? lastObject;

        // timing + inherited lines sharing a timestamp get merged, inherited wins
        double pendingTime = double.NaN;
        TimingPoint? pendingTiming;
        DifficultyPoint? pendingDiffFromTiming, pendingDiff;
        EffectPoint? pendingEffect;
        SamplePoint? pendingSample;

        public static Beatmap Decode(string path, Mods mods = Mods.None)
        {
            using var reader = new StreamReader(path);
            return new BeatmapDecoder { mods = mods, flipY = (mods & Mods.HardRock) != 0 }.decode(reader);
        }

        Mods mods;

        // Hard Rock mirrors the map top to bottom. Doing it while parsing gives the same
        // result as lazer flipping after stacking, since stacking only looks at distances.
        bool flipY;

        public static Beatmap Decode(TextReader reader) => new BeatmapDecoder().decode(reader);

        bool headerOnly;

        // metadata + difficulty only, for building the song list without parsing every object
        public static Beatmap Peek(string path)
        {
            using var reader = new StreamReader(path);
            return new BeatmapDecoder { headerOnly = true }.decode(reader);
        }

        Beatmap decode(TextReader reader)
        {
            map = new Beatmap();
            version = 14;

            string? line = reader.ReadLine();
            while (line != null && line.Trim().Length == 0)
                line = reader.ReadLine();

            if (line != null)
            {
                line = line.TrimStart('﻿').Trim();
                const string magic = "osu file format v";
                if (line.StartsWith(magic, StringComparison.Ordinal) && int.TryParse(line.AsSpan(magic.Length), out int v))
                    version = v;
            }

            map.FormatVersion = version;
            offset = version < 5 ? early_version_offset : 0;

            var section = Section.None;

            while ((line = reader.ReadLine()) != null)
            {
                if (line.Length == 0 || line.StartsWith("//", StringComparison.Ordinal) || line.StartsWith(' ') || line.StartsWith('_'))
                    continue;

                line = line.TrimEnd();
                if (line.Length == 0)
                    continue;

                if (line[0] == '[' && line[^1] == ']')
                {
                    section = Enum.TryParse(line.AsSpan(1, line.Length - 2), out Section s) ? s : Section.Unknown;
                    if (headerOnly && section == Section.HitObjects)
                        break;
                    continue;
                }

                try
                {
                    parseLine(section, line);
                }
                catch (Exception)
                {
                    // one broken line shouldn't kill the whole map
                }
            }

            flushPoints();
            clampDifficulty();

            if (headerOnly)
                return map;

            ModRules.ApplyToDifficulty(map.Difficulty, mods);

            map.HitObjects = map.HitObjects.OrderBy(h => h.StartTime).ToList();
            postProcessBreaks();

            foreach (var h in map.HitObjects)
                h.ApplyDefaults(map.ControlPoints, map.Difficulty, version);

            Combo.Assign(map.HitObjects);
            Stacking.Apply(map);

            return map;
        }

        void parseLine(Section section, string line)
        {
            switch (section)
            {
                case Section.General: general(line); break;
                case Section.Metadata: metadata(line); break;
                case Section.Difficulty: difficulty(line); break;
                case Section.Events: events(line); break;
                case Section.TimingPoints: timingPoint(line); break;
                case Section.Colours: colours(line); break;
                case Section.HitObjects: hitObject(line); break;
            }
        }

        static (string Key, string Value) keyVal(string line)
        {
            int i = line.IndexOf(':');
            if (i < 0) return (line.Trim(), "");
            return (line[..i].Trim(), line[(i + 1)..].Trim());
        }

        void general(string line)
        {
            var (k, v) = keyVal(line);

            switch (k)
            {
                case "AudioFilename": map.Metadata.AudioFile = v.Replace('\\', '/'); break;
                case "AudioLeadIn": map.AudioLeadIn = parseInt(v); break;
                case "PreviewTime":
                    int t = parseInt(v);
                    map.Metadata.PreviewTime = t == -1 ? t : t + (int)offset;
                    break;
                case "SampleSet": defaultBank = bankFromName(v); break;
                case "SampleVolume": defaultVolume = parseInt(v); break;
                case "StackLeniency": map.StackLeniency = parseFloat(v); break;
                case "Mode": map.Mode = parseInt(v); break;
            }
        }

        void metadata(string line)
        {
            var (k, v) = keyVal(line);
            var m = map.Metadata;

            switch (k)
            {
                case "Title": m.Title = v; break;
                case "TitleUnicode": m.TitleUnicode = v; break;
                case "Artist": m.Artist = v; break;
                case "ArtistUnicode": m.ArtistUnicode = v; break;
                case "Creator": m.Creator = v; break;
                case "Version": m.Version = v; break;
                case "Source": m.Source = v; break;
                case "Tags": m.Tags = v; break;
                case "BeatmapID": m.BeatmapId = parseInt(v); break;
                case "BeatmapSetID": m.BeatmapSetId = parseInt(v); break;
            }
        }

        void difficulty(string line)
        {
            var (k, v) = keyVal(line);
            var d = map.Difficulty;

            switch (k)
            {
                case "HPDrainRate": d.DrainRate = parseFloat(v); break;
                case "CircleSize": d.CircleSize = parseFloat(v); break;
                case "OverallDifficulty":
                    d.OverallDifficulty = parseFloat(v);
                    if (!hasApproachRate) d.ApproachRate = d.OverallDifficulty;
                    break;
                case "ApproachRate":
                    d.ApproachRate = parseFloat(v);
                    hasApproachRate = true;
                    break;
                case "SliderMultiplier": d.SliderMultiplier = parseDouble(v); break;
                case "SliderTickRate": d.SliderTickRate = parseDouble(v); break;
            }
        }

        void clampDifficulty()
        {
            var d = map.Difficulty;
            d.DrainRate = Math.Clamp(d.DrainRate, 0, 10);
            d.CircleSize = Math.Clamp(d.CircleSize, 0, 10);
            d.OverallDifficulty = Math.Clamp(d.OverallDifficulty, 0, 10);
            d.ApproachRate = Math.Clamp(d.ApproachRate, 0, 10);
            d.SliderMultiplier = Math.Clamp(d.SliderMultiplier, 0.4, 3.6);
            d.SliderTickRate = Math.Clamp(d.SliderTickRate, 0.5, 8);
        }

        void events(string line)
        {
            string[] s = line.Split(',');
            string type = s[0].Trim();

            switch (type)
            {
                case "0":
                case "Background":
                    if (s.Length > 2) map.Metadata.BackgroundFile = cleanFilename(s[2]);
                    break;

                case "1":
                case "Video":
                    // some old maps tag the background as a video
                    if (s.Length > 2)
                    {
                        string f = cleanFilename(s[2]);
                        string ext = Path.GetExtension(f).ToLowerInvariant();
                        if (ext is ".jpg" or ".jpeg" or ".png")
                            map.Metadata.BackgroundFile = f;
                    }
                    break;

                case "2":
                case "Break":
                    double start = parseDouble(s[1]) + offset;
                    double end = Math.Max(start, parseDouble(s[2]) + offset);
                    map.Breaks.Add(new BreakPeriod(start, end));
                    break;

                case "4":
                case "Sprite":
                    if (string.IsNullOrEmpty(map.Metadata.BackgroundFile) && s.Length > 3)
                        map.Metadata.BackgroundFile = cleanFilename(s[3]);
                    break;
            }
        }

        static string cleanFilename(string s) => s.Trim().Trim('"').Replace('\\', '/');

        void colours(string line)
        {
            var (k, v) = keyVal(line);
            if (!k.StartsWith("Combo", StringComparison.Ordinal))
                return;

            string[] c = v.Split(',');
            if (c.Length < 3) return;

            uint r = (uint)Math.Clamp(parseInt(c[0]), 0, 255);
            uint g = (uint)Math.Clamp(parseInt(c[1]), 0, 255);
            uint b = (uint)Math.Clamp(parseInt(c[2]), 0, 255);
            map.ComboColours.Add(0xFF000000 | (r << 16) | (g << 8) | b);
        }

        void timingPoint(string line)
        {
            string[] s = line.Split(',');

            double time = parseDouble(s[0].Trim()) + offset;
            double beatLength = double.Parse(s[1].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture);

            // NaN beat length means "no ticks", comparisons with NaN are false so sv stays 1
            double sv = beatLength < 0 ? 100.0 / -beatLength : 1;

            int meter = 4;
            if (s.Length >= 3 && s[2].Length > 0 && s[2][0] != '0')
                meter = parseInt(s[2]);

            int bank = s.Length >= 4 ? parseInt(s[3]) : defaultBank;
            if (bank == 0) bank = 1;
            int custom = s.Length >= 5 ? parseInt(s[4]) : 0;
            int volume = s.Length >= 6 ? parseInt(s[5]) : defaultVolume;
            bool uninherited = s.Length < 7 || s[6].Length == 0 || s[6][0] == '1';

            bool kiai = false, omitBar = false;
            if (s.Length >= 8)
            {
                int fx = parseInt(s[7]);
                kiai = (fx & 1) != 0;
                omitBar = (fx & 8) != 0;
            }

            if (time != pendingTime)
                flushPoints();
            pendingTime = time;

            var diff = new DifficultyPoint(time, sv, !double.IsNaN(beatLength));

            if (uninherited)
            {
                if (double.IsNaN(beatLength))
                    return;

                pendingTiming = new TimingPoint(time, beatLength, meter, omitBar);
                pendingDiffFromTiming = diff;
                pendingEffect ??= new EffectPoint(time, kiai);
                pendingSample ??= new SamplePoint(time, bank, custom, volume);
            }
            else
            {
                pendingDiff = diff;
                pendingEffect = new EffectPoint(time, kiai);
                pendingSample = new SamplePoint(time, bank, custom, volume);
            }
        }

        void flushPoints()
        {
            var cp = map.ControlPoints;

            if (pendingTiming is TimingPoint t)
                cp.Timing.Add(t);

            if ((pendingDiff ?? pendingDiffFromTiming) is DifficultyPoint d)
            {
                // drop redundant points like lazer does, keeps lookups short
                if (cp.Difficulty.Count == 0 || cp.Difficulty[^1].SliderVelocity != d.SliderVelocity || cp.Difficulty[^1].GenerateTicks != d.GenerateTicks)
                    cp.Difficulty.Add(d);
            }

            if (pendingEffect is EffectPoint e && (cp.Effects.Count == 0 || cp.Effects[^1].Kiai != e.Kiai))
                cp.Effects.Add(e);

            if (pendingSample is SamplePoint sp)
                cp.Samples.Add(sp);

            pendingTiming = null;
            pendingDiff = pendingDiffFromTiming = null;
            pendingEffect = null;
            pendingSample = null;
        }

        void hitObject(string line)
        {
            string[] s = line.Split(',');

            float x = Math.Clamp(parseFloat(s[0]), 0, 512);
            float y = Math.Clamp(parseFloat(s[1]), 0, 512);
            var pos = new Vector2(x, y);

            // pre-lazer maps store whole pixels
            if (version < first_lazer_version)
                pos = new Vector2((int)x, (int)y);
            if (flipY)
                pos.Y = 384 - pos.Y;

            double startTime = parseDouble(s[2]) + offset;
            int type = parseInt(s[3]);

            int comboOffset = (type & 0b0111_0000) >> 4;
            bool combo = (type & 4) != 0;
            var sound = (HitSound)(parseInt(s[4]) & 0xF);

            bool newCombo = lastObject == null || lastObject is Spinner || combo;

            OsuObject obj;

            if ((type & 1) != 0)
            {
                obj = new HitCircle();
                if (s.Length > 5) readBanks(s[5], ref sound, out obj.Samples);
                obj.Samples.Sound = sound;
            }
            else if ((type & 2) != 0)
            {
                int repeats = parseInt(s[6]);
                if (repeats > 9000)
                    throw new FormatException("repeat count is way too high");
                repeats = Math.Max(0, repeats - 1);

                double? length = null;
                if (s.Length > 7)
                {
                    double l = Math.Max(0, parseDouble(s[7]));
                    if (l != 0) length = l;
                }

                var path = new SliderPath(parsePath(s[5], pos), length);

                var nodes = new List<Samples>();
                for (int i = 0; i < repeats + 2; i++)
                    nodes.Add(Samples.From(sound));

                if (s.Length > 8 && s[8].Length > 0)
                {
                    string[] adds = s[8].Split('|');
                    for (int i = 0; i < nodes.Count && i < adds.Length; i++)
                    {
                        var n = nodes[i];
                        n.Sound = (HitSound)(parseInt(adds[i]) & 0xF);
                        nodes[i] = n;
                    }
                }

                if (s.Length > 9 && s[9].Length > 0)
                {
                    string[] sets = s[9].Split('|');
                    for (int i = 0; i < nodes.Count && i < sets.Length; i++)
                    {
                        string[] b = sets[i].Split(':');
                        var n = nodes[i];
                        if (b.Length > 0) n.Bank = (byte)parseInt(b[0]);
                        if (b.Length > 1) n.AddBank = (byte)parseInt(b[1]);
                        nodes[i] = n;
                    }
                }

                // zero length sliders with repeats were an exploit in stable, lazer drops the repeats
                if (Precision.AlmostEquals(path.Distance, 0))
                {
                    repeats = 0;
                    nodes = new List<Samples> { nodes[0], nodes[^1] };
                }

                var slider = new Slider { Path = path, RepeatCount = repeats, NodeSamples = nodes };
                if (s.Length > 10) readBanks(s[10], ref sound, out slider.Samples);
                slider.Samples.Sound = sound;
                obj = slider;
            }
            else if ((type & 8) != 0)
            {
                var spinner = new Spinner { Duration = Math.Max(0, parseDouble(s[5]) + offset - startTime) };
                if (s.Length > 6) readBanks(s[6], ref sound, out spinner.Samples);
                spinner.Samples.Sound = sound;
                obj = spinner;
                pos = new Vector2(256, 192);
            }
            else
            {
                // mania holds and garbage, nothing to do with them here
                return;
            }

            obj.StartTime = startTime;
            obj.Position = pos;
            obj.NewCombo = newCombo;
            obj.ComboOffset = combo ? comboOffset : 0;

            map.HitObjects.Add(obj);
            lastObject = obj;
        }

        static void readBanks(string str, ref HitSound sound, out Samples samples)
        {
            samples = default;
            if (string.IsNullOrEmpty(str)) return;

            string[] s = str.Split(':');
            if (s.Length > 0) samples.Bank = (byte)parseInt(s[0]);
            if (s.Length > 1) samples.AddBank = (byte)parseInt(s[1]);
            if (s.Length > 2) samples.Custom = (ushort)Math.Max(0, parseInt(s[2]));
            if (s.Length > 3) samples.Volume = (byte)Math.Clamp(parseInt(s[3]), 0, 100);
        }

        ControlPoint[] parsePath(string str, Vector2 start)
        {
            string[] parts = str.Split('|');
            var points = new List<Vector2>(parts.Length + 1);
            var segments = new List<(PathType Type, int Start)>();

            foreach (string p in parts)
            {
                if (p.Length == 0) continue;

                if (char.IsLetter(p[0]))
                {
                    segments.Add((pathType(p), points.Count));
                    if (points.Count == 0)
                        points.Add(Vector2.Zero);
                }
                else
                {
                    string[] xy = p.Split(':');
                    float px = parseFloat(xy[0]), py = parseFloat(xy[1]);
                    var v = version >= first_lazer_version ? new Vector2(px, py) : new Vector2((int)px, (int)py);
                    if (flipY)
                        v.Y = 384 - v.Y;
                    points.Add(v - start);
                }
            }

            if (segments.Count == 0)
                segments.Add((PathType.Catmull, 0));
            if (points.Count == 0)
                points.Add(Vector2.Zero);

            var result = new List<ControlPoint>(points.Count);

            for (int i = 0; i < segments.Count; i++)
            {
                int from = segments[i].Start;
                int to = i < segments.Count - 1 ? segments[i + 1].Start : points.Count;
                Vector2? endPoint = i < segments.Count - 1 && to < points.Count ? points[to] : null;
                convertSegment(segments[i].Type, points.GetRange(from, to - from), endPoint, result);
            }

            return result.ToArray();
        }

        static PathType pathType(string s)
        {
            switch (s[0])
            {
                case 'B':
                    if (s.Length > 1 && int.TryParse(s.AsSpan(1), out int deg) && deg > 0)
                        return new PathType(CurveKind.BSpline, deg);
                    return PathType.Bezier;
                case 'L': return PathType.Linear;
                case 'P': return PathType.Perfect;
                default: return PathType.Catmull;
            }
        }

        void convertSegment(PathType type, List<Vector2> pts, Vector2? endPoint, List<ControlPoint> output)
        {
            if (pts.Count == 0)
                return;

            var verts = new ControlPoint[pts.Count];
            for (int i = 0; i < pts.Count; i++)
                verts[i] = new ControlPoint(pts[i]);

            // stable's edge cases for perfect curves
            if (type.Kind == CurveKind.Perfect)
            {
                int extra = endPoint == null ? 0 : 1;

                if (version < first_lazer_version)
                {
                    if (verts.Length + extra != 3)
                        type = PathType.Bezier;
                    else if (pts.Count >= 2 && isLinear(pts[0], pts[1], endPoint ?? pts[2]))
                        type = PathType.Linear;
                }
                else if (verts.Length + extra > 3)
                    type = PathType.Bezier;
            }

            verts[0].Type = type;

            // two identical points in a row start a new implicit segment
            int startIndex = 0, endIndex = 0;

            while (++endIndex < verts.Length)
            {
                if (verts[endIndex].Position != verts[endIndex - 1].Position)
                    continue;

                // legacy catmull never had multiple segments
                if (type.Kind == CurveKind.Catmull && endIndex > 1 && version < first_lazer_version)
                    continue;

                if (endIndex == verts.Length - 1)
                    continue;

                verts[endIndex - 1].Type = type;
                for (int i = startIndex; i < endIndex; i++)
                    output.Add(verts[i]);

                startIndex = endIndex + 1;
            }

            for (int i = startIndex; i < endIndex; i++)
                output.Add(verts[i]);
        }

        static bool isLinear(Vector2 p0, Vector2 p1, Vector2 p2)
            => Precision.AlmostEquals(0, (p1.Y - p0.Y) * (p2.X - p0.X) - (p1.X - p0.X) * (p2.Y - p0.Y));

        void postProcessBreaks()
        {
            int current = 0;

            foreach (var h in map.HitObjects)
            {
                bool force = false;
                while (current < map.Breaks.Count && map.Breaks[current].End < h.StartTime)
                {
                    force = true;
                    current++;
                }

                h.NewCombo |= force;
            }
        }

        static int bankFromName(string s) => s switch
        {
            "Soft" => 2,
            "Drum" => 3,
            _ => 1,
        };

        static int parseInt(string s)
        {
            double d = double.Parse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture);
            if (d > int.MaxValue || d < int.MinValue) throw new OverflowException();
            return (int)d;
        }

        static float parseFloat(string s)
        {
            float f = float.Parse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture);
            if (Math.Abs(f) > max_coord || float.IsNaN(f)) throw new OverflowException();
            return f;
        }

        static double parseDouble(string s)
        {
            double d = double.Parse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture);
            if (Math.Abs(d) > int.MaxValue || double.IsNaN(d)) throw new OverflowException();
            return d;
        }
    }
}
