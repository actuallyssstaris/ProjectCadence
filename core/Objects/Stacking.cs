// Stacking and combo numbering, from osu!'s OsuBeatmapProcessor / OsuHitObject
// (https://github.com/ppy/osu), MIT licence. Stacking logic is stable's, quirks included.

using System.Collections.Generic;
using System.Numerics;
using Osu5.Beatmaps;

namespace Osu5.Objects
{
    public static class Combo
    {
        public static void Assign(List<OsuObject> objects)
        {
            OsuObject? last = null;

            foreach (var h in objects)
            {
                if (h is not Spinner && (last == null || last is Spinner))
                    h.NewCombo = true;

                int index = last?.ComboIndex ?? 0;
                int withOffsets = last?.ComboIndexWithOffsets ?? 0;
                int inCombo = last != null ? last.IndexInCombo + 1 : 0;

                // spinners never start a combo for colouring purposes
                if (h is not Spinner && (h.NewCombo || last == null || last is Spinner))
                {
                    inCombo = 0;
                    index++;
                    withOffsets += h.ComboOffset + 1;
                    if (last != null) last.LastInCombo = true;
                }

                h.ComboIndex = index;
                h.ComboIndexWithOffsets = withOffsets;
                h.IndexInCombo = inCombo;
                last = h;
            }

            if (last != null) last.LastInCombo = true;
        }
    }

    public static class Stacking
    {
        const int stack_distance = 3;

        public static void Apply(Beatmap map)
        {
            var objs = map.HitObjects;
            if (objs.Count == 0) return;

            foreach (var h in objs)
                h.StackHeight = 0;

            if (map.FormatVersion >= 6)
                applyNew(map, objs);
            else
                applyOld(map, objs);
        }

        static float threshold(Beatmap map, OsuObject h) => (int)h.TimePreempt * map.StackLeniency;

        static void applyNew(Beatmap map, List<OsuObject> objs)
        {
            int startIndex = 0;
            int endIndex = objs.Count - 1;
            int extendedStart = startIndex;

            for (int i = endIndex; i > startIndex; i--)
            {
                int n = i;
                var objI = objs[i];
                if (objI.StackHeight != 0 || objI is Spinner) continue;

                float stackThreshold = threshold(map, objI);

                if (objI is HitCircle)
                {
                    while (--n >= 0)
                    {
                        var objN = objs[n];
                        if (objN is Spinner) continue;

                        // stable compares integer times here
                        if ((int)objI.StartTime - (int)objN.EndTime > stackThreshold)
                            break;

                        if (n < extendedStart)
                        {
                            objN.StackHeight = 0;
                            extendedStart = n;
                        }

                        // circles under the end of a slider get pushed down-right instead
                        if (objN is Slider && Vector2.Distance(objN.EndPosition, objI.Position) < stack_distance)
                        {
                            int off = objI.StackHeight - objN.StackHeight + 1;

                            for (int j = n + 1; j <= i; j++)
                            {
                                var objJ = objs[j];
                                if (Vector2.Distance(objN.EndPosition, objJ.Position) < stack_distance)
                                    objJ.StackHeight -= off;
                            }

                            break;
                        }

                        if (Vector2.Distance(objN.Position, objI.Position) < stack_distance)
                        {
                            objN.StackHeight = objI.StackHeight + 1;
                            objI = objN;
                        }
                    }
                }
                else if (objI is Slider)
                {
                    while (--n >= startIndex)
                    {
                        var objN = objs[n];
                        if (objN is Spinner) continue;

                        if (objI.StartTime - objN.StartTime > stackThreshold)
                            break;

                        if (Vector2.Distance(objN.EndPosition, objI.Position) < stack_distance)
                        {
                            objN.StackHeight = objI.StackHeight + 1;
                            objI = objN;
                        }
                    }
                }
            }
        }

        static void applyOld(Beatmap map, List<OsuObject> objs)
        {
            for (int i = 0; i < objs.Count; i++)
            {
                var cur = objs[i];
                if (cur.StackHeight != 0 && cur is not Slider)
                    continue;

                double startTime = cur.EndTime;
                int sliderStack = 0;

                for (int j = i + 1; j < objs.Count; j++)
                {
                    if (objs[j].StartTime - threshold(map, objs[i]) > startTime)
                        break;

                    Vector2 pos2 = cur is Slider s ? s.Position + s.Path.PositionAt(1) : cur.Position;

                    if (Vector2.Distance(objs[j].Position, cur.Position) < stack_distance)
                    {
                        cur.StackHeight++;
                        startTime = objs[j].StartTime;
                    }
                    else if (Vector2.Distance(objs[j].Position, pos2) < stack_distance)
                    {
                        sliderStack++;
                        objs[j].StackHeight -= sliderStack;
                        startTime = objs[j].StartTime;
                    }
                }
            }
        }
    }
}
