using System.Collections.Generic;
using System.Numerics;
using Osu5.Scoring;

namespace Osu5.Play
{
    // what the exports need from a ruleset's player
    public interface IGame
    {
        ScoreProcessor Score { get; }
        Queue<SoundEvent> Sounds { get; }
        double StartTime { get; }
        void Update(double time, Vector2 cursor, int keys);
        void BuildDrawList(DrawList dl, double time);
        void FillHud(ref Hud hud, double time);
    }
}
