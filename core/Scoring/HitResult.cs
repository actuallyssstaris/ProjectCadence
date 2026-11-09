// Result types and hit windows from osu! (https://github.com/ppy/osu), MIT licence.

using System;
using Osu5.Util;

namespace Osu5.Scoring
{
    public enum HitResult : byte
    {
        None,
        Miss,
        Meh,
        Ok,
        Great,
        LargeTickMiss,
        LargeTickHit,
        SliderTailHit,
        IgnoreMiss,
        IgnoreHit,
        SmallBonus,
        LargeBonus,
    }

    public static class HitResults
    {
        public static int BaseScore(HitResult r) => r switch
        {
            HitResult.LargeTickHit => 30,
            HitResult.SliderTailHit => 150,
            HitResult.Meh => 50,
            HitResult.Ok => 100,
            HitResult.Great => 300,
            HitResult.SmallBonus => 10,
            HitResult.LargeBonus => 50,
            _ => 0,
        };

        public static bool IsHit(HitResult r) => r is not (HitResult.None or HitResult.Miss or HitResult.LargeTickMiss or HitResult.IgnoreMiss);

        public static bool IsBasic(HitResult r) => r is HitResult.Miss or HitResult.Meh or HitResult.Ok or HitResult.Great;

        public static bool IsBonus(HitResult r) => r is HitResult.SmallBonus or HitResult.LargeBonus;

        public static bool AffectsCombo(HitResult r) => r is HitResult.Miss or HitResult.Meh or HitResult.Ok or HitResult.Great
            or HitResult.LargeTickHit or HitResult.LargeTickMiss or HitResult.SliderTailHit;

        public static bool BreaksCombo(HitResult r) => r is HitResult.Miss or HitResult.LargeTickMiss;

        public static bool AffectsAccuracy(HitResult r) => r is HitResult.Miss or HitResult.Meh or HitResult.Ok or HitResult.Great
            or HitResult.LargeTickHit or HitResult.LargeTickMiss or HitResult.SliderTailHit;

        // from lazer's Judgement.HealthIncreaseFor, max increase is 0.05
        public static double Health(HitResult r) => r switch
        {
            HitResult.Miss => -0.10,
            HitResult.LargeTickMiss => -0.05,
            HitResult.Meh => 0.0025,
            HitResult.Ok => 0.025,
            HitResult.Great => 0.05,
            HitResult.LargeTickHit => 0.05,
            HitResult.SliderTailHit => 0.02,
            HitResult.SmallBonus => 0.005,
            HitResult.LargeBonus => 0.01,
            _ => 0,
        };
    }

    public sealed class HitWindows
    {
        public static readonly DiffRange GreatRange = new DiffRange(80, 50, 20);
        public static readonly DiffRange OkRange = new DiffRange(140, 100, 60);
        public static readonly DiffRange MehRange = new DiffRange(200, 150, 100);
        public const double MissWindow = 400;

        public double Great { get; private set; }
        public double Ok { get; private set; }
        public double Meh { get; private set; }

        public HitWindows(double od)
        {
            Great = Math.Floor(Difficulty.Range(od, GreatRange)) - 0.5;
            Ok = Math.Floor(Difficulty.Range(od, OkRange)) - 0.5;
            Meh = Math.Floor(Difficulty.Range(od, MehRange)) - 0.5;
        }

        public HitResult ResultFor(double offset)
        {
            offset = Math.Abs(offset);
            if (offset <= Great) return HitResult.Great;
            if (offset <= Ok) return HitResult.Ok;
            if (offset <= Meh) return HitResult.Meh;
            if (offset <= MissWindow) return HitResult.Miss;
            return HitResult.None;
        }

        public bool CanBeHit(double offset) => offset <= Meh;
    }
}
