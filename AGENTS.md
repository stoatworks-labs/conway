# conway — for agents

The why behind the code. `CLAUDE.md` has the commands; this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-10-04 in one session, from Allan's one-line request ("a new
resolume plugin that plays conways game of life"). The spec is
`~/Projects/resolume/specs/SPEC-conway.md`; the briefs are
`~/Projects/resolume/specs/BRIEF.md` and `BRIEF-ADDENDUM.md`.

## The one idea

**A Life-like cellular automaton is a grid of cells, alive or dead, all
updated at once by one rule over the eight neighbours.** Conway's is B3/S23.
Nothing is drawn as a picture of Life: the field is an integer texture, a
generation is one pass, and the gliders, oscillators, guns, methuselahs and
the soup settling into ash fall out of the rule. The rest of the family
(twelve more rules) is the same pass with different masks.

The house-style claim is that the plugin IS Life, so the harness asks it for
the published facts (the R-pentomino's 116 cells at 1103, Diehard gone at
130, the gun's period 30, the oscillators' periods, the spaceships' speeds)
and for the family's laws (Day & Night's and Anneal's complement symmetry,
the Replicator's copies, Seeds' and Brian's Brain's no-survivors), and holds
every rule to an independent stepper bit for bit.

## How a frame goes

1. **Time** (`Conway.cpp`, `Clock.h`): the host's elapsed time, frame to frame,
   in double (the unit voted against the wall clock, rosette's code via
   radar); a jump (backwards, or over 1 s) passes no time and resets the audio
   analyser. Speed x dt is owed; whole generations run, the fraction carries,
   at most 32 a frame (the rest dropped, not banked).
2. **Counts**: the populations whose queries have come back go to the settle
   detector, which may ask for a reseed.
3. **Audio**: an onset owes Audio Steps generations and drops a patch of soup
   (Audio Seeds) at a hashed place.
4. **Presses**: Step owes one generation; Seed changes restart its sequence of
   soups; Reseed takes the next one; a Pattern change reseeds.
5. **The grid**: rows from Cell Size, columns from the raster's ASPECT. If the
   grid changed, the old buffers move aside, the new pair is allocated, the
   overlap is copied in centred, the old freed -- all before any pass binds.
6. **Seed, load or carry**: a reseed runs the seed pass into both buffers
   (soup, a stamped pattern, a glider fleet, or the clip); the harness's load
   uploads a state.
7. **Steps**: each generation is the step pass (current → the other buffer,
   swapped, so `previous` is the generation before), then the count pass
   under a `GL_SAMPLES_PASSED` query.
8. **Composite**: each output pixel box-filters the grid over its footprint,
   cells coloured by age and trail, `previous` crossfaded into `current` by
   Smooth's phase; the Over mixes the clip.

### The state

One R32UI texel per cell: **low 16 bits the age** (generations alive, 0 =
dead), **high 16 the generations since death** (0 = never alive). Both
saturate at 65535. The age is the look (newborn → elder); the time since death
is the trail AND the Generations rules' refractory period: a cell is born only
if it has never lived or died more than C − 2 generations ago. So Brian's
Brain (B2/S/C3) and Star Wars (B2/S345/C4) cost one comparison, and a
Life-like rule (C = 2, refractory 0) is unaffected.

Integer state and `texelFetch` everywhere the automaton is touched: GLSL 4.10
makes integer arithmetic exact, so the automaton is the same on every GPU and
on the software renderer. Floats appear only in the composite.

### The rules

Each rule is written once, as its notation (`Rules.h`), and parsed into the
birth and survive masks the shader uses. A typo is then a typo in a published
rule string, which the behavioural checks find (`--rules`, `--literature`)
rather than a second copy of a mask. `--literature` also holds the parser:
seven malformed notations are refused.

### The count and Auto Reseed

A pass over the new generation discards every dead cell, with colour writes
off, inside a `GL_SAMPLES_PASSED` query. With no multisampling a fragment is a
sample, so the result is the population exactly (GL 4.1 §4.1.7; the inexact
query is `_CONSERVATIVE`, 4.3). Results are polled with
`GL_QUERY_RESULT_AVAILABLE` at the next frame, never waited for, and tagged
with an epoch so a reseeded, loaded or re-gridded field's old counts are
dropped. `SettleDetector` (`Settle.h`) calls the field settled when the last
120 counts each equal the count p generations earlier for some p <= 30, or
after 8 empty generations. Ash on a torus repeats with a period dividing
lcm( 2, 3, 15 ) = 30: blinkers and beacons, pulsars, pentadecathlons; gliders
are five cells in every phase.

### The composite

The grid is box-filtered over each pixel's footprint: every cell the pixel
overlaps (at most 12 per axis) weighted by the exact area of overlap with the
cell's lit square (inset Gap/2 each side). So a whole-pixel cell is crisp, a
fractional one is evenly antialiased instead of alternating 7- and 8-pixel
columns, a cell smaller than a pixel averages, and the integral of the
picture is the lit area -- which `--coverage` measures per pixel.

A live cell's colour runs from Newborn to Elder over Age Span generations
(Spectrum: a hue that turns with age, clamped triangles, no trig); a dead
cell glows in the trail colour at exp( −since / Trail ), its alpha the same.
The source is premultiplied and transparent where nothing lives. The Over:
`mix( clip, life + clip x Backdrop x ( 1 − life alpha ), Mix )`, alpha
`mix( clip alpha, 1, Mix )`; the clip is read with `texelFetch`, so Mix 0 is
the clip bit for bit.

### The Over

The clip decides a cell by its texel holding the cell's centre, found in
INTEGER arithmetic ( ( 2c + 1 ) x raster / ( 2 grid ) ), so which pixel
decides a cell is exact on any GPU; the cell is bright if the brightest
channel x alpha exceeds Threshold (radar's lesson: luma drops a saturated
blue). Pattern *Clip* (the Over's default) seeds from it; Feed, every
generation, rebirths each bright cell with probability Feed²
(squared so the slider's low end has room).

## Decisions taken without asking

- **Name and ids**: `conway`, `SW Conway` / `SW Conway Over` (9 and 14 bytes),
  `LF01` / `LF02` (Life; checked free against every `CFFGLPluginInfo` in the
  fleet and `~/dev` on 2026-10-04). "Conway" is what people search for; the
  Life-like family is in the Rule dropdown.
- **A source and an Over effect** (radar's, boreal's and flyback's shape), each
  declaring only its own controls, About block last in both, so a future User
  guide button moves no control.
- **The grid is raster-independent** (rows from Cell Size, columns from the
  aspect), so a composition resize or a thumbnail at another size runs the
  same field, and the resize trap (a reallocation clearing the state) cannot
  arise for a same-shape resize at all.
- **Cell Size is geometric in rows**, 1080 (a cell per pixel at 1080p) to 16;
  the default is 135 rows (8 px at 1080p). Not an integer pixel size, so the
  look is the same at any raster; the box filter keeps fractional cells even.
- **Defaults**: Conway on a torus, a 35% soup, 15 generations a second, Smooth
  0.35, Gap 0.12, Phosphor, Age Span 40, Trail 3, Auto Reseed on. The Over:
  Pattern *Clip*, Threshold 0.5, Feed 0.3 (p = 0.09), Backdrop 0.35. Surveyed on
  six of Resolume's bundled clips (Trinity_09, BattleWeapon_Tank_09,
  IntoTheGlow_02, OrganicMotions_06, Beat 003, Galactucity_21) through `--pipe`
  at 60 fps: Life grows out of the rings, the tank's lit edges, the rocks and
  the dancers. Beat 003 (mid grey: 0.5% of it over 0.3) seeds almost nothing,
  IntoTheGlow_02 (34% over 0.5, 84% over 0.25) fills the field; no single
  threshold suits both, so 0.5 stands and the docs say to move it.
- **Twelve rules beyond Conway**, by their usual names; credits checked on
  Wikipedia 2026-10-04 (the LifeWiki returns 403 to automated reads).
- **Patterns**: Soup, R-pentomino, Acorn, Diehard, Gosper Gun, Gliders (one
  per 16 x 16 block with probability Density, its heading from two hash bits),
  and Clip in the Over, LAST, so shared patterns keep their indices.
- **Probabilities are integer thresholds** compared with a PCG hash on both
  sides; the harness's soup and the plugin's are the same integers.
- **Smooth's phase** is the time since the last generation over Smooth x the
  generation period (1/Speed; 0.25 s when paused); the clock's fraction gives
  the exact time since a clock-driven generation.
- **Commit trailer** is this session's model, `Claude Opus 5.5`, not the
  brief's `Claude Fable 5.1` (the brief was written for Fable sub-agents).

## The traps

**The sweep found a real bug: a Seed change swallowed a Reseed press in the
same frame.** The buttons were read Reseed first (serial + 1), then Seed
(a change resets the serial to 0). The first frame always reads as a Seed
change, so `Reseed=1` held from frame 0 did nothing, and in a host a Seed edit
and a press in one frame would lose the press. Now Seed is read first.

**A float clock's elapsed time telescopes.** `--clock`'s first negative
control (elapsed time from the host clock as a float) passed against the
generation count alone: the per-frame dts are 0 or 32 ms, but they sum to the
right elapsed time to ±32 ms, and floor( Speed x elapsed ) was right at all
ten checkpoints. The check now also bounds every frame's dt (1e-9 s), which
the float clock misses by 0.017 s. Radar's antenna caught it by its rate; a
counter needs the dt.

**The software renderer is slow at 640 x 640.** `--patterns` took 163 s and
`--settle` 123 s there, nearly all of it the two R-pentomino runs (1200 and
1000 generations of 409,600 cells, a count pass each). The software pass now
skips those two, printing a `skip` line (`CWTEST_HEAVY=1` runs them); the
rest of the pass is ~80 s. They repeat passes `--reference` and `--count`
hold bit for bit there.

**`mutate.sh` splits its table on `|`**, so a mutant whose text contains a
`|` (the first try was `& 1u` → `| 1u`) is cut in two and refused as "not one
character". It is `^` now (the survival test inverted).

**The colour mask is state.** The count pass turns colour writes off; a host
that found them off would draw nothing. `GLState.h` now captures and restores
`GL_COLOR_WRITEMASK`, and `--state` checks it.

**The demo clips are 30 fps**; `--pipe` clocks each frame at 1/60 s, so a clip
piped in raw runs the automaton at half speed against its picture. Resample
with `fps=60`.

**`clamp( x, lo, hi )` is undefined for lo > hi** in GLSL; the composite's
cell range clamps the far corner to `min( grid − 1, c0 + span − 1 )`, which
is never below `c0`.

Carried from the fleet and respected here: integer formats need an
integer-aware PassBuffer (stencil's); allocate before binding; units bound by
hand and released (`unbindTextureUnits`); a sampler on texture 0 is
"unloadable" (the source binds a 1x1 blank to its clip samplers, and samplers
of different types never share a unit); `SetTextParameter` for About; OBJECT
library; integer hashing only; the clock-unit vote and double time; primed
onsets; no `M_PI`, `<cmath>` included, no `far`/`near`; `packed`, `smooth` and
the 4.10 reserved-word list; an option reads back 0..1 whatever its count;
unique names as Arena addresses them; output alpha `mix( clip, 1, Mix )`.

## Would this hold on another rasteriser, at another raster?

Every check runs at 1280x720 and at 320x180 on this Mac's GPU, and at 320x180
on Apple's software renderer (`verify.sh`). CI runs only `--offline` and
glslc: a macOS runner has no accelerated GL.

| check | bound | why that number | raster / rasteriser |
| --- | --- | --- | --- |
| `--literature` | exact | integer counts on the harness's plane | CPU |
| `--patterns` | exact | integer state, `texelFetch`, integer arithmetic: GLSL 4.10 makes it exact | the grid is set by the test, so raster-free; both rasterisers (the 640x640 run GPU only, see the traps) |
| `--rules` | exact (0 cells) | the same | both rasterisers |
| `--reference` | bit for bit, ages included | the same | both rasterisers |
| `--count` | exact | a fragment per live cell, no multisampling: GL 4.1 §4.1.7 counts samples exactly | both rasterisers |
| `--settle` | the exact generation | integer counts through the stated law | both rasterisers (the 640x640 run GPU only) |
| `--settle-law`, `--clock-law` | exact | integer counts; the clock in double, and checkpoints within 1e-6 of a whole generation are not scored (6 and 3 of 36,000) | CPU |
| `--clock` | dt within 1e-9 s; the count exact | a double ulp at 4.99e8 ms is 6e-8 ms; measured 6.2e-11 s | raster-free |
| `--coverage` | 4 x ulp( max( cols, rows ) ) / min( cells per pixel ) + 4 x 2⁻²³ | every footprint edge is a float at up to the grid's size, four edges per pixel, over the pixel's width in cells | measured 2.9e-5–1.6e-4 (GPU), 3.3e-5–3.8e-5 (software, 320x180), each inside its own bound; 0 at half-pixel cells |
| `--over-check` | exact cells; Mix 0 bit for bit | integer pixel mapping; `texelFetch` and `mix( a, b, 0 ) = a` | both rasters, both rasterisers |
| `--prime`, `--resize`, `--state`, `--names`, `--cues` | exact | counts, integer state, GL state | raster-free; `--resize` crosses 1280x720 ↔ 320x180 ↔ 960x720 |

**Resize mid-run.** The grid is not raster-sized, so a same-shape resize does
not reallocate; a new aspect does, and the copy runs before the old buffers
are freed. `--resize` checks both, and its negative control (clear the state
on a resize) fails it.

## A check that cannot fail is not a check

`cwtest --negative` sets one wrong MODEL at a time -- a test hook in the plugin
where the model is the plugin's, so the shipped shader computes the wrong
thing -- and requires the check to fail. All 14 are detected:

| check | the wrong model |
| --- | --- |
| `--literature` | the plane stepped with HighLife's masks for Conway's |
| `--patterns` | the cell counts itself as a neighbour |
| `--rules` | Day & Night and Anneal without B6, the Replicator without B1, Conway for Life without Death and Seeds, Seeds for Brian's Brain |
| `--reference` | the torus wraps one cell short |
| `--count` | the query also counts the just-dead |
| `--settle-law` | the detector looks for a constant population only |
| `--settle` | the plugin's detector looks for a constant population only |
| `--clock-law` | the owed fraction carried in float |
| `--clock` | elapsed time from the host clock in float |
| `--coverage` | point-sample the cell under each pixel's centre |
| `--over-check` | the clip laid under the grid mirrored |
| `--prime` | the analyser unprimed |
| `--resize` | the state cleared on a resize |
| `--cues` | every control ramps between keys |

`--clock-law`'s float model fails one of its four cases (13.37 generations a
second at 60 fps: 2 of 36,000 frames off floor( Speed x elapsed )); the others
happen to land right. It is a weak wrong model, and the check fails, which is
what is asked of it.

### The recorded mutation

`tools/mutate.sh` (run by verify.sh) changes one character of shipped code in a
copy of the tree and requires a named check to fail. **The GLSL mutation of
record**: in the step, `next = ( ( Survive >> n ) & 1u ) != 0u;` became
`( Survive >> n ) ^ 1u` -- the survival test inverted -- and **`--patterns`
caught it** (the block and the boat moved, the blinker, toad and beacon
never came back, both spaceships were lost). Two more GLSL mutants (the torus wrapping the wrong way in
x, caught by `--reference` on every torus rule; the lit square growing past
its cell, caught by `--coverage`) and two C++ (the clock banking the
generations it ran, caught by `--clock-law`; a constant population never
settled, caught by `--settle-law`) are caught too. This proves the harness
drives the shaders the plugin ships, not a copy of them: it has none.

## The browser demo (2026-10-04)

`demo/` is <https://conway-demo.stoatworks-labs.com>, built to the fleet's
`resolume-demo` kit rules (`~/Projects/resolume/specs/DEMO-BRIEF.md`) by a
sub-agent of the release session. What a reader of it must know:

- **The shaders are the plugin's**, all eight pieces plus kVersion, spliced by
  `demo/tools/check_shaders.py --write` into `demo/shaders.js` and compared
  character for character by the same script, which `tools/verify.sh` runs.
  Negative-controlled once: the step's survival test `& 1u` -> `^ 1u` in the
  copy fails it. **Every pass compiles in WebGL2 as spliced**: the kit's
  `port()` adds the ES precision lines (including `usampler2D`'s), and R32UI is
  a colour attachment WebGL2 renders to natively, so the kit's PassBuffer serves
  the state with `filter: 'nearest'` and no integer-texture helper was needed.
- **The CPU half is a port that only a reader checks**: Controls.cpp's
  conversions, `ThresholdU32` and `GridFor`; the rule table and Rules.cpp's
  parser; the RLE parser and the stamp the right way up; Clock.h; the buttons
  (Seed read before Reseed, as the sweep's fix has it); a Pattern change
  reseeding; the centred re-grid copy; the salts (Hash.h's PCG via
  `Math.imul`); Smooth's phase; `lookOf`; Settle.h and the epoch rule for stale
  counts.
- **Compared once with the plugin, and it agreed exactly.** A scratch script
  drove the page headlessly (SwiftShader) and `cwtest --pipe` (this Mac's GPU)
  on the same 240 x 135 grid: generation 0 cell for cell for Soup (11,248
  cells), Acorn, Gosper Gun and Gliders, 0 cells different (a vertically
  flipped page differs in 8 to 14,874, so orientation is tested); the
  population after 1, 10 and 40 generations of Conway, 10 with Noise 0.7, 10 of
  Brian's Brain, 25 of Star Wars with dead edges, 5 after Seed 37 + Reseed, 30
  of a dense glider fleet; and the Over, the page's bars clip piped into
  `cwtest --over`: generation 0 cell for cell (23,064) and 12 generations with
  Feed 0.6. Every number identical. Nothing repeats that comparison; a change
  to the C++ half needs the port changed by hand.
- **The population is a readback, decided without asking.** The plugin counts
  with `GL_SAMPLES_PASSED`; WebGL2 has only `ANY_SAMPLES_PASSED` (a yes or no).
  The page runs the plugin's own count pass with colour writes ON into the R8
  target the plugin allocates (cleared first), `readPixels` it (RED where the
  implementation offers it, else RGBA, `PACK_ALIGNMENT` 1) and counts the marks:
  the same cells, exactly. Counts are queued and handed to the settle law at
  the start of the next frame, as the plugin's polled queries come back.
  Readback costs, so a frame counts at most 2^21 cells (one generation at 1080
  rows, every generation at the default grid); past that a generation is not
  counted, the law's gap rule restarts the history, and Auto Reseed waits. The
  page says all of this. Checked: Diehard at 500 gen/s auto-reseeds at exactly
  generation 137 (gone at 130, then 8 empty counts).
- **Differences, all said on the page:** no audio (Audio, Audio Steps, Audio
  Seeds absent -- with no spectrum the analyser never fires, so nothing
  changes); Step and Reseed are toggles the page releases after one frame;
  Seed is a 0-99 dropdown (FF_TYPE_INTEGER 0-9999); no About block; the clock
  is the page's seconds, the unit vote not ported, Restart a jump (the field
  carries on); the Plugin switch is a new instance at that constructor's
  defaults; Pattern is two rows of the same name (six elements for the source,
  seven for the Over) because the kit's option rows have one fixed list; with
  the page paused, Smooth's crossfade after a Step press stays at its start
  (no frames run); the Over's clip is the kit's premultiplied one.
- **The Over's clips** are `spot` (default: lights on black, which Threshold
  0.5 reads as three seeds), `grid`, `scene`, `bars`, `alpha`.
- **Seen, not a fault:** headless Chrome logs ANGLE's "GPU stall due to
  ReadPixels" performance WARNING a few times per browser process. That is the
  page's own count readback, synchronous by design; it is not an error.
- **The live page logs one console error that is not the page's**: the zone
  injects an inline `/cdn-cgi/challenge-platform` script and the page's
  `script-src 'self'` CSP blocks it, as on every `*-demo` host (fleet-wide,
  recorded in the demo brief's sweep). Locally there are no errors.

## Shape of the code

    source/Controls.*      the ParamIds, HostOrder(), groups, names, every 0..1 -> units mapping
    source/Rules.*         the rule table as notation, and its parser
    source/Patterns.*      RLE, and the four famous seeds
    source/Clock.h         the generation clock (offline-checked)
    source/Settle.h        the settle law (offline-checked)
    source/Hash.h          PCG, the GLSL's twin
    source/Shaders.*       kCommon, kClipCommon + seed, copy, step, count, composite
    source/Conway.*        the plugin: parameters, clock, audio, presses, grid, passes, queries
    source/PassBuffer.*    stencil's integer-capable buffer
    source/SourcePlugin.cpp, EffectPlugin.cpp   the two registrations
    source/Audio.*         the primed analyser
    tools/cwtest/          the harness: the rig, the CPU steppers, the checks
    tools/sweep.py, mutate.sh, verify.sh, glslc.sh
    demo/                  the browser demo: plugin.js (the port), shaders.js (generated), vendor/ (the kit)

## What is genuinely verified, and what is assumed

Verified on this machine (M4 Max, macOS 26), see the README's Status table for
the numbers: every check above on the GPU at two rasters and on the software
renderer at 320x180 (less the two skipped runs); both bundles universal; oxbow
probe reads `SW Conway` / `LF01` / source and `SW Conway Over` / `LF02` /
effect; oxbow selftest renders 120 frames through each; 41 parameters over both
plugins all move the picture.

Assumed, or not done:

- **Never loaded into Resolume**, on either platform; never built on Windows
  (MSVC has not seen it: `<cmath>` is included where used, no `M_PI`, no
  `far`/`near`, but nothing has compiled it). No Arena gate run, so how the 24
  and 29 parameters present in Arena, the clock unit Resolume sends, the FFT
  bins and whether events arrive as 1 then 0 are untested in a host.
- **Occlusion queries in Resolume**: exact by the spec and on both renderers
  here; how many frames late the results arrive inside Resolume is unknown
  (Auto Reseed only lags by them).
- **The literature** was checked against Wikipedia for this build (the
  LifeWiki refused the fetch), and reproduced by the harness's own stepper.
- **The palettes** are chosen by eye; the age and trail laws are a look, not a
  measurement of anything.
- **The bench** was taken while other builds may have shared the machine.

## Open design questions

- Should Auto Reseed also have a time limit (reseed after N seconds whatever
  the count)? A large Noise-free field can take minutes to settle; with Noise
  it never does.
- A **BPM-locked** clock (generations per beat from Resolume's transport) would
  suit a set better than generations per second; Audio Steps covers the beat
  from the FFT instead.
- Hexagonal and larger-than-Moore neighbourhoods (Larger than Life) would be
  new pieces in the step pass, not new rules.
- An Over mode where Life MASKS the clip (cells as windows) is reachable now
  with Clip Colour 1, Backdrop 0; it may deserve its own switch.
