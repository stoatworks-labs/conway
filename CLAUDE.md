# conway

Conway's Game of Life for Resolume Arena/Avenue, as two FFGL plugins from one
core: `SW Conway` (`LF01`, source: a soup or a famous pattern) and `SW Conway
Over` (`LF02`, effect: the clip seeds and feeds the field). C++/GLSL, CMake
MODULE → two universal `.bundle`s (macOS) + Windows `.dll`s. MIT. Bundle ids
`com.stoatworks.ffgl.conway` and `com.stoatworks.ffgl.conway.over`.

Read `AGENTS.md` before touching the state's bit layout, the step shader, the
rule table (`Rules.h`), the settle law (`Settle.h`) or the parameter tables.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (never from `~/Projects`)
- Render the source: `./build/cwtest --out /tmp/life.png --frames 600`
- Render the Over effect on the harness's card: `./build/cwtest --over --out /tmp/o.png`
- A frame of a real clip through it: `ffmpeg -i clip.mov -frames:v 1 -s 1280x720 -f rawvideo -pix_fmt rgba /tmp/c.rgba && ./build/cwtest --over --clip /tmp/c.rgba --out /tmp/o.png`
- List parameters: `./build/cwtest --list` (`--over` for the effect's)
- Set anything by name: `./build/cwtest --set "Rule=2" --set "Cell Size=0.7"` (options by element index)
- The source as video: `./build/cwtest --pipe --frames 1800 --size 1280x720 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through the effect: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba -s WxH - | ./build/cwtest --over --pipe --size WxH | ffmpeg …`
  (Resolume's demo clips are 30 fps; the harness clocks every frame at 1/60 s, so
  resample with `fps=60` or the automaton runs at half speed.)
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`. Held before the first key and after the last; a standard
  control is linear between keys, and an option, boolean, event or integer
  STEPS (holds each key until the next key's frame), so a press is three keys
  (0, 1, 0). Frame *n* is clocked at n / `--fps` (60). An unknown name exits 2
  before any frame; a partial frame at EOF ends the stream with exit 0; a reader
  that hangs up ends `--pipe`/`--film` with exit 1 (SIGPIPE is ignored), never a
  silent 141. The source's `--pipe` with no `--frames` runs until the reader
  hangs up, so it only ever ends with exit 1. The source's frames are written
  over black (it is premultiplied, transparent where nothing lives).
- A draft of the fleet gate's expectation from what the plugins declare:
  `./build/cwtest --expect` (a draft only: its `note` fields are not the gate's).

## Verify
- Everything: `tools/verify.sh` (~2 min: reserved words, no unbounded trig in the
  GLSL, glslc, fresh universal build, both bundles through
  lipo/plist/codesign/oxbow probe+selftest, every check at 1280x720 and 320x180,
  the same checks on Apple's software renderer at 320x180, the offline set, the
  `--pipe` contract, the negative controls, the mutants, the sweep, the bench)
- **The literature**: `--literature` (no GL: the harness's plane), `--patterns`
  (the plugin), `--rules` (the family's laws), `--reference` (bit for bit).
- **The machinery**: `--count` (the occlusion query), `--settle` (Auto Reseed),
  `--clock` (Resolume's 499 million ms), `--coverage` (the box filter),
  `--over-check`, `--prime`, `--resize`, `--state`; no GL: `--settle-law`,
  `--clock-law`, `--cues`, `--names`.
- One raster only: add `--size WxH`. The software renderer:
  `CWTEST_RENDERER=software ./build/cwtest --rules --size 320x180`
  (`CWTEST_HEAVY=1` also runs the two 640x640 methuselahs it skips there).
- **The checks can fail**: `--negative` (14 wrong models), `tools/mutate.sh`
  (one character of GLSL, Clock.h and Settle.h).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (41 parameters over both plugins).
- Cost: `--bench` (720p/1080p/4K, both plugins, and 1080 rows).

## Notes
- **The state is one R32UI texel per cell**: low 16 bits the AGE (0 = dead),
  high 16 the generations SINCE death (0 = never alive), both saturating.
  Every pass that reads it is `texelFetch` and integer arithmetic: bit-exact
  on any GLSL 4.10 implementation.
- **The grid follows Cell Size and the raster's ASPECT, not its size.** A
  re-grid copies the overlap, centred, into new buffers allocated before
  anything is bound; the old pair is freed after.
- **Rules are written once, as notation** (`Rules.h`), parsed into masks. A
  Generations rule's C sets the refractory generations (C − 2).
- **Probabilities are integer thresholds** (`ThresholdU32`): a PCG hash below
  p × 2³² fires, on both sides, never a float compare.
- **The population is an occlusion query** (`GL_SAMPLES_PASSED` round a pass
  that discards dead cells, colour writes off), read when ready, never waited
  for outside the harness. Queries from an older field (a reseed, a load, a
  re-grid) are dropped by epoch.
- **Host indices are not ParamIds.** Each plugin declares its own dense list
  (`HostOrder()`), About block LAST in both (radar's shape).
- Every host parameter is 0..1 except Seed and Audio Steps (real integers).
  An option reads back 0..1 whatever its count: map by element index.
- No sin/cos/atan in the GLSL (verify.sh greps): the hue wheel is clamped
  triangles. Randomness is PCG integer hashing, never `fract(sin(...))`.
- GLSL reserved words must not be identifiers (including `packed`, `smooth`);
  verify.sh greps for the 4.10 list. No `M_PI`, no `far`/`near`.
- `conway_core` is an OBJECT library: the registrations are file-scope
  constructors nothing references.
- Public at `github.com/stoatworks-labs/conway`, registered on the website
  (2026-10-04).

## Not done yet
- Never loaded into Resolume on macOS (oxbow selftest only). No OpenFX port.
- `StoatworksAbout.h`, `ATTRIBUTIONS.md`, `.github/FUNDING.yml` and
  `.github/ISSUE_TEMPLATE/` are GENERATED by the backend's sync scripts; the
  user guide is `docs/USER-GUIDE.md` (the PDF and the site page are generated
  from it by the website's `build_guides.py`). The User guide button sits
  inside the About block, which is last, so no control moved: 24 and 29
  parameters.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which shader failed to compile and the GL
vendor/renderer.

    ~/Library/Logs/conway/conway.YYYY-MM-DD.log
