// Parts of this file are adapted from osu! (https://github.com/ppy/osu), MIT licence, (c) ppy Pty Ltd.

using System;

namespace Osu5.Util
{
    public readonly record struct DiffRange(double Min, double Mid, double Max);

    public static class Difficulty
    {
        public static double Range(double difficulty) => (difficulty - 5) / 5;

        public static double Range(double difficulty, double min, double mid, double max)
        {
            if (difficulty > 5)
                return mid + (max - mid) * Range(difficulty);
            if (difficulty < 5)
                return mid + (mid - min) * Range(difficulty);
            return mid;
        }

        public static double Range(double difficulty, DiffRange r) => Range(difficulty, r.Min, r.Mid, r.Max);

        public static int RangeInt(double difficulty, DiffRange r) => (int)Range(difficulty, r);

        // 1.00041 is stable's gamefield rounding fudge, kept so circle radii match.
        public static float ScaleFromCircleSize(float cs)
            => (float)(1.0f - 0.7f * Range(cs)) / 2 * 1.00041f;
    }

    public static class Precision
    {
        public const float FloatEpsilon = 1e-3f;
        public const double DoubleEpsilon = 1e-7;

        public static bool AlmostEquals(float a, float b, float eps = FloatEpsilon) => Math.Abs(a - b) <= eps;
        public static bool AlmostEquals(double a, double b, double eps = DoubleEpsilon) => Math.Abs(a - b) <= eps;
    }
}
