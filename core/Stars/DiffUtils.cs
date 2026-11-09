// From osu!'s DiffUtils (https://github.com/ppy/osu), MIT licence. Only what the
// osu! star rating uses.

using System;
using System.Collections.Generic;

namespace Osu5.Stars
{
    static class DiffUtils
    {
        public static double BPMToMilliseconds(double bpm, int delimiter = 4) => 60000.0 / delimiter / bpm;

        public static double MillisecondsToBPM(double ms, int delimiter = 4) => 60000.0 / (ms * delimiter);

        public static double Logistic(double x, double midpointOffset, double multiplier, double maxValue = 1)
            => maxValue / (1 + Math.Exp(multiplier * (midpointOffset - x)));

        public static double Logistic(double exponent, double maxValue = 1) => maxValue / (1 + Math.Exp(exponent));

        public static double Norm(double p, params double[] values)
        {
            double sum = 0;
            foreach (double x in values)
                sum += Math.Pow(x, p);
            return Math.Pow(sum, 1.0 / p);
        }

        public static double SmoothstepBellCurve(double x)
        {
            x = 0.5 - Math.Abs(x - 0.5);
            x = Math.Clamp(x * 2.0, 0.0, 1.0);
            return x * x * (3.0 - 2.0 * x);
        }

        public static double Smoothstep(double x, double start, double end)
        {
            x = Math.Clamp((x - start) / (end - start), 0.0, 1.0);
            return x * x * (3.0 - 2.0 * x);
        }

        public static double Smootherstep(double x, double start, double end)
        {
            x = Math.Clamp((x - start) / (end - start), 0.0, 1.0);
            return x * x * x * (x * (6.0 * x - 15.0) + 10.0);
        }

        public static double ReverseLerp(double x, double start, double end) => Math.Clamp((x - start) / (end - start), 0.0, 1.0);

        // osu-framework's Interpolation.Lerp; upstream also uses .NET's double.Lerp in
        // one spot, which rounds differently, so that one stays double.Lerp
        public static double Lerp(double a, double b, double t) => a + (b - a) * t;

        public static double Pow(double x, double exponent) => Math.Pow(x, exponent);

        public static double Pow(double x, int exponent) => exponent switch
        {
            0 => 1,
            1 => x,
            2 => x * x,
            3 => x * x * x,
            4 => x * x * x * x,
            5 => x * x * x * x * x,
            _ => Math.Pow(x, exponent),
        };

        public static double DegreesToRadians(double d) => double.DegreesToRadians(d);

        public const double SQRT2 = 1.4142135623730950;

        public static double Erf(double x)
        {
            if (x == 0)
                return 0;
            if (double.IsPositiveInfinity(x))
                return 1;
            if (double.IsNegativeInfinity(x))
                return -1;
            if (double.IsNaN(x))
                return double.NaN;

            double t = 1.0 / (1.0 + 0.3275911 * Math.Abs(x));
            double tau = t * (0.254829592
                              + t * (-0.284496736
                                     + t * (1.421413741
                                            + t * (-1.453152027
                                                   + t * 1.061405429))));
            double erf = 1.0 - tau * Math.Exp(-x * x);
            return x >= 0 ? erf : -erf;
        }

        public static double ErfInv(double x)
        {
            if (x <= -1)
                return double.NegativeInfinity;
            if (x >= 1)
                return double.PositiveInfinity;
            if (x == 0)
                return 0;

            const double a = 0.147;
            double sgn = Math.Sign(x);
            x = Math.Abs(x);

            double ln = Math.Log(1 - x * x);
            double t1 = 2 / (Math.PI * a) + ln / 2;
            double t2 = ln / a;
            double baseApprox = Math.Sqrt(t1 * t1 - t2) - t1;

            double c = x >= 0.85 ? Pow((x - 0.85) / 0.293, 8) : 0;
            return sgn * (Math.Sqrt(baseApprox) + c);
        }

        // osu-framework's AddInPlace: keep a sorted list sorted
        public static void AddInPlace<T>(this List<T> list, T item)
        {
            int i = list.BinarySearch(item);
            list.Insert(i < 0 ? ~i : i, item);
        }
    }
}
