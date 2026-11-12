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
        SuddenDeath = 128,
        Perfect = 256,
        Flashlight = 512,
        Nightcore = 1024,  // DoubleTime with the pitch going up; the host sets DoubleTime with it
        Daycore = 2048,    // HalfTime, pitch down; same
        Relax = 4096,
        Autopilot = 8192,
        SpunOut = 16384,
        Mirror = 32768,
    }

    public static class ModRules
    {
        // Difficulty Adjust (negative: the map's own) and a custom speed for DT / HT
        public static float CsOverride = -1, ArOverride = -1, OdOverride = -1, HpOverride = -1;
        public static double CustomRate;

        public static void ApplyToDifficulty(BeatmapDifficulty d, Mods mods)
        {
            applyEasyHard(d, mods);
            if (CsOverride >= 0) d.CircleSize = CsOverride;
            if (ArOverride >= 0) d.ApproachRate = ArOverride;
            if (OdOverride >= 0) d.OverallDifficulty = OdOverride;
            if (HpOverride >= 0) d.DrainRate = HpOverride;
        }

        static void applyEasyHard(BeatmapDifficulty d, Mods mods)
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
            if ((mods & Mods.Flashlight) != 0) m *= 1.12;
            if ((mods & (Mods.Relax | Mods.Autopilot)) != 0) m *= 0.1;
            if ((mods & Mods.SpunOut) != 0) m *= 0.9;
            return m;
        }

        public static double Rate(Mods mods)
        {
            if ((mods & (Mods.DoubleTime | Mods.HalfTime)) == 0)
                return 1;
            if (CustomRate > 0)
                return CustomRate;
            return (mods & Mods.DoubleTime) != 0 ? 1.5 : 0.75;
        }
    }
}
