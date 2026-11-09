using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;

namespace Osu5.Play
{
    // Shared with host/include/osu5.h, keep the layouts in sync.
    public enum DrawKind : int
    {
        Circle = 1,
        Approach = 2,
        SliderBody = 3,
        SliderBall = 4,
        FollowCircle = 5,
        Tick = 6,
        Reverse = 7,
        Spinner = 8,
        Judgement = 9,
        FollowPoint = 10,
        Cursor = 11,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DrawCmd
    {
        public DrawKind Kind;
        public uint Colour;
        public int Number;
        public int Flags;
        public float X, Y;
        public float Radius;
        public float Alpha;
        public float P0, P1;
        public int PathStart;
        public int PathCount;
    }

    public sealed class DrawList
    {
        public readonly List<DrawCmd> Commands = new List<DrawCmd>(512);
        public readonly List<Vector2> Points = new List<Vector2>(8192);

        public void Clear()
        {
            Commands.Clear();
            Points.Clear();
        }

        public void Add(in DrawCmd cmd) => Commands.Add(cmd);
    }

    public enum SoundKind : int
    {
        Hit = 1,
        SliderTick = 2,
        SliderSlide = 3,
        SpinnerBonus = 4,
        ComboBreak = 5,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SoundEvent
    {
        public SoundKind Kind;
        public int Sounds;   // HitSound bits
        public int Bank;     // 1 normal, 2 soft, 3 drum
        public int AddBank;
        public int Volume;   // 0..100
        public int Custom;
        public float Pan;    // -1..1
    }

    public enum PlayState : int
    {
        Intro = 0,
        Playing = 1,
        Break = 2,
        Failed = 3,
        Finished = 4,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Hud
    {
        public long Score;
        public int Combo;
        public int MaxCombo;
        public float Accuracy;
        public float Health;
        public int Great, Ok, Meh, Miss;
        public PlayState State;
        public int Rank;
        public float Progress;
        public float UnstableRate;
        public double SkipTarget;    // > 0 when the intro or a break can be skipped
    }
}
