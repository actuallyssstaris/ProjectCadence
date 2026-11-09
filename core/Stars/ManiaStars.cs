// osu!mania star rating, the strain model lazer's mania calculator used before its
// 2024 rework (https://github.com/ppy/osu), MIT licence. Close to the old numbers,
// not the current website ones.

using System;
using System.Collections.Generic;
using Osu5.Beatmaps;
using Osu5.Play;

namespace Osu5.Stars
{
    public static class ManiaStars
    {
        const double individual_decay = 0.125, overall_decay = 0.30;
        const double section_length = 400, decay_weight = 0.9, star_scaling = 0.018;

        public static double Calculate(Beatmap map, Mods mods)
        {
            int columns = Math.Clamp((int)Math.Round(map.Difficulty.CircleSize), 1, 10);
            double rate = ModRules.Rate(mods);
            var objs = map.HitObjects;
            if (objs.Count < 2)
                return 0;

            var holdEnds = new double[columns];
            var individual = new double[columns];
            double overall = 1, prevTime = objs[0].StartTime / rate;
            double current = 0, sectionEnd = Math.Ceiling(prevTime / section_length) * section_length, peak = 0;
            var peaks = new List<double>();

            for (int i = 1; i < objs.Count; i++)
            {
                var h = objs[i];
                double start = h.StartTime / rate, end = h.EndTime / rate, delta = start - prevTime;
                int col = Math.Clamp((int)(h.Position.X * columns / 512), 0, columns - 1);

                while (start > sectionEnd)
                {
                    peaks.Add(peak);
                    peak = current; // strain carries over unchanged, it doesn't decay here
                    sectionEnd += section_length;
                }

                double holdFactor = 1, holdAddition = 0;
                for (int c = 0; c < columns; c++)
                {
                    if (start < holdEnds[c] && end > holdEnds[c]) holdAddition = 1;
                    if (end == holdEnds[c]) holdAddition = 0;
                    if (holdEnds[c] > end) holdFactor = 1.25;
                }
                for (int c = 0; c < columns; c++)
                    individual[c] *= Math.Pow(individual_decay, delta / 1000);
                individual[col] += 2 * holdFactor;
                overall = overall * Math.Pow(overall_decay, delta / 1000) + (1 + holdAddition) * holdFactor;
                holdEnds[col] = end;

                current = individual[col] + overall;
                peak = Math.Max(peak, current);
                prevTime = start;
            }
            peaks.Add(peak);

            peaks.Sort((a, b) => b.CompareTo(a));
            double total = 0, weight = 1;
            foreach (double p in peaks)
            {
                if (p <= 0) break;
                total += p * weight;
                weight *= decay_weight;
            }
            return total * star_scaling;
        }
    }
}
