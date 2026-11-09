using System.Collections.Generic;
using Osu5.Objects;

namespace Osu5.Beatmaps
{
    public sealed class BeatmapDifficulty
    {
        public float DrainRate = 5;
        public float CircleSize = 5;
        public float OverallDifficulty = 5;
        public float ApproachRate = 5;
        public double SliderMultiplier = 1.4;
        public double SliderTickRate = 1;
    }

    public sealed class BeatmapMetadata
    {
        public string Title = "";
        public string TitleUnicode = "";
        public string Artist = "";
        public string ArtistUnicode = "";
        public string Creator = "";
        public string Version = "";
        public string Source = "";
        public string Tags = "";
        public string AudioFile = "";
        public string BackgroundFile = "";
        public int PreviewTime = -1;
        public int BeatmapId;
        public int BeatmapSetId = -1;
    }

    public readonly record struct BreakPeriod(double Start, double End)
    {
        public double Duration => End - Start;

        // lazer ignores breaks shorter than this
        public bool HasEffect => Duration >= 650;
    }

    public sealed class Beatmap
    {
        public int FormatVersion = 14;
        public int Mode;
        public int AudioLeadIn;
        public float StackLeniency = 0.7f;

        public readonly BeatmapMetadata Metadata = new BeatmapMetadata();
        public readonly BeatmapDifficulty Difficulty = new BeatmapDifficulty();
        public readonly ControlPoints ControlPoints = new ControlPoints();
        public readonly List<BreakPeriod> Breaks = new List<BreakPeriod>();
        public List<OsuObject> HitObjects = new List<OsuObject>();

        // file colours, empty means the skin defaults
        public readonly List<uint> ComboColours = new List<uint>();

        public double FirstObjectTime => HitObjects.Count > 0 ? HitObjects[0].StartTime : 0;

        public double LastObjectEndTime
        {
            get
            {
                double t = 0;
                foreach (var h in HitObjects)
                    if (h.EndTime > t) t = h.EndTime;
                return t;
            }
        }
    }
}
