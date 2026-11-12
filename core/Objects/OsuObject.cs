// Object model trimmed down from osu!'s osu ruleset (https://github.com/ppy/osu), MIT licence.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Beatmaps;
using Osu5.Paths;
using Osu5.Util;

namespace Osu5.Objects
{
    [Flags]
    public enum HitSound : byte
    {
        None = 0,
        Normal = 1,
        Whistle = 2,
        Finish = 4,
        Clap = 8,
    }

    public struct Samples
    {
        public HitSound Sound;
        public byte Bank;      // 0 auto, 1 normal, 2 soft, 3 drum
        public byte AddBank;
        public byte Volume;    // 0 = take it from the timing point
        public ushort Custom;

        public static Samples From(HitSound s) => new Samples { Sound = s };
    }

    public abstract class OsuObject
    {
        public const float ObjectRadius = 64;
        public const double PreemptMin = 450;
        public static readonly DiffRange PreemptRange = new DiffRange(1800, 1200, 450);

        public double StartTime;
        public Vector2 Position;
        public bool NewCombo;
        public int ComboOffset;

        public int StackHeight;
        public float Scale = 1;
        public double TimePreempt = 600;
        public double TimeFadeIn = 400;

        public int ComboIndex;
        public int ComboIndexWithOffsets;
        public int IndexInCombo;
        public bool LastInCombo;

        public Samples Samples;

        public virtual double EndTime => StartTime;
        public virtual Vector2 EndPosition => Position;

        public virtual Vector2 StackOffset => new Vector2(StackHeight * Scale * -6.4f);
        public Vector2 StackedPosition => Position + StackOffset;
        public Vector2 StackedEndPosition => EndPosition + StackOffset;

        public float Radius => ObjectRadius * Scale;

        public virtual void ApplyDefaults(ControlPoints cp, BeatmapDifficulty diff, int formatVersion)
        {
            TimePreempt = (float)Difficulty.Range(diff.ApproachRate, PreemptRange);
            TimeFadeIn = 400 * Math.Min(1, TimePreempt / PreemptMin);
            Scale = Difficulty.ScaleFromCircleSize(diff.CircleSize);
        }
    }

    public sealed class HitCircle : OsuObject
    {
    }

    // osu!mania's long note; only mania maps have them
    public sealed class HoldNote : OsuObject
    {
        public double End;
        public override double EndTime => End;
    }

    public sealed class Spinner : OsuObject
    {
        public static readonly DiffRange ClearRpm = new DiffRange(90, 150, 225);
        public static readonly DiffRange CompleteRpm = new DiffRange(250, 380, 430);
        const int bonus_gap = 2;

        public double Duration;
        public int SpinsRequired = 1;
        public int MaxBonusSpins = 1;

        public override double EndTime => StartTime + Duration;
        public override Vector2 StackOffset => Vector2.Zero;

        public int SpinsForBonus => SpinsRequired + bonus_gap;
        public int TotalTicks => SpinsRequired + bonus_gap + MaxBonusSpins;

        public override void ApplyDefaults(ControlPoints cp, BeatmapDifficulty diff, int formatVersion)
        {
            base.ApplyDefaults(cp, diff, formatVersion);

            double minRps = Difficulty.Range(diff.OverallDifficulty, ClearRpm) / 60;
            double maxRps = Difficulty.Range(diff.OverallDifficulty, CompleteRpm) / 60;
            double secs = Duration / 1000;

            SpinsRequired = (int)(minRps * secs + 0.0001);
            MaxBonusSpins = Math.Max(0, (int)(maxRps * secs + 0.0001) - SpinsRequired - bonus_gap);
        }
    }

    public enum NestedKind : byte
    {
        Head,
        Tick,
        Repeat,
        Tail,
    }

    public struct SliderNested
    {
        public NestedKind Kind;
        public double Time;
        public double SpanStartTime;
        public int SpanIndex;
        public double PathProgress;
        public Vector2 Position;
    }

    public sealed class Slider : OsuObject
    {
        const double base_scoring_distance = 100;

        // stable ends the slider 36ms early for judging, lazer keeps that
        public const double TailLeniency = -36;

        public SliderPath Path = null!;
        public int RepeatCount;
        public List<Samples> NodeSamples = new List<Samples>();

        public double Velocity = 1;
        public double TickDistance;
        public double TickDistanceMultiplier = 1;
        public double SliderVelocity = 1;
        public bool GenerateTicks = true;
        // length as a taiko drum roll, longer than the slider itself; NaN otherwise
        public double RollDuration = double.NaN;

        public readonly List<SliderNested> Nested = new List<SliderNested>();

        public int SpanCount => RepeatCount + 1;
        public double SpanDuration => Path.Distance / Velocity;
        public double Duration => SpanCount * SpanDuration;
        public override double EndTime => StartTime + Duration;

        public override Vector2 EndPosition => Position + CurvePositionAt(1);

        // progress over the whole slider (repeats included) to position along the path
        public double ProgressAt(double progress)
        {
            double p = progress * SpanCount % 1;
            if ((int)(progress * SpanCount) % 2 == 1)
                p = 1 - p;
            return p;
        }

        public Vector2 CurvePositionAt(double progress) => Path.PositionAt(ProgressAt(progress));

        public override void ApplyDefaults(ControlPoints cp, BeatmapDifficulty diff, int formatVersion)
        {
            base.ApplyDefaults(cp, diff, formatVersion);

            var timing = cp.TimingAt(StartTime);
            var dp = cp.DifficultyAt(StartTime);
            SliderVelocity = dp.SliderVelocity;
            GenerateTicks = dp.GenerateTicks;

            if (formatVersion < 8)
                TickDistanceMultiplier = 1 / SliderVelocity;

            Velocity = base_scoring_distance * diff.SliderMultiplier / precisionAdjustedBeatLength(timing.BeatLength);

            // not simplified on purpose, the float error matches stable
            double scoringDistance = Velocity * timing.BeatLength;
            TickDistance = GenerateTicks ? scoringDistance / diff.SliderTickRate * TickDistanceMultiplier : double.PositiveInfinity;

            buildNested();
        }

        double precisionAdjustedBeatLength(double beatLength)
        {
            double asBeatLength = -100 / SliderVelocity;
            double mult = asBeatLength < 0 ? Math.Clamp((float)-asBeatLength, 10, 1000) / 100.0 : 1;
            return beatLength * mult;
        }

        void buildNested()
        {
            Nested.Clear();

            double spanDuration = SpanDuration;
            int spans = SpanCount;
            double length = Math.Min(100000, Path.Distance);
            double tickDist = Math.Clamp(TickDistance, 0, length);
            double minFromEnd = Velocity * 10;

            add(NestedKind.Head, StartTime, StartTime, 0, 0);

            for (int span = 0; span < spans; span++)
            {
                double spanStart = StartTime + span * spanDuration;
                bool reversed = span % 2 == 1;

                if (tickDist != 0)
                {
                    int first = Nested.Count;

                    for (double d = tickDist; d <= length; d += tickDist)
                    {
                        if (d >= length - minFromEnd)
                            break;

                        double pathProgress = d / length;
                        double timeProgress = reversed ? 1 - pathProgress : pathProgress;
                        add(NestedKind.Tick, spanStart + timeProgress * spanDuration, spanStart, span, pathProgress);
                    }

                    if (reversed)
                        Nested.Reverse(first, Nested.Count - first);
                }

                if (span < spans - 1)
                    add(NestedKind.Repeat, StartTime + (span + 1) * spanDuration, spanStart, span, (span + 1) % 2);
            }

            int last = spans - 1;
            add(NestedKind.Tail, StartTime + spans * spanDuration, StartTime + last * spanDuration, last, spans % 2);
        }

        void add(NestedKind kind, double time, double spanStart, int span, double progress)
        {
            Nested.Add(new SliderNested
            {
                Kind = kind,
                Time = time,
                SpanStartTime = spanStart,
                SpanIndex = span,
                PathProgress = progress,
                Position = Position + Path.PositionAt(progress),
            });
        }

        // time at which the tail is actually checked
        public double TailJudgeTime => Math.Max(StartTime + Duration / 2, EndTime + TailLeniency);
    }
}
