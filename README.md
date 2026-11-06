<h1 align="center">Retro Flash Player</h1>

<p align="center">
  <b>A from-scratch Flash Player 10-compatible SWF player for Windows NT 3.1 and later.</b><br>
  Written in strict C89 — no DirectX, no zlib, no C runtime dependency.
</p>

<p align="center">
  <img alt="language" src="https://img.shields.io/badge/language-C89-blue">
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%20NT%203.1%2B%20%7C%2095%2B-5a9fd4">
  <img alt="arch" src="https://img.shields.io/badge/arch-i386-lightgrey">
  <img alt="version" src="https://img.shields.io/badge/version-1.0-brightgreen">
  <img alt="author" src="https://img.shields.io/badge/by-ages2001-orange">
</p>

---

Retro Flash Player (source code name FLASHNT) is a standalone SWF player
written from scratch in strict C89.
It aims to play everything Flash Player 10 could play, as faithfully as
possible: ActionScript 1/2 (AVM1), ActionScript 3 (AVM2), vector and
bitmap graphics, text, sound, video, filters, Pixel Bender and a
software Stage3D. A single EXE runs on Windows NT 3.1, 3.5x, 4.0,
Windows 95 and every later Windows; a headless host build runs anywhere
for development and testing.

### Highlights

* **One tiny EXE, no dependencies** — imports only KERNEL32, USER32, GDI32,
  WINMM, COMDLG32 and WSOCK32, all present since NT 3.1.
* **AVM1 + AVM2** — full ActionScript 1/2/3 with a verifier, GC and the
  `flash.*` API.
* **Everything in software** — vector rasterizer, all filters and blend
  modes, bitmap/JPEG/PNG/JPEG-XR decoders, MP3/ADPCM/AAC audio, and video
  (Sorenson, VP6, H.264).
* **Runs the real games** — Super Mario 63, Run 3, Bloons TD, The Binding
  of Isaac and many more, tested into gameplay (see below).
* **Proven against Ruffle & avmplus** — thousands of conformance tests run
  as regressions on every change.

There is no DirectX, no msvcrt.dll and no zlib. The NT build links
against its own tiny C runtime and imports only KERNEL32, USER32, GDI32,
WINMM, COMDLG32 and WSOCK32 (all present in NT 3.1), with PE subsystem
version 3.10 and i386 code (no CMOV), tuned for current processors
(`-mtune=generic`).

The renderer's hot loops (the scanline fill and the blur) are in the EXE
twice, as x87 and as SSE2 code, and the player picks one at startup:

* SSE2 is used when the CPU has it and the OS runs SSE instructions:
  Windows 98, 2000 and later do this themselves.
* On Windows NT 3.1/3.5x/4.0 and Windows 95, SSE only works when a driver
  such as WINSIMD has enabled it. These systems do not save the SSE
  registers on a task switch, so the player then keeps itself on one CPU.
* Otherwise, or with the environment variable `RETROFP_NOSSE` set, the
  x87 code is used. *Help > About* shows which one is running.

```
dist/RETROFP.EXE     Windows NT 3.1+ / Windows 95+ player (i386)
flashnt-host         headless host build: renders frames to PNG, prints trace()
```

## What it plays

* **SWF 1–10+**, uncompressed, zlib (`CWS`) and LZMA (`ZWS`).
* **AVM1** (ActionScript 1/2): every opcode, `DefineFunction2`
  registers, closures, `with`, `try/catch/finally`, AS2 classes,
  getters and setters, clip events, the complete MovieClip, Button,
  TextField, Sound, XML, LoadVars, SharedObject, BitmapData, filters,
  geom, Key/Mouse/Stage/Selection, LocalConnection, NetStream,
  FileReference and ExternalInterface APIs.
* **AVM2** (ActionScript 3):
  * An ABC interpreter with a data-flow verifier, a mark-and-sweep GC
    and ApplicationDomains.
  * E4X (XML/XMLList), RegExp, JSON, Vector, Dictionary, Proxy, and
    ByteArray with AMF0/AMF3 and the zlib/deflate/LZMA codecs.
  * The `flash.*` API: display list and timelines, events, text and the
    FTE text engine, net, media, filters and geom (including 3D),
    system and ui, Workers (single-threaded) and the
    `avm2.intrinsics.memory` ops used by Alchemy/CrossBridge.
  * Mixed movies: AVM1 content loaded into AVM2 through Loader.
* **Graphics**:
  * DefineShape 1–4, linear, radial and focal gradients, bitmap fills,
    LINESTYLE2 strokes (joins, caps, miter limits, scaling modes), morph
    shapes, masks, scrollRect and cacheAsBitmap.
  * All blend modes and all filters (blur, glow, drop shadow, bevel,
    gradient glow/bevel, convolution, colour matrix, displacement map).
  * A software rasterizer with Flash's anti-aliasing and the Low/Medium/High
    quality settings.
* **Bitmaps**: lossless 8/15/24/32-bit, baseline and progressive JPEG
  (JPEGTables, JPEG3/4 alpha and deblocking), PNG, GIF, JPEG XR and
  ATF textures, and the complete
  BitmapData API with Flash's premultiplied arithmetic.
* **Text**: embedded fonts (DefineFont 1–4), static and edit text, HTML
  text, style sheets, input fields with keyboard and IME, selection and
  clipboard. Built-in device fonts are Liberation Sans/Serif/Mono.
* **Sound**: event and stream sounds, PCM, ADPCM, MP3 and AAC, SoundTransform
  and dynamic `SampleDataEvent` sound, played through a 44.1 kHz software
  mixer (waveOut on Windows).
* **Video**: embedded DefineVideoStream and progressive FLV through
  NetStream. Codecs: Sorenson Spark (H.263), Screen Video 1/2,
  On2 VP6/VP6A (with alpha) and H.264 (CAVLC and CABAC).
* **Pixel Bender** shaders (`Shader`, `ShaderFilter`, blend shaders,
  `ShaderJob`) on an interpreter for PBJ bytecode.
* **Stage3D** in software: Context3D, AGAL validation and
  interpretation, textures, render-to-texture.

## Compatibility status

The Ruffle and avmplus test suites are run as regressions on every
change. Results at the time of writing:

| suite | passing |
|---|---|
| avmplus (Tamarin) acceptance tests (Ruffle's `from_avmplus` set) | 1574 / 1574 |
| Ruffle AVM2 conformance tests | 1265 / 1265 |
| Ruffle AVM1 conformance tests | 747 / 748 |
| Ruffle image comparison tests | 559 / 585 |
| Ruffle `from_gnash` tests (whole movies / subtests) | 338 / 338, 200 / 208 |
| Ruffle `from_shumway` tests | 227 / 229 |
| Ruffle `visual` tests | 148 / 149 |
| audio, fonts, text, timeline, mixed AVM, swf, import_assets, Stage3D | all (71 / 71) |

Some of these pass only through test-harness options (environment
variables) that the player never sets on its own:

* `for-in` over small script objects follows avmplus's hash-table order.
  avmplus hashes strings by their memory address, so the player uses a
  stand-in string hash whose seed was chosen to reproduce the two
  avmplus tests; other movies get a plausible order, not Flash's exact one.
* `FLASHNT_NULL_AUDIO` reports a sound position of 0, as Ruffle's
  silent test backend does.
* `FLASHNT_AVM1_RUFFLE_SHAPES` lays out the built-in AVM1 classes the way
  Ruffle declares them (the default layout is Flash Player's).
* The `from_gnash` suite runs with the Linux Flash Player's device-font
  metrics and AVM1 object model, because gnash recorded its expectations
  with that player.

The remaining failures:

* `BitmapData.pixelDissolve` (AVM1): Flash's pixel order is undocumented
  and cannot be recovered from the test's few samples.
* gnash subtests that depend on the Linux player reading past the end of a
  string, on how many seconds the original player ran, or on its exact
  gradient rasterizer and JPEG decoder.
* image tests whose reference images come from Flash Player itself
  (its anti-aliasing, Windows Arial/Times device fonts) rather than from
  Ruffle, plus DefineFont4/TLF text.

Real games and movies that were tested to their menus, and most of them
into gameplay, include:

* Super Mario 63
* Super Mario Bros. Crossover
* Run 3
* Bloons Tower Defense 4 and 5
* QWOP, Canabalt and Learn to Fly 1/2
* The Fancy Pants Adventure World 3
* The Binding of Isaac
* Age of War 2
* Stick War
* GemCraft, Cursed Treasure and Desktop Tower Defense
* Crush the Castle, Bowmaster and Gun Mayhem
* Madness: Project Nexus and The Last Stand
* Sonic RPG episodes 1–10
* Final Fantasy Sonic X1–X6
* Mega Man X Virus Mission 1/2
* Super Mario RPG (the Flash animation series)
* Dino Run, Earn to Die, Raft Wars and Motherload
* The Impossible Quiz and Papa's Pizzeria
* Super Smash Flash, Strike Force Heroes, Xiao Xiao 9, Madness
  Interactive, Storm the House 2 and Ultimate Flash Sonic

## Using it on Windows

```
RETROFP.EXE C:\GAMES\MOVIE.SWF
```

Without an argument, the player opens a Flash Player 10-style *Open*
dialog. The menus mirror the Flash Player 10 standalone projector:

* **File**: Open, Close, Exit.
* **View**: Magnification, Quality, Full Screen.
* **Control**: Play, Rewind, Loop, Step Forward/Backward.
* **Help**: help and About.

Right-clicking the stage opens the usual context menu. `trace()` output
goes to `retrofp.log` in the current folder, along with a line naming the
exception and address if the player ever crashes. SharedObjects are saved next to
the EXE as `.sav` files.

## Building

Host build (any gcc or clang):

```
make                                   # -> flashnt-host
./flashnt-host game.swf -n 600 -e 50 -o out/
```

Windows NT 3.1+ target (cross-compiled from Linux):

```
apt install gcc-mingw-w64-i686
./build/mingw-nt31.sh                  # -> dist/RETROFP.EXE
```

`build/nt31.mak` builds the same sources with Visual C++ 2.0/4.x.

The built-in ActionScript 3 library (classes written in AS3, many of them
taken from Ruffle) is compiled into `src/avm2_builtin.c`:

```
RUFFLE=/path/to/ruffle ./as3/build.sh
```

## The host driver

`flashnt-host` runs a movie headlessly and is how all of the testing is
done:

```
./flashnt-host movie.swf -n 1300 -e 100 -o out/ \
    -c 419,287@30     # click at stage pixel (419,287) on tick 30
    -k 90@1700        # press key code 90 (Z) on tick 1700
    -i input.txt      # scripted input (keys, mouse, text, IME, clipboard)
    -L                # log URL fetches
```

Useful environment variables:

* `FLASHNT_DEBUG=1|2|3` logs gotos and loads, then AVM2 throws, then
  stack traces.
* `FLASHNT_A1CALLS=1` logs AVM1 calls.
* `FLASHNT_TRACEOPS=<method>` traces AVM2 opcodes;
  `FLASHNT_TRACESELF=<class>` limits that to one receiver class.
* `FLASHNT_TREE=1` dumps the display list.
* `FLASHNT_GCDBG=1|2` prints GC statistics.

The tools in `tools/` help with debugging:

* `abcdump.py` and `swfdump.py` disassemble ABC and SWF files.
* `disasm.py` disassembles AVM1 code.
* `regress.sh` runs the test suites against saved baselines.

## Source layout

| files | role |
|---|---|
| `swf.c` `reader.c` `inflate.c` `lzma.c` `deflate.c` | SWF container, bit reader, decompressors |
| `shape.c` `raster.c` `render.c` `render_filters.c` `filters.c` | vector shapes, rasterizer, filters and blend modes |
| `image.c` `jpeg.c` `jxr*.c` `bmd.c` | bitmap decoders, BitmapData storage and operations |
| `text.c` `textfield.c` `richtext.c` `devfont.c` | fonts, text layout, HTML text, device fonts |
| `player.c` `dobj_geom.c` `button.c` `morph.c` `video.c` | display list, timelines, hit testing, geometry |
| `sound.c` `sound_codec.c` `mp3.c` `aac.c` | mixer and audio codecs |
| `vdec.c` `h263.c` `vp6.c` `h264.c` `netstream.c` | video decoders and FLV playback |
| `avm1_*.c` | ActionScript 1/2 virtual machine and its classes |
| `abc.c` `avm2*.c` | ActionScript 3 virtual machine, verifier and `flash.*` natives |
| `pbj.c` `stage3d.c` | Pixel Bender interpreter and software Stage3D |
| `plat_win32.c` `ntcrt.c` `ntmath*.c` | Windows front end, the freestanding C runtime and its libm |
| `plat_host.c` | headless driver |

## Known gaps

* DefineScalingGrid (9-slice) scaling.
* Nellymoser and Speex audio.
* Real concurrency for Workers.
* Network loads: URLs are resolved to local files next to the movie, with
  no HTTP downloads and no RTMP.

## Credits and third-party code

* **[Ruffle](https://ruffle.rs)** — the Flash Player emulator written in
  Rust — was the main reference for this project. Retro Flash Player:
  * follows Ruffle's behaviour and test expectations throughout;
  * ports several of its algorithms (the text and focus models, the
    display list and frame lifecycle, BitmapData operations, number
    formatting);
  * compiles many ActionScript 3 classes from Ruffle's built-in library
    into `src/avm2_builtin.c`;
  * uses Ruffle's test suite as its conformance suite.

  Ruffle is © Ruffle LLC and contributors, dual-licensed under MIT and
  Apache-2.0; see `LICENSE-RUFFLE.txt`.
* **[NihAV](https://nihav.org) by Kostya Shishkov** — `src/vp6.c` is a C
  port of NihAV's On2 VP6 decoder (`nihav-duck`), the same decoder Ruffle
  uses. MIT license, © 2021 Kostya Shishkov; the full notice is at the
  top of `src/vp6.c`.
* **avmplus (Tamarin)** by Adobe/Mozilla — the AVM2 semantics, the error
  messages, `describeType` and the ByteArray behaviour follow it, and its
  acceptance tests are part of the regression suite.
* **jxrlib** by Microsoft (BSD) — `src/jxr*.c` is a C port of the JPEG XR
  reference decoder; see `LICENSE-JXRLIB.txt`.
* **OpenH264** by Cisco (BSD) — `src/h264.c` reproduces its output order
  and uses tables from it; see `LICENSE-OPENH264.txt`.
* **minimp3** by lieff (CC0) decodes MP3 (`src/thirdparty/minimp3.h`).
* **Liberation fonts** (SIL OFL 1.1) supply the built-in device fonts;
  see `LICENSE-FONTS.txt`.
* **CORE-MATH** (Paul Zimmermann, Tom Hubrecht, Alexei Sibidanov; MIT)
  and **ARM optimized-routines** via **musl** (MIT) — the NT build's own
  libm (`src/ntmath*.c`): correctly rounded sin/cos/tan/asin/acos/atan/atan2
  and glibc's exp/log/pow algorithms; see `LICENSE-NTMATH.txt`.

Flash, Flash Player and ActionScript are trademarks of Adobe. Retro Flash Player is
an independent implementation and contains no Adobe code.
