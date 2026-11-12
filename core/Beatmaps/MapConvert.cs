using System;
using System.Collections.Generic;
using Osu5.Objects;

namespace Osu5.Beatmaps
{
    // osu! maps played as another mode, the way lazer offers "converted beatmaps".
    // Taiko and catch already read sliders and spinners from the osu! objects
    // (drumrolls and swells, juice streams and bananas), so for those the map only
    // changes its mode. Mania needs a key count and holds where sliders were.
    public static class MapConvert
    {
        public static void To(Beatmap map, int mode)
        {
            if (map.Mode != 0 || mode <= 0 || mode > 3)
                return;

            if (mode == 3)
                toMania(map);
            map.Mode = mode;
        }

        // lazer's ManiaBeatmapConverter picks the key count from how much of the map
        // is sliders and spinners, and from CS / OD
        public static int ManiaKeys(Beatmap map)
        {
            int total = map.HitObjects.Count, long_ = 0;
            foreach (var h in map.HitObjects)
                if (h is Slider || h is Spinner)
                    long_++;
            double share = total > 0 ? (double)long_ / total : 0;
            int cs = (int)Math.Round(map.Difficulty.CircleSize);
            int od = (int)Math.Round(map.Difficulty.OverallDifficulty);

            if (share < 0.2)
                return 7;
            if (share < 0.3 || cs >= 5)
                return od > 5 ? 7 : 6;
            if (share > 0.6)
                return od > 4 ? 5 : 4;
            return Math.Max(4, Math.Min(od + 1, 7));
        }

        static void toMania(Beatmap map)
        {
            int keys = ManiaKeys(map);
            var objects = new List<OsuObject>(map.HitObjects.Count);
            int lastCol = -1;
            double lastTime = double.NegativeInfinity;
            foreach (var h in map.HitObjects)
            {
                // the column from x, nudged off the previous one when notes come fast,
                // so streams don't turn into one long jack
                int col = Math.Clamp((int)(h.Position.X * keys / 512), 0, keys - 1);
                if (col == lastCol && h.StartTime - lastTime < 120)
                    col = (col + 1) % keys;
                lastCol = col;
                lastTime = h.EndTime;
                var pos = new System.Numerics.Vector2((col + 0.5f) * 512f / keys, 192);

                if (h is Slider || h is Spinner)
                    objects.Add(new HoldNote { StartTime = h.StartTime, End = h.EndTime, Position = pos, Samples = h.Samples });
                else
                    objects.Add(new HitCircle { StartTime = h.StartTime, Position = pos, Samples = h.Samples });
            }
            map.HitObjects = objects;
            map.Difficulty.CircleSize = keys;
        }
    }
}
