using System.Collections.Generic;

namespace Osu5.Beatmaps
{
    public readonly record struct TimingPoint(double Time, double BeatLength, int Meter, bool OmitFirstBarLine);

    public readonly record struct DifficultyPoint(double Time, double SliderVelocity, bool GenerateTicks);

    public readonly record struct EffectPoint(double Time, bool Kiai);

    public readonly record struct SamplePoint(double Time, int Bank, int CustomIndex, int Volume);

    public sealed class ControlPoints
    {
        public readonly List<TimingPoint> Timing = new List<TimingPoint>();
        public readonly List<DifficultyPoint> Difficulty = new List<DifficultyPoint>();
        public readonly List<EffectPoint> Effects = new List<EffectPoint>();
        public readonly List<SamplePoint> Samples = new List<SamplePoint>();

        static readonly TimingPoint default_timing = new TimingPoint(0, 60000.0 / 60, 4, false);
        static readonly DifficultyPoint default_difficulty = new DifficultyPoint(0, 1, true);

        // before the first timing point we use the first one, like lazer
        public TimingPoint TimingAt(double time)
        {
            if (Timing.Count == 0) return default_timing;
            int i = find(Timing, time, static p => p.Time);
            return i < 0 ? Timing[0] : Timing[i];
        }

        public DifficultyPoint DifficultyAt(double time)
        {
            int i = find(Difficulty, time, static p => p.Time);
            return i < 0 ? default_difficulty : Difficulty[i];
        }

        public bool KiaiAt(double time)
        {
            int i = find(Effects, time, static p => p.Time);
            return i >= 0 && Effects[i].Kiai;
        }

        public SamplePoint SampleAt(double time)
        {
            if (Samples.Count == 0) return new SamplePoint(0, 1, 0, 100);
            int i = find(Samples, time, static p => p.Time);
            return i < 0 ? Samples[0] : Samples[i];
        }

        // last index with Time <= time, -1 if none
        static int find<T>(List<T> list, double time, System.Func<T, double> key)
        {
            int lo = 0, hi = list.Count - 1, best = -1;

            while (lo <= hi)
            {
                int mid = (lo + hi) >> 1;
                if (key(list[mid]) <= time)
                {
                    best = mid;
                    lo = mid + 1;
                }
                else
                    hi = mid - 1;
            }

            return best;
        }
    }
}
