// Star rating and pp for taiko, catch and mania.
//
// Catch's movement skill and all three pp formulas follow osu!'s calculators
// (https://github.com/ppy/osu), MIT licence; catch leaves out the edge-dash bonus.
// Taiko's stars are a small strain model of our own (stamina, colour, rhythm)
// tuned to land near the real numbers: good for sorting maps, not exact.

using System;
using System.Collections.Generic;
using Osu5.Beatmaps;
using Osu5.Play;

namespace Osu5.Stars
{
    public sealed class OtherAttributes
    {
        public int Mode;
        public double Stars;
        public int MaxCombo;       // catch: fruit + droplets
        public int HitCount;       // taiko / mania: notes
        public double GreatWindow; // taiko, ms at the map's own speed
        public double ApproachRate; // catch, after rate changes
    }

    public struct OtherStats
    {
        public double Accuracy;
        public int MaxCombo;
        public int Perfect, Great, Good, Ok, Meh, Miss;
        public int Droplets, DropletMisses, TinyHits, TinyMisses;
    }

    public static class OtherStars
    {
        public static OtherAttributes? Calculate(Beatmap map, Mods mods)
        {
            double rate = ModRules.Rate(mods);
            switch (map.Mode)
            {
                case 1:
                {
                    var tp = new TaikoPlayer(map, mods);
                    var hits = tp.Hits();
                    return new OtherAttributes { Mode = 1, Stars = taiko(hits, rate), HitCount = hits.Count, GreatWindow = tp.GreatWindow };
                }
                case 2:
                {
                    var cp = new CatchPlayer(map, mods);
                    var objs = cp.Palpable();
                    double preempt = Util.Difficulty.Range(map.Difficulty.ApproachRate, 1800, 1200, 450) / rate;
                    double ar = preempt > 1200 ? (1800 - preempt) / 120 : (1200 - preempt) / 150 + 5;
                    return new OtherAttributes { Mode = 2, Stars = catchStars(objs, cp.HalfWidth, map.Difficulty.CircleSize, rate), MaxCombo = objs.Count, ApproachRate = ar };
                }
                case 3:
                    return new OtherAttributes { Mode = 3, Stars = ManiaStars.Calculate(map, mods), HitCount = map.HitObjects.Count };
            }
            return null;
        }

        /* ---------- taiko ---------- */

        static double taiko(List<(double Time, bool Kat)> hits, double rate)
        {
            if (hits.Count < 2) return 0;
            const double section = 400, decay = 0.4;
            var peaks = new List<double>();
            double strain = 0, peak = 0, sectionEnd = Math.Ceiling(hits[0].Time / rate / section) * section;
            double prevDelta = 0;
            int run = 1, prevRun = 0;

            for (int i = 1; i < hits.Count; i++)
            {
                double t = hits[i].Time / rate;
                double delta = Math.Max(25, t - hits[i - 1].Time / rate);
                double prevT = hits[i - 1].Time / rate;

                while (t > sectionEnd)
                {
                    peaks.Add(peak);
                    peak = strain * Math.Pow(decay, (sectionEnd - prevT) / 1000);
                    sectionEnd += section;
                }

                // stamina: alternating hands, so the same hand comes back every other note
                double sameHand = i >= 2 ? Math.Max(50, t - hits[i - 2].Time / rate) : delta * 2;
                double stamina = 0.25 + 150 / sameHand;

                // colour: a switch after a run of a different length is the hard part
                double colour = 0;
                if (hits[i].Kat != hits[i - 1].Kat)
                {
                    colour = run != prevRun ? 1 : 0.5;
                    prevRun = run;
                    run = 1;
                }
                else run++;

                // rhythm: a change of spacing costs, more the further from a clean ratio
                double rhythm = 0;
                if (prevDelta > 0)
                {
                    double ratio = Math.Abs(Math.Log2(delta / prevDelta));
                    double offGrid = Math.Abs(ratio - Math.Round(ratio));
                    rhythm = Math.Min(1, ratio * 0.5 + offGrid * 2);
                }
                prevDelta = delta;

                strain = strain * Math.Pow(decay, delta / 1000) + stamina + 0.6 * colour + 0.4 * rhythm;
                peak = Math.Max(peak, strain);
            }
            peaks.Add(peak);
            return 0.31 * Math.Pow(weighted(peaks, 0.9), 0.55);
        }

        /* ---------- catch ---------- */

        const double norm_radius = 41, position_error = 16, direction_change_bonus = 21;

        static double catchStars(List<(double Time, float X)> objs, float halfWidth, float cs, double rate)
        {
            if (objs.Count < 2) return 0;
            double half = halfWidth * (1 - Math.Max(0, cs - 5.5) * 0.0625);
            double scaling = norm_radius / half;
            double sectionLength = 750 * rate;   // lazer measures sections in map time

            var peaks = new List<double>();
            double strain = 0, peak = 0;
            double sectionEnd = Math.Ceiling(objs[1].Time / sectionLength) * sectionLength;
            double? lastPlayer = null;
            double lastMoved = 0, lastStrainTime = 0;

            for (int i = 1; i < objs.Count; i++)
            {
                double pos = objs[i].X * scaling, lastPos = objs[i - 1].X * scaling;
                double delta = (objs[i].Time - objs[i - 1].Time) / rate;
                double strainTime = Math.Max(40, delta);

                while (objs[i].Time > sectionEnd)
                {
                    peaks.Add(peak);
                    peak = strain * Math.Pow(0.2, (sectionEnd - objs[i - 1].Time) / rate / 1000);
                    sectionEnd += sectionLength;
                }

                double wTime = strainTime + 13 + 3 / rate;
                double player = Math.Clamp(lastPlayer ?? lastPos, pos - (norm_radius - position_error), pos + (norm_radius - position_error));
                double moved = player - (lastPlayer ?? lastPos);
                double add = Math.Pow(Math.Abs(moved), 1.3) / 510;
                double sqrtStrain = Math.Sqrt(wTime);

                if (Math.Abs(moved) > 0.1)
                {
                    if (Math.Abs(lastMoved) > 0.1 && Math.Sign(moved) != Math.Sign(lastMoved))
                    {
                        double bonus = Math.Min(50, Math.Abs(moved)) / 50;
                        double antiflow = Math.Max(Math.Min(70, Math.Abs(lastMoved)) / 70, 0.38);
                        add += direction_change_bonus / Math.Sqrt(lastStrainTime + 16) * bonus * antiflow
                               * Math.Max(1 - Math.Pow(wTime / 1000, 3), 0);
                    }
                    add += 12.5 * Math.Min(Math.Abs(moved), norm_radius * 2) / (norm_radius * 6) / sqrtStrain;
                }

                lastPlayer = player;
                lastMoved = moved;
                lastStrainTime = strainTime;

                strain = strain * Math.Pow(0.2, delta / 1000) + add / wTime * 900;
                peak = Math.Max(peak, strain);
            }
            peaks.Add(peak);
            return Math.Sqrt(weighted(peaks, 0.94)) * 0.153;
        }

        static double weighted(List<double> peaks, double decay)
        {
            peaks.Sort((a, b) => b.CompareTo(a));
            double total = 0, w = 1;
            foreach (double p in peaks)
            {
                if (p <= 0) break;
                total += p * w;
                w *= decay;
            }
            return total;
        }

        /* ---------- pp ---------- */

        public static double Pp(OtherAttributes a, Mods mods, OtherStats s)
        {
            switch (a.Mode)
            {
                case 1: return taikoPp(a, mods, s);
                case 2: return catchPp(a, mods, s);
                case 3: return maniaPp(a, mods, s);
            }
            return 0;
        }

        static bool has(Mods m, Mods f) => (m & f) != 0;

        static double taikoPp(OtherAttributes a, Mods mods, OtherStats s)
        {
            int total = s.Great + s.Ok + s.Miss;
            if (total == 0) return 0;
            double acc = Math.Clamp(s.Accuracy, 0, 1);

            double diff = Math.Pow(5 * Math.Max(1, a.Stars / 0.115) - 4, 2.25) / 1150;
            double lengthBonus = 1 + 0.1 * Math.Min(1, total / 1500.0);
            diff *= lengthBonus * Math.Pow(0.986, s.Miss);
            if (has(mods, Mods.Easy)) diff *= 0.985;
            if (has(mods, Mods.Hidden)) diff *= 1.025;
            if (has(mods, Mods.HardRock)) diff *= 1.05;
            if (has(mods, Mods.Flashlight)) diff *= 1.05 * lengthBonus;
            diff *= acc * acc;

            double window = a.GreatWindow / ModRules.Rate(mods);
            double accValue = 0;
            if (window > 0)
            {
                accValue = Math.Pow(60 / window, 1.1) * Math.Pow(acc, 8) * Math.Pow(a.Stars, 0.4) * 27;
                accValue *= Math.Min(1.15, Math.Pow(total / 1500.0, 0.3));
                if (has(mods, Mods.Hidden) && has(mods, Mods.Flashlight)) accValue *= 1.1;
            }

            double mult = 1.13;
            if (has(mods, Mods.Hidden)) mult *= 1.075;
            if (has(mods, Mods.Easy)) mult *= 0.975;
            return Math.Pow(Math.Pow(diff, 1.1) + Math.Pow(accValue, 1.1), 1 / 1.1) * mult;
        }

        static double catchPp(OtherAttributes a, Mods mods, OtherStats s)
        {
            double value = Math.Pow(5 * Math.Max(1, a.Stars / 0.0049) - 4, 2) / 100000;
            int combo = s.Great + s.Miss + s.Droplets + s.DropletMisses;
            if (combo == 0) return 0;

            double lengthBonus = 0.95 + 0.3 * Math.Min(1, combo / 2500.0) + (combo > 2500 ? Math.Log10(combo / 2500.0) * 0.475 : 0);
            value *= lengthBonus;
            value *= Math.Pow(0.97, s.Miss + s.DropletMisses);
            if (a.MaxCombo > 0)
                value *= Math.Min(Math.Pow(s.MaxCombo, 0.8) / Math.Pow(a.MaxCombo, 0.8), 1);

            double ar = a.ApproachRate, arFactor = 1;
            if (ar > 9) arFactor += 0.1 * (ar - 9);
            if (ar > 10) arFactor += 0.1 * (ar - 10);
            else if (ar < 8) arFactor += 0.025 * (8 - ar);
            value *= arFactor;

            if (has(mods, Mods.Hidden))
                value *= ar <= 10 ? 1.05 + 0.075 * (10 - ar) : 1.01 + 0.04 * (11 - Math.Min(11, ar));
            if (has(mods, Mods.Flashlight)) value *= 1.35 * lengthBonus;
            value *= Math.Pow(Math.Clamp(s.Accuracy, 0, 1), 5.5);
            if (has(mods, Mods.NoFail)) value *= 0.9;
            return value;
        }

        static double maniaPp(OtherAttributes a, Mods mods, OtherStats s)
        {
            int total = s.Perfect + s.Great + s.Good + s.Ok + s.Meh + s.Miss;
            if (total == 0) return 0;
            // mania's pp weighs Perfects above Greats, unlike the score accuracy
            double acc = (s.Perfect * 320.0 + s.Great * 300 + s.Good * 200 + s.Ok * 100 + s.Meh * 50) / (total * 320.0);
            double diff = Math.Pow(Math.Max(a.Stars - 0.15, 0.05), 2.2) * Math.Max(0, 5 * acc - 4) * (1 + 0.1 * Math.Min(1, total / 1500.0));
            double mult = 8;
            if (has(mods, Mods.NoFail)) mult *= 0.75;
            if (has(mods, Mods.Easy)) mult *= 0.5;
            return diff * mult;
        }
    }
}
