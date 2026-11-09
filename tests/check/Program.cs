// Desktop sanity check for the core: decode maps, autoplay them, print the result.
// usage: check <file.osu|dir> ...

using System;
using System.IO;
using System.Linq;
using System.Numerics;
using Osu5.Beatmaps;
using Osu5.Objects;
using Osu5.Play;

int bad = 0;

// every map goes through twice: as is, then with the mods that move objects around
foreach (var mods in new[] { Mods.None, Mods.HardRock | Mods.Hidden | Mods.Easy })
foreach (string arg in args)
{
    var files = Directory.Exists(arg) ? Directory.GetFiles(arg, "*.osu") : new[] { arg };

    foreach (string f in files)
    {
        Beatmap map;
        try
        {
            map = BeatmapDecoder.Decode(f, mods);
        }
        catch (Exception e)
        {
            Console.WriteLine($"FAIL decode {Path.GetFileName(f)}: {e.Message}");
            bad++;
            continue;
        }

        if (map.Mode != 0 || map.HitObjects.Count == 0)
            continue;

        int circles = map.HitObjects.Count(h => h is HitCircle);
        int sliders = map.HitObjects.Count(h => h is Slider);
        int spinners = map.HitObjects.Count(h => h is Spinner);

        // every head, tick, repeat and tail adds combo, plus circles and spinners
        int expectCombo = circles + spinners + map.HitObjects.OfType<Slider>().Sum(s => s.Nested.Count);

        var p = new Player(map, mods | Mods.Auto);
        var dl = new DrawList();

        double end = map.LastObjectEndTime + 2000;
        for (double t = p.StartTime; t < end && p.State != PlayState.Finished; t += 1)
        {
            p.Update(t, Vector2.Zero, 0);
            if ((int)t % 16 == 0) p.BuildDrawList(dl, t);
        }

        var s = p.Score;
        bool ok = s.Miss == 0 && s.MaxCombo == expectCombo && s.Accuracy >= 0.9999;
        if (!ok) bad++;

        var sr = Osu5.Stars.StarRating.Calculate(BeatmapDecoder.Decode(f, mods), mods);
        double pp = Osu5.Stars.Performance.Calculate(sr, mods, new Osu5.Stars.PlayStats
        {
            Accuracy = s.Accuracy, MaxCombo = s.MaxCombo, Great = s.Great, Ok = s.Ok, Meh = s.Meh, Miss = s.Miss,
            SliderTailHits = s.SliderTailHits, LargeTickMisses = s.LargeTickMisses,
        });
        Console.WriteLine($"{(ok ? "ok  " : "FAIL")} {Path.GetFileNameWithoutExtension(f)} [{mods}] {sr.Stars:F3}* {pp:F1}pp (aim {sr.Aim:F3} speed {sr.Speed:F3} reading {sr.Reading:F3} combo {sr.MaxCombo})");
        Console.WriteLine($"     v{map.FormatVersion} {circles}c {sliders}s {spinners}sp  combo {s.MaxCombo}/{expectCombo}  acc {s.Accuracy:P2}  score {s.TotalScore}  300 {s.Great} 100 {s.Ok} 50 {s.Meh} x {s.Miss}  sb {s.SliderBreaks}  ur {s.UnstableRate:F1}");
    }
}

return bad == 0 ? 0 : 1;
