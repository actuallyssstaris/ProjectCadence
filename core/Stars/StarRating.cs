// Star rating as osu!'s OsuDifficultyCalculator works it out (https://github.com/ppy/osu,
// MIT licence), difficulty calculator version 20260706. No Flashlight: there's no
// FL mod here.

using System;
using System.Collections.Generic;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Play;
using Osu5.Scoring;

namespace Osu5.Stars
{
    public readonly record struct StarResult(double Stars, double Aim, double Speed, double Reading);

    public static class StarRating
    {
        public const int Version = 20260706;

        const double performance_base_multiplier = 1.12;
        const double performance_norm_exponent = 1.1;

        // map: decoded with the same mods, so HR/EZ are already in its difficulty
        public static StarResult Calculate(Beatmap map, Mods mods)
        {
            var objs = map.HitObjects;
            if (objs.Count == 0)
                return default;

            double rate = ModRules.Rate(mods);
            double great = new HitWindows(map.Difficulty.OverallDifficulty).Great;

            var list = new List<DiffObject>(objs.Count);
            for (int i = 1; i < objs.Count; i++)
                list.Add(new DiffObject(objs[i], objs[i - 1], rate, objs[i] is Spinner ? 0 : great, list, list.Count));

            var aim = new Aim(true);
            var speed = new Speed();
            var reading = new Reading((mods & Mods.Hidden) != 0);

            foreach (var o in list)
            {
                aim.Process(o);
                speed.Process(o);
                reading.Process(o);
            }

            double aimRating = DiffUtils.Pow(aim.DifficultyValue(), 0.63) * 0.02275;
            double speedRating = Math.Sqrt(speed.DifficultyValue()) * 0.0675;
            double readingRating = Math.Sqrt(reading.DifficultyValue()) * 0.0675;

            double baseAim = 4.0 * DiffUtils.Pow(aimRating, 3);
            double baseSpeed = HarmonicSkill.DifficultyToPerformance(speedRating);
            double baseReading = HarmonicSkill.DifficultyToPerformance(readingRating);

            double basePerformance = DiffUtils.Norm(performance_norm_exponent, baseAim, baseSpeed, baseReading);
            double stars = Math.Cbrt(basePerformance * performance_base_multiplier);

            return new StarResult(stars, aimRating, speedRating, readingRating);
        }
    }
}
