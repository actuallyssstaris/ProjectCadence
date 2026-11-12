<div align="center">

<img src="docs/logo.png" width="160">

# ProjectCadence

**osu! on a jailbroken PS5, running natively.**

Circles, taiko, catch and mania, with lazer's gameplay and a menu that looks like lazer's.

[![release](https://img.shields.io/github/v/release/actuallyssstaris/ProjectCadence?style=flat-square&color=ff66aa)](https://github.com/actuallyssstaris/ProjectCadence/releases)
[![downloads](https://img.shields.io/github/downloads/actuallyssstaris/ProjectCadence/total?style=flat-square&color=8866ee)](https://github.com/actuallyssstaris/ProjectCadence/releases)
[![license](https://img.shields.io/badge/license-GPL--3.0-66ccff?style=flat-square)](LICENSE)
![platform](https://img.shields.io/badge/platform-PS5-222?style=flat-square)

[Download](https://github.com/actuallyssstaris/ProjectCadence/releases) · [Controls](#controls) · [Maps](#maps) · [Skins](#skins) · [Building](#building)

</div>

---

## Why it plays right

The gameplay is a port of lazer's own code, not a lookalike. Timing windows, slider
ticks, stacking, scoring, health drain, star rating and pp all come out the same as
lazer. Converts do too: an osu! map played as taiko or mania goes through ports of
lazer's converters, so you get the same patterns you'd get on PC.

<table>
<tr>
<td width="50%" valign="top">

**Gameplay**
- osu!, taiko, catch and mania
- converts from osu! maps to the other three modes
- EZ, NF, HT, HR, HD, DT and Auto (DT and HT keep the pitch)
- storyboards and background videos
- replays of your best score on each difficulty

</td>
<td width="50%" valign="top">

**Around it**
- downloads maps and skins on the console
- local scores with pp, plus the osu.ppy.sh leaderboard
- lazer-style menus, or the older simplified ones
- first-run setup that walks you through it
- multiplayer rooms and an account screen

</td>
</tr>
</table>

## Getting started

1. Grab the latest build from [Releases](https://github.com/actuallyssstaris/ProjectCadence/releases) and install it on a jailbroken PS5 with ShadowMountPlus.
2. Launch **ProjectCadence** from the home screen.
3. The setup runs on first launch: pick a skin, get a few maps, choose the look. You can run it again from settings.

Everything lives in `/data/cadence` on the console.

## Controls

| In a map | |
|---|---|
| Left stick | aim, like a tablet area (full tilt reaches the edge) |
| Right stick | nudge the cursor |
| Touchpad | touch to aim, press to click |
| R1 / R2 / Cross | key 1 |
| L1 / L2 / Square | key 2 |
| Options | pause |
| Triangle | skip intros and breaks |

| Song select | |
|---|---|
| Left stick | scroll through sets |
| D-pad up / down | pick the difficulty |
| Right stick left / right | change mode |
| Right stick down | random set |
| Cross | play |
| Square | mods |
| Triangle | get maps |
| L1 | search |
| R1 | change sort |
| R2 | watch your best replay |
| L2 | osu.ppy.sh leaderboard |
| Options | settings |
| Touchpad | skins |

On the main menu, L1 and R1 change the mode too.

## Maps

**Get maps** searches [catboy.best](https://catboy.best), a public mirror of
osu.ppy.sh, and downloads sets straight to the console. Filter by mode with L2 and
by status (ranked, loved, graveyard...) with R2.

Or copy .osz files over FTP: they're zips, so extract each one into
`/data/cadence/Songs/<folder>/` and rescan from settings.

Star ratings use lazer's difficulty calculator. They're worked out on the console in
the background and cached in `stars.txt`; scores go in `scores.txt`.

## Skins

Nothing skin-related ships with this repo. The picker can download osu!'s classic
default (from ppy/osu-resources, CC BY-NC 4.0), skins their authors publish on
GitHub, or any community skin listed by [o!rdr](https://ordr.issou.best).

Any other osu! skin works as well: unzip the .osk into `/data/cadence/skins/<name>/`.
Circles, numbers, cursor, slider parts, spinner, hit popups, score and combo digits,
health bar, ranking letters, combo colours and hitsounds all come from the skin.

## Online leaderboards

Read only. Make an OAuth application in your osu! account settings (any callback
URL), then put its client id and secret on two lines in `/data/cadence/osuapi.txt`.

## Building

Needs WSL (Ubuntu 24.04) and a jailbroken console with ShadowMountPlus.

```sh
tools/fetch-deps.sh          # PS5_Vulkan, RADV, SharpProspero
tools/setup-dotnet-wsl.sh    # .NET 10 + pwsh
./build.sh
PS5_IP=<console> tools/deploy.sh
```

The build ends up in `dist/PPSA70727`. Logs go to `/data/cadence/cadence.log`.

<details>
<summary><b>What's where</b></summary>

- `core/` - C# gameplay: .osu decoder, slider paths, stacking, judging, scoring,
  health, converts. Ported from ppy/osu (MIT). NativeAOT compiles it and it's linked
  straight into the eboot.
- `host/` - the C side: Vulkan renderer on PS5_Vulkan's RADV, DualSense input,
  audio, and every screen.
- `tests/check` - desktop autoplay check over a folder of maps:
  `dotnet run -c Release -- <maps dir>`
- `tools/` - build helpers, FTP deploy, packaging, a test map generator, klog helpers.

</details>

## Credits

- [ppy/osu](https://github.com/ppy/osu) - the gameplay this is ported from (MIT)
- [PS5_Vulkan](https://github.com/Mihawk-99/PS5_Vulkan) - RADV for PS5, toolchain,
  ELF converter and the clean-room libc.prx (GPL-3.0)
- [SharpProspero](https://github.com/SvenGDK/SharpProspero) - NativeAOT for PS5 (GPL-3.0)
- dr_mp3 / dr_wav, stb_vorbis / stb_image, font8x8 - see [`NOTICE`](NOTICE)

<div align="center">

<sub>Not affiliated with ppy. osu! is a trademark of ppy Pty Ltd.<br>
GPL-3.0-or-later, see <a href="LICENSE">LICENSE</a>. Code ported from osu! keeps its MIT notice in <a href="NOTICE">NOTICE</a>.</sub>

</div>
