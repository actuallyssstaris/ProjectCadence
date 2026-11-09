// Mod values follow osu!'s osu ruleset mods (https://github.com/ppy/osu), MIT licence.

using System;
using Osu5.Beatmaps;

namespace Osu5.Play
{
    // same bits as OSU5_MOD_* in host/include/osu5.h
    [Flags]
    public enum Mods
    {
        None = 0,
        Auto = 1,
        NoFail = 2,
        Hidden = 4,
        HardRock = 8,
        Easy = 16,
        DoubleTime = 32,
        HalfTime = 64,
    }

    public static class ModRules
    {
        public static void ApplyToDifficulty(BeatmapDifficulty d, Mods mods)
        {
            if ((mods & Mods.Easy) != 0)
            {
                d.CircleSize *= 0.5f;
                d.ApproachRate *= 0.5f;
                d.OverallDifficulty *= 0.5f;
                d.DrainRate *= 0.5f;
            }

            if ((mods & Mods.HardRock) != 0)
            {
                d.CircleSize = Math.Min(d.CircleSize * 1.3f, 10);
                d.ApproachRate = Math.Min(d.ApproachRate * 1.4f, 10);
                d.OverallDifficulty = Math.Min(d.OverallDifficulty * 1.4f, 10);
                d.DrainRate = Math.Min(d.DrainRate * 1.4f, 10);
            }
        }

        public static double ScoreMultiplier(Mods mods)
        {
            double m = 1;
            if ((mods & Mods.NoFail) != 0) m *= 0.5;
            if ((mods & Mods.Easy) != 0) m *= 0.5;
            if ((mods & Mods.HalfTime) != 0) m *= 0.3;
            if ((mods & Mods.Hidden) != 0) m *= 1.06;
            if ((mods & Mods.HardRock) != 0) m *= 1.06;
            if ((mods & Mods.DoubleTime) != 0) m *= 1.12;
            return m;
        }

        public static double Rate(Mods mods)
            => (mods & Mods.DoubleTime) != 0 ? 1.5 : (mods & Mods.HalfTime) != 0 ? 0.75 : 1;
    }
}
