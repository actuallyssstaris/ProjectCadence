// Adapted from osu!'s SliderPath (https://github.com/ppy/osu), MIT licence, (c) ppy Pty Ltd.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Util;

namespace Osu5.Paths
{
    public enum CurveKind : byte
    {
        Catmull,
        Bezier,
        BSpline,
        Linear,
        Perfect,
    }

    public readonly record struct PathType(CurveKind Kind, int Degree = 0)
    {
        public static readonly PathType Catmull = new(CurveKind.Catmull);
        public static readonly PathType Bezier = new(CurveKind.Bezier);
        public static readonly PathType Linear = new(CurveKind.Linear);
        public static readonly PathType Perfect = new(CurveKind.Perfect);
    }

    public struct ControlPoint
    {
        public Vector2 Position;
        public PathType? Type;

        public ControlPoint(Vector2 pos, PathType? type = null)
        {
            Position = pos;
            Type = type;
        }
    }

    public sealed class SliderPath
    {
        public readonly ControlPoint[] ControlPoints;
        public readonly double? ExpectedDistance;

        readonly List<Vector2> path = new List<Vector2>();
        readonly List<double> cumulative = new List<double>();
        double optimisedLength;
        double calculatedLength;

        public SliderPath(ControlPoint[] controlPoints, double? expectedDistance)
        {
            ControlPoints = controlPoints;
            ExpectedDistance = expectedDistance;
            build();
            measure();
        }

        public double Distance => cumulative.Count == 0 ? 0 : cumulative[^1];

        public double CalculatedDistance => calculatedLength;

        public IReadOnlyList<Vector2> Points => path;

        public Vector2 PositionAt(double progress)
        {
            double d = Math.Clamp(progress, 0, 1) * Distance;
            return lerpAt(indexOf(d), d);
        }

        // vertices between two progress values, used for snaking sliders
        public void PathBetween(double p0, double p1, List<Vector2> into)
        {
            double d0 = Math.Clamp(p0, 0, 1) * Distance;
            double d1 = Math.Clamp(p1, 0, 1) * Distance;

            int i = 0;
            for (; i < path.Count && cumulative[i] < d0; ++i) { }

            into.Add(lerpAt(i, d0));

            for (; i < path.Count && cumulative[i] <= d1; ++i)
                into.Add(path[i]);

            into.Add(lerpAt(i, d1));
        }

        void build()
        {
            path.Clear();
            optimisedLength = 0;

            int n = ControlPoints.Length;
            if (n == 0)
                return;

            var verts = new Vector2[n];
            for (int i = 0; i < n; i++)
                verts[i] = ControlPoints[i].Position;

            int start = 0;

            for (int i = 0; i < n; i++)
            {
                if (ControlPoints[i].Type == null && i < n - 1)
                    continue;

                var seg = verts.AsSpan(start, i - start + 1);
                var type = ControlPoints[start].Type ?? PathType.Linear;

                if (seg.Length == 1)
                    path.Add(seg[0]);
                else if (seg.Length > 1)
                {
                    var sub = subPath(seg, type);
                    bool skipFirst = path.Count > 0 && sub.Count > 0 && path[^1] == sub[0];

                    for (int j = skipFirst ? 1 : 0; j < sub.Count; j++)
                        path.Add(sub[j]);
                }

                start = i;
            }
        }

        List<Vector2> subPath(ReadOnlySpan<Vector2> cp, PathType type)
        {
            switch (type.Kind)
            {
                case CurveKind.Linear:
                    return PathApproximator.Linear(cp);

                case CurveKind.Perfect:
                {
                    if (cp.Length != 3)
                        break;

                    var arc = new CircularArc(cp);
                    if (!arc.IsValid)
                        break;

                    // absurdly long arcs fall back to bezier, same as lazer
                    if (PathApproximator.ArcPointCount(arc) >= 1000)
                        break;

                    var sub = PathApproximator.CircularArc(cp);
                    if (sub.Count == 0)
                        break;

                    return sub;
                }

                case CurveKind.Catmull:
                    return optimiseCatmull(PathApproximator.Catmull(cp));
            }

            int degree = type.Kind == CurveKind.BSpline && type.Degree > 0 ? type.Degree : cp.Length;
            return PathApproximator.BSpline(cp, degree);
        }

        // stable drops catmull vertices closer than 6px when drawing. Without this,
        // sliders with repeated knots grow little bulbs and come out too long.
        List<Vector2> optimiseCatmull(List<Vector2> sub)
        {
            const int segment_len = 100; // catmull_detail * 2

            var outp = new List<Vector2>(sub.Count);
            Vector2? lastStart = null;
            double removed = 0;

            for (int i = 0; i < sub.Count; i++)
            {
                if (lastStart == null)
                {
                    outp.Add(sub[i]);
                    lastStart = sub[i];
                    continue;
                }

                double fromStart = Vector2.Distance(lastStart.Value, sub[i]);
                removed += Vector2.Distance(sub[i - 1], sub[i]);

                if (fromStart > 6 || (i + 1) % segment_len == 0 || i == sub.Count - 1)
                {
                    outp.Add(sub[i]);
                    optimisedLength += removed - fromStart;
                    lastStart = null;
                    removed = 0;
                }
            }

            return outp;
        }

        void measure()
        {
            calculatedLength = optimisedLength;
            cumulative.Clear();
            cumulative.Add(0);

            for (int i = 0; i < path.Count - 1; i++)
            {
                calculatedLength += (path[i + 1] - path[i]).Length();
                cumulative.Add(calculatedLength);
            }

            if (ExpectedDistance is not double expected || calculatedLength == expected)
                return;

            // stable doesn't extend if the last two points are the same
            if (path.Count >= 2 && path[^1] == path[^2] && expected > calculatedLength)
            {
                cumulative.Add(calculatedLength);
                return;
            }

            cumulative.RemoveAt(cumulative.Count - 1);
            int end = path.Count - 1;

            if (calculatedLength > expected)
            {
                while (cumulative.Count > 0 && cumulative[^1] >= expected)
                {
                    cumulative.RemoveAt(cumulative.Count - 1);
                    path.RemoveAt(end--);
                }
            }

            if (end <= 0)
            {
                cumulative.Add(0);
                return;
            }

            var dir = Vector2.Normalize(path[end] - path[end - 1]);
            if (float.IsNaN(dir.X)) dir = Vector2.Zero;

            path[end] = path[end - 1] + dir * (float)(expected - cumulative[^1]);
            cumulative.Add(expected);
        }

        int indexOf(double d)
        {
            int i = cumulative.BinarySearch(d);
            return i < 0 ? ~i : i;
        }

        Vector2 lerpAt(int i, double d)
        {
            if (path.Count == 0)
                return Vector2.Zero;
            if (i <= 0)
                return path[0];
            if (i >= path.Count)
                return path[^1];

            Vector2 p0 = path[i - 1], p1 = path[i];
            double d0 = cumulative[i - 1], d1 = cumulative[i];

            if (Precision.AlmostEquals(d0, d1))
                return p0;

            double w = (d - d0) / (d1 - d0);
            return p0 + (p1 - p0) * (float)w;
        }
    }
}
