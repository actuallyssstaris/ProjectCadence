using System;
using System.Collections.Generic;
using Osu5.Beatmaps;
using Osu5.Util;

namespace Osu5.Scoring
{
    // Draining health in the style of lazer's DrainingHealthProcessor: pick a drain
    // rate so that a perfect play bottoms out near a target health that depends on HP.
    public sealed class HealthProcessor
    {
        public double Health { get; private set; } = 1;
        public double DrainRate { get; private set; }
        public bool Failed => Health <= 0;

        readonly List<BreakPeriod> breaks;
        readonly double drainStart, drainEnd;
        double lastTime = double.NaN;

        public HealthProcessor(Beatmap map, IReadOnlyList<(double Time, HitResult Max)> judgements)
        {
            breaks = map.Breaks;
            // no drain before the first object or after the last one
            drainStart = map.FirstObjectTime;
            drainEnd = map.LastObjectEndTime;
            computeDrain(map, judgements);
        }

        void computeDrain(Beatmap map, IReadOnlyList<(double Time, HitResult Max)> judgements)
        {
            if (judgements.Count == 0) return;

            double target = Difficulty.Range(map.Difficulty.DrainRate, 0.95, 0.70, 0.30);
            double drain = 1;
            double adjust = 1;

            for (int iter = 0; iter < 20; iter++)
            {
                double hp = 1, lowest = 1;
                double t = judgements[0].Time;

                foreach (var (time, max) in judgements)
                {
                    hp -= drain * drainedTime(t, time);
                    t = time;
                    lowest = Math.Min(lowest, hp);
                    hp = Math.Min(1, hp + HitResults.Health(max));
                }

                if (Math.Abs(lowest - target) < 0.01) break;

                adjust *= 2;
                drain += 1.0 / adjust * Math.Sign(lowest - target);
            }

            DrainRate = Math.Max(0, drain) / 1000.0; // per ms
        }

        double drainedTime(double from, double to)
        {
            from = Math.Max(from, drainStart);
            to = Math.Min(to, drainEnd);
            double total = Math.Max(0, to - from);
            foreach (var b in breaks)
            {
                if (!b.HasEffect) continue;
                double s = Math.Max(from, b.Start), e = Math.Min(to, b.End);
                if (e > s) total -= e - s;
            }

            return Math.Max(0, total);
        }

        public void Update(double time)
        {
            if (!double.IsNaN(lastTime) && time > lastTime)
                Health = Math.Max(0, Health - DrainRate * drainedTime(lastTime, time));
            lastTime = time;
        }

        public void Apply(HitResult result) => Health = Math.Clamp(Health + HitResults.Health(result), 0, 1);
    }
}
