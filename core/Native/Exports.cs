using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Osu5.Beatmaps;
using Osu5.Play;

namespace Osu5.Native
{
    // C entry points. Prototypes live in host/include/osu5.h.
    // Nothing here may throw across the boundary, so every export catches.
    public static unsafe class Exports
    {
        static Beatmap? map;
        static Player? player;
        static readonly DrawList drawList = new DrawList();
        static string lastError = "";
        static Mods mods;
        static uint[]? skinColours;

        [UnmanagedCallersOnly(EntryPoint = "osu5_load", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Load(byte* path)
        {
            try
            {
                string p = Marshal.PtrToStringUTF8((IntPtr)path) ?? "";
                map = BeatmapDecoder.Decode(p, mods);

                if (map.Mode != 0)
                {
                    lastError = "only osu!standard maps are supported";
                    map = null;
                    player = null;
                    return -2;
                }

                player = new Player(map, mods, skinColours);
                return 0;
            }
            catch (Exception e)
            {
                lastError = e.Message;
                map = null;
                player = null;
                return -1;
            }
        }

        // Runs one full collection with this native frame on the stack. A broken
        // unwinder setup takes the game down here at startup, not mid-map.
        [UnmanagedCallersOnly(EntryPoint = "osu5_gc_check", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int GcCheck()
        {
            try
            {
                var junk = new object[4096];
                for (int i = 0; i < junk.Length; i++) junk[i] = new byte[64];
                GC.Collect();
                GC.WaitForPendingFinalizers();
                return GC.CollectionCount(0);
            }
            catch (Exception e)
            {
                lastError = e.Message;
                return -1;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_unload", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void Unload()
        {
            map = null;
            player = null;
            drawList.Clear();
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_set_mods", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void SetMods(int flags)
        {
            // takes effect on the next load, most mods change the map itself
            mods = (Mods)flags;
        }

        // 0xAARRGGBB, n = 0 clears; used for maps that bring no colours of their own
        [UnmanagedCallersOnly(EntryPoint = "osu5_set_skin_colours", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void SetSkinColours(uint* cols, int n)
        {
            if (n <= 0)
            {
                skinColours = null;
                return;
            }
            skinColours = new uint[Math.Min(n, 8)];
            for (int i = 0; i < skinColours.Length; i++)
                skinColours[i] = cols[i];
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_start_time", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static double StartTime()
        {
            try
            {
                return player?.StartTime ?? 0;
            }
            catch (Exception e)
            {
                lastError = e.Message;
                return 0;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_update", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void Update(double time, float x, float y, int keys)
        {
            try
            {
                player?.Update(time, new Vector2(x, y), keys);
            }
            catch (Exception e)
            {
                lastError = e.Message;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_frame", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Frame(double time, DrawCmd* cmds, int maxCmds, float* points, int maxPoints, int* pointCount)
        {
            *pointCount = 0;
            if (player == null) return 0;

            try
            {
                player.BuildDrawList(drawList, time);

                int np = Math.Min(drawList.Points.Count, maxPoints);
                for (int i = 0; i < np; i++)
                {
                    points[i * 2] = drawList.Points[i].X;
                    points[i * 2 + 1] = drawList.Points[i].Y;
                }
                *pointCount = np;

                int n = 0;
                foreach (var c in drawList.Commands)
                {
                    if (n >= maxCmds) break;
                    var cmd = c;
                    // drop paths that got cut off by the point buffer
                    if (cmd.PathCount > 0 && cmd.PathStart + cmd.PathCount > np)
                        cmd.PathCount = Math.Max(0, np - cmd.PathStart);
                    cmds[n++] = cmd;
                }

                return n;
            }
            catch (Exception e)
            {
                lastError = e.Message;
                return 0;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_get_hud", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void GetHud(double time, Hud* hud)
        {
            *hud = default;
            try
            {
                player?.FillHud(ref *hud, time);
            }
            catch (Exception e)
            {
                lastError = e.Message;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_poll_sound", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int PollSound(SoundEvent* ev)
        {
            if (player == null || player.Sounds.Count == 0) return 0;
            *ev = player.Sounds.Dequeue();
            return 1;
        }

        // 0 title, 1 artist, 2 difficulty name, 3 creator, 4 audio file, 5 background, 6 last error
        [UnmanagedCallersOnly(EntryPoint = "osu5_string", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int GetString(int id, byte* buf, int len)
        {
            try
            {
                return writeString(stringFor(map, id), buf, len);
            }
            catch
            {
                if (len > 0) buf[0] = 0;
                return 0;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_difficulty", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void GetDifficulty(float* out4)
        {
            if (map == null) return;
            out4[0] = map.Difficulty.CircleSize;
            out4[1] = map.Difficulty.ApproachRate;
            out4[2] = map.Difficulty.OverallDifficulty;
            out4[3] = map.Difficulty.DrainRate;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PeekInfo
        {
            public int Mode;
            public int PreviewTime;
            public int BeatmapSetId;
            public float CS, AR, OD, HP;
            public fixed byte Title[128];
            public fixed byte Artist[128];
            public fixed byte Version[96];
            public fixed byte Creator[64];
            public fixed byte Audio[128];
            public fixed byte Background[128];
        }

        [UnmanagedCallersOnly(EntryPoint = "osu5_peek", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Peek(byte* path, PeekInfo* info)
        {
            try
            {
                string p = Marshal.PtrToStringUTF8((IntPtr)path) ?? "";
                var m = BeatmapDecoder.Peek(p);

                *info = default;
                info->Mode = m.Mode;
                info->PreviewTime = m.Metadata.PreviewTime;
                info->BeatmapSetId = m.Metadata.BeatmapSetId;
                info->CS = m.Difficulty.CircleSize;
                info->AR = m.Difficulty.ApproachRate;
                info->OD = m.Difficulty.OverallDifficulty;
                info->HP = m.Difficulty.DrainRate;

                writeString(m.Metadata.Title, info->Title, 128);
                writeString(m.Metadata.Artist, info->Artist, 128);
                writeString(m.Metadata.Version, info->Version, 96);
                writeString(m.Metadata.Creator, info->Creator, 64);
                writeString(m.Metadata.AudioFile, info->Audio, 128);
                writeString(m.Metadata.BackgroundFile, info->Background, 128);
                return 0;
            }
            catch (Exception e)
            {
                lastError = e.Message;
                return -1;
            }
        }

        static string stringFor(Beatmap? m, int id)
        {
            if (id == 6) return lastError;
            if (m == null) return "";

            return id switch
            {
                0 => m.Metadata.Title,
                1 => m.Metadata.Artist,
                2 => m.Metadata.Version,
                3 => m.Metadata.Creator,
                4 => m.Metadata.AudioFile,
                5 => m.Metadata.BackgroundFile,
                _ => "",
            };
        }

        static int writeString(string s, byte* buf, int len)
        {
            if (len <= 0) return 0;

            byte[] bytes = Encoding.UTF8.GetBytes(s);
            int n = Math.Min(bytes.Length, len - 1);
            // don't split a multibyte sequence
            while (n > 0 && n < bytes.Length && (bytes[n] & 0xC0) == 0x80)
                n--;

            bytes.AsSpan(0, n).CopyTo(new Span<byte>(buf, n));
            buf[n] = 0;
            return n;
        }
    }
}
