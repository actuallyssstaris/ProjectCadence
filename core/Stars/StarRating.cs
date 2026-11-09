// Star rating as osu!'s OsuDifficultyCalculator works it out (https://github.com/ppy/osu,
// MIT licence), difficulty calculator version 20260706. No Flashlight: there's no
// FL mod here.

using System;
using System.Collections.Generic;
using System.Linq;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Play;
using Osu5.Scoring;

namespace Osu5.Stars
{
    // what the pp calculation needs from the map, as OsuDifficultyAttributes
    public sealed class StarResult
    {
        public double Stars, Aim, Speed, Reading;
        public double SliderFactor = 1;
        public double AimDifficultSliderCount, AimDifficultStrainCount;
        public double SpeedNoteCount, SpeedDifficultStrainCount;
        public double ReadingDifficultNoteCount;
        public int MaxCombo, HitCircleCount, SliderCount, SpinnerCount;
        public double ApproachRate, OverallDifficulty, DrainRate;
    }

    public static class StarRating
    {
        public const int Version = 20260706;

        public const double PerformanceBaseMultiplier = 1.12;
        public const double PerformanceNormExponent = 1.1;

        // map: decoded with the same mods, so HR/EZ are already in its difficulty
        public static StarResult Calculate(Beatmap map, Mods mods)
        {
            var objs = map.HitObjects;
            var r = new StarResult
            {
                ApproachRate = map.Difficulty.ApproachRate,
                OverallDifficulty = map.Difficulty.OverallDifficulty,
                DrainRate = map.Difficulty.DrainRate,
            };
            if (objs.Count == 0)
                return r;

            double rate = ModRules.Rate(mods);
            double great = new HitWindows(map.Difficulty.OverallDifficulty).Great;

            var list = new List<DiffObject>(objs.Count);
            for (int i = 1; i < objs.Count; i++)
                list.Add(new DiffObject(objs[i], objs[i - 1], rate, objs[i] is Spinner ? 0 : great, list, list.Count));

            var aim = new Aim(true);
            var aimNoSliders = new Aim(false);
            var speed = new Speed();
            var reading = new Reading((mods & Mods.Hidden) != 0);

            foreach (var o in list)
            {
                aim.Process(o);
                aimNoSliders.Process(o);
                speed.Process(o);
                reading.Process(o);
            }

            double aimValue = aim.DifficultyValue();
            double aimNoSlidersValue = aimNoSliders.DifficultyValue();
            double speedValue = speed.DifficultyValue();
            double readingValue = reading.DifficultyValue();

            r.Aim = aimRating(aimValue);
            r.Speed = Math.Sqrt(speedValue) * 0.0675;
            r.Reading = Math.Sqrt(readingValue) * 0.0675;
            r.SliderFactor = aimValue > 0 ? aimRating(aimNoSlidersValue) / r.Aim : 1;

            r.AimDifficultStrainCount = aim.CountTopWeightedStrains(aimValue);
            r.AimDifficultSliderCount = aim.GetDifficultSliders();
            r.SpeedDifficultStrainCount = speed.CountTopWeightedObjectDifficulties(speedValue);
            r.SpeedNoteCount = speed.RelevantObjectCount();
            r.ReadingDifficultNoteCount = reading.CountTopWeightedObjectDifficulties(readingValue);

            r.HitCircleCount = objs.Count(h => h is HitCircle);
            r.SliderCount = objs.Count(h => h is Slider);
            r.SpinnerCount = objs.Count(h => h is Spinner);
            r.MaxCombo = r.HitCircleCount + r.SpinnerCount + objs.OfType<Slider>().Sum(s => s.Nested.Count);

            double baseAim = 4.0 * DiffUtils.Pow(r.Aim, 3);
            double baseSpeed = HarmonicSkill.DifficultyToPerformance(r.Speed);
            double baseReading = HarmonicSkill.DifficultyToPerformance(r.Reading);

            double basePerformance = DiffUtils.Norm(PerformanceNormExponent, baseAim, baseSpeed, baseReading);
            r.Stars = Math.Cbrt(basePerformance * PerformanceBaseMultiplier);
            return r;
        }

        static double aimRating(double v) => DiffUtils.Pow(v, 0.63) * 0.02275;
    }
}
