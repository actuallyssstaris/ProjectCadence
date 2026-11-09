// Adapted from osu-framework's PathApproximator / CircularArcProperties
// (https://github.com/ppy/osu-framework), MIT licence, (c) ppy Pty Ltd.
// Only the parts sliders need. Output has to match lazer closely or slider
// lengths drift, so the maths is kept as-is.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Util;

namespace Osu5.Paths
{
    public static class PathApproximator
    {
        const float bezier_tolerance = 0.25f;
        const int catmull_detail = 50;
        const float circular_arc_tolerance = 0.1f;

        public static List<Vector2> Bezier(ReadOnlySpan<Vector2> pts) => BSpline(pts, Math.Max(1, pts.Length - 1));

        public static List<Vector2> BSpline(ReadOnlySpan<Vector2> pts, int degree)
        {
            if (degree < 1) degree = 1;

            if (pts.Length < 2)
                return pts.Length == 0 ? new List<Vector2>() : new List<Vector2> { pts[0] };

            degree = Math.Min(degree, pts.Length - 1);

            var output = new List<Vector2>();
            int pointCount = pts.Length - 1;

            Stack<Vector2[]> toFlatten = splitToBeziers(pts, ref degree);
            var freeBuffers = new Stack<Vector2[]>();

            var buf1 = new Vector2[degree + 1];
            var buf2 = new Vector2[degree * 2 + 1];
            Vector2[] left = buf2;

            // iterative DFS instead of recursion, deep sliders blow the stack otherwise
            while (toFlatten.Count > 0)
            {
                Vector2[] parent = toFlatten.Pop();

                if (flatEnough(parent))
                {
                    approximate(parent, output, buf1, buf2, degree + 1);
                    freeBuffers.Push(parent);
                    continue;
                }

                Vector2[] right = freeBuffers.Count > 0 ? freeBuffers.Pop() : new Vector2[degree + 1];
                subdivide(parent, left, right, buf1, degree + 1);

                for (int i = 0; i < degree + 1; ++i)
                    parent[i] = left[i];

                toFlatten.Push(right);
                toFlatten.Push(parent);
            }

            output.Add(pts[pointCount]);
            return output;
        }

        public static List<Vector2> Catmull(ReadOnlySpan<Vector2> pts)
        {
            var result = new List<Vector2>(Math.Max(0, (pts.Length - 1) * catmull_detail * 2));

            for (int i = 0; i < pts.Length - 1; i++)
            {
                var v1 = i > 0 ? pts[i - 1] : pts[i];
                var v2 = pts[i];
                var v3 = i < pts.Length - 1 ? pts[i + 1] : v2 + v2 - v1;
                var v4 = i < pts.Length - 2 ? pts[i + 2] : v3 + v3 - v2;

                for (int c = 0; c < catmull_detail; c++)
                {
                    result.Add(catmullPoint(v1, v2, v3, v4, (float)c / catmull_detail));
                    result.Add(catmullPoint(v1, v2, v3, v4, (float)(c + 1) / catmull_detail));
                }
            }

            return result;
        }

        public static List<Vector2> CircularArc(ReadOnlySpan<Vector2> pts)
        {
            var arc = new CircularArc(pts);
            if (!arc.IsValid)
                return Bezier(pts);

            int count = ArcPointCount(arc);
            var output = new List<Vector2>(count);

            for (int i = 0; i < count; ++i)
            {
                double fract = (double)i / (count - 1);
                double theta = arc.ThetaStart + arc.Direction * fract * arc.ThetaRange;
                var o = new Vector2((float)Math.Cos(theta), (float)Math.Sin(theta)) * arc.Radius;
                output.Add(arc.Centre + o);
            }

            return output;
        }

        public static int ArcPointCount(in CircularArc arc)
            => 2 * arc.Radius <= circular_arc_tolerance
                ? 2
                : Math.Max(2, (int)Math.Ceiling(arc.ThetaRange / (2 * Math.Acos(1 - circular_arc_tolerance / arc.Radius))));

        public static List<Vector2> Linear(ReadOnlySpan<Vector2> pts) => new List<Vector2>(pts.ToArray());

        // Boehm knot insertion, splits a clamped uniform b-spline into bezier pieces
        static Stack<Vector2[]> splitToBeziers(ReadOnlySpan<Vector2> pts, ref int degree)
        {
            var result = new Stack<Vector2[]>();

            degree = Math.Min(degree, pts.Length - 1);

            int pointCount = pts.Length - 1;
            var points = pts.ToArray();

            if (degree == pointCount)
            {
                result.Push(points);
                return result;
            }

            for (int i = 0; i < pointCount - degree; i++)
            {
                var sub = new Vector2[degree + 1];
                sub[0] = points[i];

                for (int j = 0; j < degree - 1; j++)
                {
                    sub[j + 1] = points[i + 1];

                    for (int k = 1; k < degree - j; k++)
                    {
                        int l = Math.Min(k, pointCount - degree - i);
                        points[i + k] = (l * points[i + k] + points[i + k + 1]) / (l + 1);
                    }
                }

                sub[degree] = points[i + 1];
                result.Push(sub);
            }

            result.Push(points[(pointCount - degree)..]);
            return new Stack<Vector2[]>(result); // flip so pops come out in order
        }

        static bool flatEnough(Vector2[] cp)
        {
            for (int i = 1; i < cp.Length - 1; i++)
            {
                if ((cp[i - 1] - 2 * cp[i] + cp[i + 1]).LengthSquared() > bezier_tolerance * bezier_tolerance * 4)
                    return false;
            }

            return true;
        }

        static void subdivide(Vector2[] cp, Vector2[] l, Vector2[] r, Vector2[] mid, int count)
        {
            for (int i = 0; i < count; ++i)
                mid[i] = cp[i];

            for (int i = 0; i < count; i++)
            {
                l[i] = mid[0];
                r[count - i - 1] = mid[count - i - 1];

                for (int j = 0; j < count - i - 1; j++)
                    mid[j] = (mid[j] + mid[j + 1]) / 2;
            }
        }

        static void approximate(Vector2[] cp, List<Vector2> output, Vector2[] buf1, Vector2[] buf2, int count)
        {
            Vector2[] l = buf2;
            Vector2[] r = buf1;

            subdivide(cp, l, r, buf1, count);

            for (int i = 0; i < count - 1; ++i)
                l[count + i] = r[i + 1];

            output.Add(cp[0]);

            for (int i = 1; i < count - 1; ++i)
            {
                int index = 2 * i;
                output.Add(0.25f * (l[index - 1] + 2 * l[index] + l[index + 1]));
            }
        }

        static Vector2 catmullPoint(Vector2 v1, Vector2 v2, Vector2 v3, Vector2 v4, float t)
        {
            float t2 = t * t;
            float t3 = t * t2;

            return new Vector2(
                0.5f * (2f * v2.X + (-v1.X + v3.X) * t + (2f * v1.X - 5f * v2.X + 4f * v3.X - v4.X) * t2 + (-v1.X + 3f * v2.X - 3f * v3.X + v4.X) * t3),
                0.5f * (2f * v2.Y + (-v1.Y + v3.Y) * t + (2f * v1.Y - 5f * v2.Y + 4f * v3.Y - v4.Y) * t2 + (-v1.Y + 3f * v2.Y - 3f * v3.Y + v4.Y) * t3));
        }
    }

    public readonly struct CircularArc
    {
        public readonly bool IsValid;
        public readonly double ThetaStart;
        public readonly double ThetaRange;
        public readonly double Direction;
        public readonly float Radius;
        public readonly Vector2 Centre;

        public CircularArc(ReadOnlySpan<Vector2> pts)
        {
            Vector2 a = pts[0], b = pts[1], c = pts[2];

            // degenerate triangle, caller falls back to bezier
            if (Precision.AlmostEquals(0, (b.Y - a.Y) * (c.X - a.X) - (b.X - a.X) * (c.Y - a.Y)))
            {
                this = default;
                return;
            }

            float d = 2 * (a.X * (b - c).Y + b.X * (c - a).Y + c.X * (a - b).Y);
            float aSq = a.LengthSquared(), bSq = b.LengthSquared(), cSq = c.LengthSquared();

            Centre = new Vector2(
                aSq * (b - c).Y + bSq * (c - a).Y + cSq * (a - b).Y,
                aSq * (c - b).X + bSq * (a - c).X + cSq * (b - a).X) / d;

            Vector2 dA = a - Centre;
            Vector2 dC = c - Centre;

            Radius = dA.Length();

            ThetaStart = Math.Atan2(dA.Y, dA.X);
            double thetaEnd = Math.Atan2(dC.Y, dC.X);

            while (thetaEnd < ThetaStart)
                thetaEnd += 2 * Math.PI;

            Direction = 1;
            ThetaRange = thetaEnd - ThetaStart;

            // which side of AC is B on
            var ortho = new Vector2((c - a).Y, -(c - a).X);

            if (Vector2.Dot(ortho, b - a) < 0)
            {
                Direction = -Direction;
                ThetaRange = 2 * Math.PI - ThetaRange;
            }

            IsValid = true;
        }
    }
}
