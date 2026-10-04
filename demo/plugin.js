/**
 * Conway — browser demo.
 *
 * Conway's Game of Life, and twelve of its relatives, on an integer texture.
 * The one idea, from `AGENTS.md`: **a grid of cells, alive or dead, all updated
 * at once by one rule over the eight neighbours.** Nothing is drawn as a
 * picture of Life: the field is one R32UI texel per cell (the low 16 bits its
 * age, the high 16 the generations since it died), a generation is one pass,
 * and gliders, oscillators, guns and the soup settling into ash fall out of the
 * rule. SW Conway plays a soup or a famous pattern; SW Conway Over lets the
 * clip seed the field and keep feeding it.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the eight
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced across
 *   by `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character and `tools/verify.sh` runs it. They
 *   are assembled as `Assemble()` assembles them (kVersion + kCommon + the
 *   pieces: kClipCommon first for seed and step) and run as the same passes,
 *   into buffers of the same formats (an R32UI pair, `current` and `previous`,
 *   the grid's size; an R8 count target), with the same uniforms as
 *   `ConwayPlugin::seed`, `step`, `count` and `composite`. Every pass that
 *   touches the automaton is `texelFetch` and integer arithmetic, which GLSL ES
 *   3.00's highp makes exact, as GLSL 4.10 does.
 *
 *   **The CPU half is a port** (`ConwayState` and the functions above it), and
 *   nothing checks it but a reader: Controls.cpp's conversions (Speed, Noise,
 *   Cell Size, Gap, Age Span, Trail, Feed, `ThresholdU32`, `GridFor`), the rule
 *   table parsed from its notation (Rules.cpp), the RLE parser and the stamp
 *   laid the right way up (Patterns.cpp, `seed()`), the generation clock
 *   (Clock.h), the buttons, the centred re-grid, the salts (Hash.h's PCG, via
 *   `Math.imul`), Smooth's phase, the palettes (`lookOf`) and the settle law
 *   (Settle.h). `cwtest --clock-law` and `--settle-law` check the C++ and have
 *   never heard of this page. It was compared ONCE with the plugin through
 *   cwtest (2026-10-04, see AGENTS.md): generation 0 cell for cell and later
 *   populations, all identical. Nothing repeats that comparison.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **The population is a readback, not an occlusion query.** The plugin counts
 * each generation's live cells with GL_SAMPLES_PASSED round its count pass
 * (colour writes off, dead cells discarded). WebGL2 has only
 * ANY_SAMPLES_PASSED, which says whether ANY cell is alive, not how many. So
 * the page runs the plugin's own count pass with colour writes ON into the R8
 * target the plugin allocates for it, cleared first, reads that back with
 * readPixels and counts the marked cells here: the same set of cells (the
 * count fragment decides, `aliveOf`), counted exactly. The counts are handed to
 * the settle law at the start of the next frame, as the plugin's polled queries
 * are when they come back by then. A readback costs, so a frame counts at most
 * `kCountCellsPerFrame` cells' worth; past that a generation goes uncounted,
 * and the law's own rule for a gap (the history starts again) applies, so at
 * the finest Cell Sizes run fast Auto Reseed waits. At the default grid
 * (240 x 135 on a 16:9 page) every generation is counted.
 *
 * **Both plugins, one page.** `SW Conway` (LF01) is a source and `SW Conway
 * Over` (LF02) an effect: one class with a flag, each declaring only its own
 * controls (`HostOrder()`). The Over adds Threshold, Feed, Clip Colour,
 * Backdrop and Mix, and its Pattern has a seventh element, Clip, which is its
 * default. The kit's option rows have one fixed element list, so Pattern is
 * two rows of the same name -- six elements for the source, seven for the Over
 * -- and only the one the chosen plugin declares is shown. The Plugin dropdown
 * is a new instance, as it would be in Resolume, at that constructor's
 * defaults. `?plugin=over` opens the effect; Copy link carries it.
 *
 * **Nothing audio.** `Audio` is an FFT buffer Resolume fills; `Audio Steps` and
 * `Audio Seeds` act on its onsets. A browser has no Resolume FFT, so all three
 * are absent from the panel rather than present and dead. The removal is
 * exact: with no spectrum the analyser never fires, so no onset steps and no
 * patch of soup is ever dropped -- here as in a layer with no audio.
 *
 * **Step and Reseed are toggles the page releases.** They are FF_TYPE_EVENT in
 * the plugin; the kit has none. The page reads the press as the plugin does
 * (the rising edge), then puts the toggle back to 0, which is what a host does
 * with an event button. **Seed is a dropdown**: FF_TYPE_INTEGER 0-9999 in the
 * plugin, 0-99 here (the default, 1, is among them).
 *
 * **The clock.** The kit's clock is seconds accumulated from frame deltas,
 * capped at 0.1 s a frame, paused by Pause, handed to the port as the host's
 * time already in seconds: the plugin's seconds-or-milliseconds vote is not
 * ported, because this host's unit is known. The jump rule is the plugin's: a
 * step backwards or over a second is a jump and no time passes across it, so
 * Restart leaves the field where it was.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

//===========================================================================
// Constants (Controls.h, Conway.cpp)
//===========================================================================

const kMaxRows = 1080;
const kMinRows = 16;
const kMaxStepsPerFrame = 32;
/** Host seconds: a bigger forward step, or any backward one, is a jump. */
const kMaxFrameDelta = 1.0;
/** Smooth's crossfade when nothing sets a period (Speed 0, stepping by hand). */
const kPausedPeriod = 0.25;

/**
 * The page's own limit, not the plugin's: cells read back and counted in one
 * frame. 2^21 is one generation at the finest grid (1080 rows x 1920 columns
 * at 16:9), or every generation of a frame at the default grid.
 */
const kCountCellsPerFrame = 1 << 21;

const f32 = Math.fround;

//===========================================================================
// Controls.cpp, ported: every host 0..1 to the plugin's units.
//
// The plugin's parameters are floats, so a value from the page goes through
// f32 on the way in (`host()` in the renderer) before any of these.
//===========================================================================

const clamp01 = (v) => Math.min(1, Math.max(0, v));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clamp(value, lo, hi) / low) / Math.log(high / low));
}
const kSpeedLow = 0.5;
const kSpeedHigh = 500.0;
const kNoiseLow = 1e-6;
const kNoiseHigh = 1e-2;
const kTrailLow = 0.5;
const kTrailHigh = 200.0;

/** std::lround: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

const SpeedFromParam = (v) => (v <= 0 ? 0.0 : geometric(v, kSpeedLow, kSpeedHigh));
const ParamFromSpeed = (gps) => (gps <= 0 ? 0 : inverseGeometric(gps, kSpeedLow, kSpeedHigh));
const SmoothFromParam = (v) => clamp01(v);
const DensityFromParam = (v) => clamp01(v);
const NoiseFromParam = (v) => (v <= 0 ? 0.0 : geometric(v, kNoiseLow, kNoiseHigh));
const ThresholdFromParam = (v) => clamp01(v);
const FeedFromParam = (v) => clamp01(v) * clamp01(v);
const RowsFromParam = (v) => lround(geometric(v, kMaxRows, kMinRows));
const ParamFromRows = (rows) => inverseGeometric(rows, kMaxRows, kMinRows);
const GapFromParam = (v) => 0.5 * clamp01(v);
const AgeSpanFromParam = (v) => geometric(v, 1.0, 1000.0);
const ParamFromAgeSpan = (g) => inverseGeometric(g, 1.0, 1000.0);
const TrailFromParam = (v) => (v <= 0 ? 0.0 : geometric(v, kTrailLow, kTrailHigh));
const ParamFromTrail = (g) => (g <= 0 ? 0 : inverseGeometric(g, kTrailLow, kTrailHigh));

/** A probability as the integer threshold a 32-bit hash is compared with. */
function ThresholdU32(probability) {
  if (!(probability > 0.0)) return 0;
  const scaled = Math.round(probability * 4294967296.0);
  return scaled >= 4294967295.0 ? 0xffffffff : scaled >>> 0;
}

/** The grid for a raster: `rows` rows, columns from the raster's aspect. */
function GridFor(rows, width, height) {
  const r = Math.max(rows, 1);
  const aspect = height > 0 ? width / height : 1.0;
  return { cols: Math.max(1, lround(r * aspect)), rows: r };
}

const OptionIndex = (value, count) => clamp(lround(value), 0, count - 1);

/** Integer division rounding towards minus infinity (Conway.cpp's floorDiv). */
const floorDiv = (a, b) => (a >= 0 ? Math.trunc(a / b) : -Math.trunc((-a + b - 1) / b));

const EDGE_NAMES = ['Torus', 'Dead'];
const PATTERN_NAMES = ['Soup', 'R-pentomino', 'Acorn', 'Diehard', 'Gosper Gun', 'Gliders', 'Clip'];
/** The source has the first six; the Over adds Clip, LAST. */
const PatternCount = (effect) => (effect ? 7 : 6);
const PALETTE_NAMES = ['Phosphor', 'Heat', 'Ice', 'Mono', 'Spectrum'];

//===========================================================================
// Rules.h / Rules.cpp, ported: each rule written once, as its notation, and
// parsed into the masks the step pass uses.
//===========================================================================

const RULES = [
  { name: 'Conway', notation: 'B3/S23' },
  { name: 'HighLife', notation: 'B36/S23' },
  { name: 'Day & Night', notation: 'B3678/S34678' },
  { name: 'Seeds', notation: 'B2/S' },
  { name: 'Life without Death', notation: 'B3/S012345678' },
  { name: 'Maze', notation: 'B3/S12345' },
  { name: 'Replicator', notation: 'B1357/S1357' },
  { name: 'Diamoeba', notation: 'B35678/S5678' },
  { name: '2x2', notation: 'B36/S125' },
  { name: 'Morley', notation: 'B368/S245' },
  { name: 'Anneal', notation: 'B4678/S35678' },
  { name: "Brian's Brain", notation: 'B2/S/C3' },
  { name: 'Star Wars', notation: 'B2/S345/C4' },
];

/** Parse `B<digits>/S<digits>[/C<number>]`; `valid` is false on anything else. */
function ParseRule(notation) {
  const masks = { birth: 0, survive: 0, states: 2, valid: false };
  // std::toupper on each character, ASCII only, split on '/'.
  const upper = [...notation].map((c) => (c >= 'a' && c <= 'z' ? c.toUpperCase() : c)).join('');
  const parts = upper.split('/');
  if (parts.length < 2 || parts.length > 3 || parts[0] === '' || parts[0][0] !== 'B' || parts[1] === ''
    || parts[1][0] !== 'S') return masks;

  const counts = (digits) => {
    let out = 0;
    for (const c of digits) {
      if (c < '0' || c > '8') return null;
      const bit = 1 << (c.charCodeAt(0) - 48);
      if (out & bit) return null; // a count written twice is a typo, not a rule
      out |= bit;
    }
    return out;
  };
  const birth = counts(parts[0].slice(1));
  const survive = counts(parts[1].slice(1));
  if (birth === null || survive === null) return masks;
  masks.birth = birth;
  masks.survive = survive;
  if (parts.length === 3) {
    if (parts[2].length < 2 || parts[2][0] !== 'C') return masks;
    let states = 0;
    for (let i = 1; i < parts[2].length; i += 1) {
      const c = parts[2][i];
      if (c < '0' || c > '9' || states > 65535) return masks;
      states = states * 10 + (c.charCodeAt(0) - 48);
    }
    if (states < 2 || states > 65535) return masks;
    masks.states = states;
  }
  masks.valid = true;
  return masks;
}

const RULE_MASKS = RULES.map((r) => ParseRule(r.notation));
/** Generations a cell spends dying before it can be born again. */
const Refractory = (m) => (m.states > 2 ? m.states - 2 : 0);
const MasksOf = (index) => RULE_MASKS[clamp(index, 0, RULES.length - 1)];

//===========================================================================
// Patterns.h / Patterns.cpp, ported: RLE, rows top to bottom as written.
//===========================================================================

const RLE = {
  1: 'b2o$2o$bo!', // R-pentomino
  2: 'bo$3bo$2o2b3o!', // Acorn
  3: '6bo$2o$bo3b3o!', // Diehard
  4: '24bo$22bobo$12b2o6b2o12b2o$11bo3bo4b2o12b2o$2o8bo5bo3b2o$2o8bo3bob2o4bobo$10bo5bo7bo$'
    + '11bo3bo$12b2o!', // Gosper Gun
};

function ParseRle(rle) {
  const shape = { width: 0, height: 0, cells: [] };
  let x = 0;
  let y = 0;
  let run = 0;
  for (const c of rle) {
    if (c === '!') break;
    if (c >= '0' && c <= '9') {
      run = run * 10 + (c.charCodeAt(0) - 48);
      continue;
    }
    const count = run > 0 ? run : 1;
    run = 0;
    if (c === 'b' || c === '.') x += count;
    else if (c === 'o' || c === 'A') {
      for (let i = 0; i < count; i += 1) shape.cells.push({ x: x++, y });
    } else if (c === '$') {
      y += count;
      x = 0;
    }
    shape.width = Math.max(shape.width, x);
  }
  shape.height = y + 1;
  for (const cell of shape.cells) {
    shape.width = Math.max(shape.width, cell.x + 1);
    shape.height = Math.max(shape.height, cell.y + 1);
  }
  return shape;
}

//===========================================================================
// Hash.h, ported: PCG in uint32 throughout (= the GLSL's pcg()).
//===========================================================================

function Pcg(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

//===========================================================================
// Clock.h, ported: owed generations, whole ones taken, at most maxSteps.
//===========================================================================

class GenerationClock {
  constructor() { this.owed = 0.0; }

  advance(gensPerSecond, dt, maxSteps) {
    if (gensPerSecond <= 0.0 || dt <= 0.0) return 0;
    this.owed += gensPerSecond * dt;
    let whole = Math.floor(this.owed);
    if (whole > maxSteps) {
      whole = maxSteps;
      this.owed = Math.min(this.owed - whole, 0.999999);
    } else {
      this.owed -= whole;
    }
    return whole;
  }

  fraction() { return this.owed; }
}

//===========================================================================
// Settle.h, ported: has the population repeated, with a period <= 30, for
// 120 generations -- or been empty for 8?
//===========================================================================

class SettleDetector {
  static kWindow = 120;
  static kMaxPeriod = 30;
  static kExtinct = 8;

  constructor() {
    this.history = [];
    this.last = 0;
    this.period = 0;
  }

  push(generation, population) {
    const { kWindow, kMaxPeriod, kExtinct } = SettleDetector;
    if (this.history.length > 0 && generation !== this.last + 1) this.history = [];
    this.last = generation;
    this.history.push(population);
    while (this.history.length > kWindow + kMaxPeriod) this.history.shift();

    let empty = 0;
    for (let i = this.history.length - 1; i >= 0 && this.history[i] === 0; i -= 1) empty += 1;
    if (empty >= kExtinct) {
      this.period = 0;
      return true;
    }

    const n = this.history.length;
    for (let p = 1; p <= kMaxPeriod; p += 1) {
      if (n < kWindow + p) break;
      let repeats = true;
      for (let i = n - kWindow; i < n && repeats; i += 1) repeats = this.history[i] === this.history[i - p];
      if (repeats) {
        this.period = p;
        return true;
      }
    }
    return false;
  }

  reset() {
    this.history = [];
    this.period = 0;
  }
}

//===========================================================================
// Conway.cpp's lookOf(): newborn, elder, trail, in linear light.
//===========================================================================

const LOOKS = [
  { newborn: [0.80, 1.00, 0.80], elder: [0.08, 0.70, 0.22], trail: [0.04, 0.30, 0.08] }, // Phosphor
  { newborn: [1.00, 0.95, 0.70], elder: [0.80, 0.12, 0.02], trail: [0.50, 0.10, 0.02] }, // Heat
  { newborn: [0.92, 0.98, 1.00], elder: [0.10, 0.35, 0.95], trail: [0.04, 0.14, 0.42] }, // Ice
  { newborn: [1.00, 1.00, 1.00], elder: [1.00, 1.00, 1.00], trail: [0.50, 0.50, 0.50] }, // Mono
  { newborn: [1.00, 1.00, 1.00], elder: [1.00, 1.00, 1.00], trail: [0.30, 0.30, 0.36] }, // Spectrum
];

//===========================================================================
// The plugin's per-instance state and ProcessOpenGL, ported.
//===========================================================================

function integerTexture(gl) {
  const t = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32UI, 1, 1, 0, gl.RED_INTEGER, gl.UNSIGNED_INT, new Uint32Array(1));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return t;
}

function blankTexture(gl) {
  const t = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return t;
}

/** One instance: what `ConwayPlugin` holds between frames. */
class ConwayState {
  constructor(gl, isEffect) {
    this.gl = gl;
    this.isEffect = isEffect;

    // PassBuffer::Ensure's formats and sampling (NEAREST: an integer texture
    // cannot be filtered). WebGL2 zero-initialises a new texture, which is the
    // plugin's Clear() on allocation.
    this.current = null;
    this.previous = null;
    this.oldCurrent = null;
    this.oldPrevious = null;
    this.countTarget = new PassBuffer(gl, { filter: 'nearest' });
    this.countRead = null;
    this.stampTexture = integerTexture(gl);
    this.stampSize = [1, 1];
    this.blankClip = blankTexture(gl);
    this.grid = { cols: 0, rows: 0 };

    // Time.
    this.lastNow = -1.0;
    this.clock = new GenerationClock();
    this.sinceStep = 0.0;
    this.phase = 1.0;

    // The field.
    this.generation = 0;
    this.totalGenerations = 0;
    this.epoch = 0;
    this.needSeed = true;
    this.seedSerial = 0;
    this.lastPattern = -1;
    this.lastSeed = -1;
    this.stepHeld = false;
    this.reseedHeld = false;
    this.pendingSteps = 0;

    // Populations.
    this.pendingCounts = [];
    this.settle = new SettleDetector();
    this.reseeds = 0;
    this.lastSettled = 0;
    this.lastSettledPeriod = -1;
    this.countBudget = kCountCellsPerFrame;
    this.lastCount = null;
    this.uncounted = 0;
  }

  dispose() {
    const gl = this.gl;
    for (const b of [this.current, this.previous, this.oldCurrent, this.oldPrevious, this.countTarget]) b?.dispose();
    gl.deleteTexture(this.stampTexture);
    gl.deleteTexture(this.blankClip);
  }
}

function createRenderer(gl, quad) {
  // Assemble(): kVersion + kCommon + the pieces. The vertex stage is kVersion
  // + kQuadVertex, as InitGL builds it.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (a, b = '') => S.VERSION + S.COMMON + a + b;
  const programs = {
    seed: new Program(gl, vertex, assemble(S.CLIP_COMMON, S.SEED_FRAGMENT), 'seed'),
    copy: new Program(gl, vertex, assemble(S.COPY_FRAGMENT), 'copy'),
    step: new Program(gl, vertex, assemble(S.CLIP_COMMON, S.STEP_FRAGMENT), 'step'),
    count: new Program(gl, vertex, assemble(S.COUNT_FRAGMENT), 'count'),
    composite: new Program(gl, vertex, assemble(S.COMPOSITE_FRAGMENT), 'composite'),
  };

  let instance = null;
  let instanceVariant = null;

  /** glUniform2i, which the kit's Program has no sugar for. */
  const setIvec2 = (program, name, x, y) => {
    const loc = program.location(name);
    if (loc === null) program.missing.add(name);
    else gl.uniform2i(loc, x | 0, y | 0);
  };

  const unbind = (count) => {
    for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
    gl.activeTexture(gl.TEXTURE0);
  };

  const drawInto = (buffer) => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, buffer.fbo);
    gl.viewport(0, 0, buffer.width, buffer.height);
    quad.draw();
  };

  /** ConwayPlugin::seedSalt. */
  const seedSalt = (st, host) => {
    const seed = clamp(lround(host('seed')), 0, 9999) >>> 0;
    return Pcg((Math.imul(seed, 2654435761) ^ Pcg((st.seedSerial + 0x9e3779b9) >>> 0)) >>> 0);
  };

  /**
   * ConwayPlugin::count. The plugin's count pass, with colour writes on, into
   * the R8 target; read back and counted here (see the header). Queued for
   * the next frame's collectCounts, as the plugin's queries are.
   */
  function count(st) {
    const cells = st.grid.cols * st.grid.rows;
    if (cells > st.countBudget) {
      // Never read: a gap in the history, which the settle law starts again on.
      st.uncounted += 1;
      return;
    }
    st.countBudget -= cells;

    const target = st.countTarget;
    gl.bindFramebuffer(gl.FRAMEBUFFER, target.fbo);
    gl.viewport(0, 0, target.width, target.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const p = programs.count.use();
    bindTexture(gl, 0, st.current.texture);
    p.setSampler('Current', 0);
    p.setInt('CountDyingForTest', 0);
    quad.draw();
    unbind(1);

    // RGBA / UNSIGNED_BYTE is always accepted from a normalised target; RED is
    // a quarter of the bytes where the implementation offers it.
    if (!st.countRead || st.countRead.cells !== cells) {
      const format = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_FORMAT);
      const type = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_TYPE);
      const red = format === gl.RED && type === gl.UNSIGNED_BYTE;
      st.countRead = { cells, red, data: new Uint8Array(cells * (red ? 1 : 4)) };
    }
    const read = st.countRead;
    const alignment = gl.getParameter(gl.PACK_ALIGNMENT);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
    gl.readPixels(0, 0, target.width, target.height, read.red ? gl.RED : gl.RGBA, gl.UNSIGNED_BYTE, read.data);
    gl.pixelStorei(gl.PACK_ALIGNMENT, alignment);
    let population = 0;
    const stride = read.red ? 1 : 4;
    for (let i = 0; i < read.data.length; i += stride) if (read.data[i] !== 0) population += 1;

    st.pendingCounts.push({ generation: st.generation, epoch: st.epoch, population });
    st.lastCount = { generation: st.generation, population };
  }

  /** ConwayPlugin::collectCounts: everything is back by the next frame. */
  function collectCounts(st, host) {
    while (st.pendingCounts.length > 0) {
      const pending = st.pendingCounts.shift();
      // A field since reseeded or re-gridded: its counts say nothing about this one.
      if (pending.epoch !== st.epoch) continue;
      if (st.settle.push(pending.generation, pending.population) && host('autoReseed') > 0.5 && !st.needSeed) {
        st.needSeed = true;
        st.lastSettled = pending.generation;
        st.lastSettledPeriod = st.settle.period;
        st.seedSerial = (st.seedSerial + 1) >>> 0;
        st.reseeds += 1;
        st.epoch += 1;
        st.settle.reset();
      }
    }
  }

  /** ConwayPlugin::ensureBuffers: the old pair moves aside on a new size. */
  function ensureBuffers(st, want) {
    if (st.current && (st.current.width !== want.cols || st.current.height !== want.rows)) {
      st.oldCurrent = st.current;
      st.oldPrevious = st.previous;
      st.current = null;
      st.previous = null;
    }
    st.current = (st.current ?? new PassBuffer(gl, { filter: 'nearest' })).ensure(want.cols, want.rows, gl.R32UI);
    st.previous = (st.previous ?? new PassBuffer(gl, { filter: 'nearest' })).ensure(want.cols, want.rows, gl.R32UI);
    st.countTarget.ensure(want.cols, want.rows, gl.R8);
  }

  /** ConwayPlugin::seed. */
  function seed(st, host, clip, clipRaster, pattern) {
    let mode = 0;
    if (pattern === 5) mode = 2; // Gliders
    else if (pattern === 6) mode = 3; // Clip
    else if (RLE[pattern]) mode = 1;

    const origin = [0, 0];
    if (mode === 1) {
      // The pattern as a stamp, right way up (its first row on top), centred.
      const shape = ParseRle(RLE[pattern]);
      const cells = new Uint32Array(shape.width * shape.height);
      for (const cell of shape.cells) cells[(shape.height - 1 - cell.y) * shape.width + cell.x] = 1;
      const alignment = gl.getParameter(gl.UNPACK_ALIGNMENT);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.bindTexture(gl.TEXTURE_2D, st.stampTexture);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32UI, shape.width, shape.height, 0, gl.RED_INTEGER, gl.UNSIGNED_INT, cells);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, alignment);
      st.stampSize = [shape.width, shape.height];
      origin[0] = floorDiv(st.grid.cols - shape.width, 2);
      origin[1] = floorDiv(st.grid.rows - shape.height, 2);
    }

    const p = programs.seed.use();
    bindTexture(gl, 0, st.stampTexture);
    bindTexture(gl, 1, clip ?? st.blankClip);
    p.setSampler('Stamp', 0);
    p.setSampler('Clip', 1);
    setIvec2(p, 'Grid', st.grid.cols, st.grid.rows);
    p.setInt('Mode', mode);
    p.setUint('DensityU', ThresholdU32(DensityFromParam(host('density'))));
    p.setUint('Salt', seedSalt(st, host));
    setIvec2(p, 'StampOrigin', origin[0], origin[1]);
    setIvec2(p, 'StampSize', st.stampSize[0], st.stampSize[1]);
    setIvec2(p, 'ClipRaster', clipRaster[0], clipRaster[1]);
    p.set('Threshold', ThresholdFromParam(host('threshold')));
    p.setInt('MirrorForTest', 0);
    // Both buffers: the crossfade from the last generation starts from this one.
    drawInto(st.current);
    drawInto(st.previous);
    unbind(2);

    st.generation = 0;
    st.epoch += 1;
    st.settle.reset();
    count(st);
  }

  /** ConwayPlugin::step, with no audio patch (there is no audio here). */
  function step(st, host, clip, clipRaster) {
    const masks = MasksOf(OptionIndex(host('rule'), RULES.length));
    const salt = seedSalt(st, host);
    const serial = st.totalGenerations >>> 0;
    const feedU = st.isEffect ? ThresholdU32(FeedFromParam(host('feed'))) : 0;

    const p = programs.step.use();
    bindTexture(gl, 0, st.current.texture);
    bindTexture(gl, 1, clip ?? st.blankClip);
    p.setSampler('Current', 0);
    p.setSampler('Clip', 1);
    setIvec2(p, 'Grid', st.grid.cols, st.grid.rows);
    p.setInt('Torus', OptionIndex(host('edges'), 2) === 0 ? 1 : 0);
    p.setUint('Birth', masks.birth);
    p.setUint('Survive', masks.survive);
    p.setUint('Refractory', Refractory(masks));
    p.setInt('RuleOn', 1);
    p.setUint('NoiseU', ThresholdU32(NoiseFromParam(host('noise'))));
    p.setUint('NoiseSalt', Pcg((salt ^ Pcg((Math.imul(serial, 2) + 1) >>> 0)) >>> 0));
    p.setInt('FeedOn', feedU !== 0 && clip ? 1 : 0);
    p.setUint('FeedU', feedU);
    p.setUint('FeedSalt', Pcg((salt ^ Pcg((Math.imul(serial, 2) + 2) >>> 0)) >>> 0));
    setIvec2(p, 'ClipRaster', clipRaster[0], clipRaster[1]);
    p.set('Threshold', ThresholdFromParam(host('threshold')));
    p.setInt('MirrorForTest', 0);
    // The audio patch: only an onset sets one, and there is none.
    p.setInt('PatchRadius', 0);
    setIvec2(p, 'PatchCentre', 0, 0);
    p.setUint('PatchU', ThresholdU32(DensityFromParam(host('density'))));
    p.setUint('PatchSalt', 0);
    p.setInt('CountSelfForTest', 0);
    p.setInt('WrapSkewForTest', 0);
    drawInto(st.previous);
    unbind(2);
    // The new generation is current; the one before it stays as previous.
    [st.current, st.previous] = [st.previous, st.current];
  }

  /**
   * ConwayPlugin::composite, into the canvas: the viewport is the host's, the
   * Raster the clip's (the same size here, as in the plugin).
   */
  function composite(st, host, clip, raster, width, height) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const palette = OptionIndex(host('palette'), PALETTE_NAMES.length);
    const look = LOOKS[palette];
    const p = programs.composite.use();
    bindTexture(gl, 0, st.current.texture);
    bindTexture(gl, 1, st.previous.texture);
    bindTexture(gl, 2, clip ?? st.blankClip);
    p.setSampler('Current', 0);
    p.setSampler('Previous', 1);
    p.setSampler('InputTexture', 2);
    setIvec2(p, 'Grid', st.grid.cols, st.grid.rows);
    p.set('Raster', raster[0], raster[1]);
    p.set('ViewOrigin', 0, 0);
    p.set('Phase', st.phase);
    p.set('Gap', GapFromParam(host('gap')));
    p.set('AgeSpan', AgeSpanFromParam(host('ageSpan')));
    p.set('Trail', TrailFromParam(host('trail')));
    p.setInt('Spectrum', palette === 4 ? 1 : 0);
    p.set('Newborn', ...look.newborn);
    p.set('Elder', ...look.elder);
    p.set('TrailColour', ...look.trail);
    p.setInt('IsEffect', st.isEffect ? 1 : 0);
    p.set('ClipColour', st.isEffect ? clamp01(host('clipColour')) : 0.0);
    p.set('Backdrop', st.isEffect ? clamp01(host('backdrop')) : 0.0);
    p.set('MixAmount', st.isEffect ? clamp01(host('mix')) : 1.0);
    p.setInt('PointSampleForTest', 0);
    quad.draw();
    unbind(3);
  }

  return {
    get stats() { return instance?.stats ?? null; },
    get programs() { return programs; },

    render({ input, params, width, height, time, variant }) {
      const isEffect = variant === 'over';
      if (!instance || instanceVariant !== variant) {
        instance?.dispose();
        instance = new ConwayState(gl, isEffect);
        instanceVariant = variant;
      }
      const st = instance;
      // The plugin's params[] is float: what the host set, as a float.
      const host = (id) => f32(params.get(id));
      gl.disable(gl.BLEND);

      // The raster: the clip's for the Over, the host viewport's for the source.
      const w = isEffect ? input.width : width;
      const h = isEffect ? input.height : height;
      const clip = isEffect ? input.texture : null;
      const clipRaster = [w, h];

      //-------------------------------------------------------------------
      // Time: the host's (the page's clock, in seconds), frame to frame. A
      // backwards or large step is a jump and no time passes across it (a
      // jump also resets the audio analyser; there is none here).
      //-------------------------------------------------------------------
      let dt = 0.0;
      if (st.lastNow >= 0.0) {
        const stepTime = time - st.lastNow;
        if (!(stepTime < 0.0 || stepTime > kMaxFrameDelta)) dt = stepTime;
      }
      st.lastNow = time;
      st.countBudget = kCountCellsPerFrame;

      //-------------------------------------------------------------------
      // The populations that have come back, which may settle the field.
      //-------------------------------------------------------------------
      collectCounts(st, host);

      //-------------------------------------------------------------------
      // The buttons (a press is the rising edge), and what reseeds. Seed is
      // read BEFORE Reseed: a new Seed restarts its sequence, and a Reseed
      // press in the same frame then takes the next soup (Conway.cpp).
      //-------------------------------------------------------------------
      const stepDown = host('step') > 0.5;
      if (stepDown && !st.stepHeld) st.pendingSteps += 1;
      st.stepHeld = stepDown;
      const seedValue = clamp(lround(host('seed')), 0, 9999);
      if (seedValue !== st.lastSeed) {
        st.seedSerial = 0;
        st.needSeed = true;
      }
      st.lastSeed = seedValue;
      const reseedDown = host('reseed') > 0.5;
      if (reseedDown && !st.reseedHeld) {
        st.seedSerial = (st.seedSerial + 1) >>> 0;
        st.needSeed = true;
      }
      st.reseedHeld = reseedDown;
      const pattern = OptionIndex(host(isEffect ? 'patternOver' : 'pattern'), PatternCount(isEffect));
      if (pattern !== st.lastPattern) st.needSeed = true;
      st.lastPattern = pattern;

      //-------------------------------------------------------------------
      // How many generations this frame: what the clock owes, then the
      // presses, at most kMaxStepsPerFrame.
      //-------------------------------------------------------------------
      const speed = SpeedFromParam(host('speed'));
      const clockSteps = st.clock.advance(speed, dt, kMaxStepsPerFrame);
      const steps = Math.min(clockSteps + st.pendingSteps, kMaxStepsPerFrame);
      if (clockSteps > 0) st.sinceStep = st.clock.fraction() / speed;
      else if (steps > 0) st.sinceStep = 0.0;
      else st.sinceStep += dt;
      st.pendingSteps = 0;

      //-------------------------------------------------------------------
      // The grid follows Cell Size and the raster's ASPECT; a new shape keeps
      // the overlap, centred, copied before the old pair is freed.
      //-------------------------------------------------------------------
      const want = GridFor(RowsFromParam(host('cellSize')), w, h);
      const first = !st.current;
      ensureBuffers(st, want);
      if (st.oldCurrent) {
        const p = programs.copy.use();
        p.setSampler('Old', 0);
        setIvec2(p, 'OldGrid', st.oldCurrent.width, st.oldCurrent.height);
        setIvec2(p, 'Offset', floorDiv(st.oldCurrent.width - want.cols, 2), floorDiv(st.oldCurrent.height - want.rows, 2));
        for (const [from, to] of [[st.oldCurrent, st.current], [st.oldPrevious, st.previous]]) {
          bindTexture(gl, 0, from.texture);
          drawInto(to);
        }
        unbind(1);
        st.oldCurrent.dispose();
        st.oldPrevious.dispose();
        st.oldCurrent = null;
        st.oldPrevious = null;
      }
      const regridded = !first && (want.cols !== st.grid.cols || want.rows !== st.grid.rows);
      st.grid = want;

      //-------------------------------------------------------------------
      // The field: seeded, or carried over a re-grid; then the steps.
      //-------------------------------------------------------------------
      if (st.needSeed) {
        st.needSeed = false;
        seed(st, host, clip, clipRaster, pattern);
      } else if (regridded) {
        // The overlap is the same field, cropped or padded: its history of
        // populations is not.
        st.epoch += 1;
        st.settle.reset();
        count(st);
      }
      for (let i = 0; i < steps && !st.needSeed; i += 1) {
        step(st, host, clip, clipRaster);
        st.generation += 1;
        st.totalGenerations += 1;
        count(st);
      }

      //-------------------------------------------------------------------
      // Smooth: how far the crossfade from the last generation has got.
      //-------------------------------------------------------------------
      const smooth = SmoothFromParam(host('smooth'));
      const period = speed > 0.0 ? 1.0 / speed : kPausedPeriod;
      st.phase = smooth <= 0.0 ? 1.0 : clamp(st.sinceStep / (smooth * period), 0.0, 1.0);

      composite(st, host, clip, clipRaster, width, height);

      // A host lets go of an event button; the kit has none, so the page does.
      if (stepDown) params.set('step', 0);
      if (reseedDown) params.set('reseed', 0);

      st.stats = {
        speed,
        steps,
        grid: st.grid,
        generation: st.generation,
        total: st.totalGenerations,
        count: st.lastCount,
        reseeds: st.reseeds,
        lastSettled: st.lastSettled,
        lastSettledPeriod: st.lastSettledPeriod,
        uncounted: st.uncounted,
      };
    },
  };
}

//===========================================================================
// The parameters: each constructor's, in HostOrder()'s order and groups,
// with the plugin's names and defaults. `only` says which plugin declares it.
//===========================================================================

const fmt = (n, digits = 0) => n.toFixed(digits);
/** Three significant figures, for the geometric controls. */
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));

/**
 * The constructor's defaults, source then Over. Each holds every id, because
 * the plugin's params[] does. The only difference is Pattern: Soup in the
 * source, Clip in the Over (here two rows; see the header).
 */
const COMMON_DEFAULTS = {
  rule: 0, edges: 0, speed: ParamFromSpeed(15.0), smooth: 0.35, step: 0,
  pattern: 0, patternOver: 6, density: 0.35, seed: 1, reseed: 0, autoReseed: 1, noise: 0.0,
  threshold: 0.5, feed: 0.3,
  cellSize: ParamFromRows(135), gap: 0.24, palette: 0, ageSpan: ParamFromAgeSpan(40.0),
  trail: ParamFromTrail(3.0), clipColour: 0.0, backdrop: 0.35, mix: 1.0,
};
const DEFAULTS = { source: { ...COMMON_DEFAULTS }, over: { ...COMMON_DEFAULTS } };

const query = new URLSearchParams(window.location.search);
const initialVariant = query.get('plugin') === 'over' ? 'over' : 'source';

const std = (id, name, group, display, hint, only) => ({ id, name, type: 'standard', group, display, hint, only });
const opt = (id, name, elements, group, hint, only) => ({ id, name, type: 'option', elements, group, hint, only });
const bool = (id, name, group, hint, only) => ({ id, name, type: 'boolean', group, hint, only });

const SEED_CHOICES = 100;

const PARAMS = [
  opt('rule', 'Rule', RULES.map((r) => r.name), 'Automaton',
    'Which rule the cells obey, each parsed from its notation: Conway B3/S23, HighLife B36/S23, Day & Night B3678/S34678, Seeds B2/S, Life without Death B3/S012345678, Maze B3/S12345, Replicator B1357/S1357, Diamoeba B35678/S5678, 2x2 B36/S125, Morley B368/S245, Anneal B4678/S35678, Brian’s Brain B2/S/C3, Star Wars B2/S345/C4. Changing it does not reseed.'),
  opt('edges', 'Edges', EDGE_NAMES, 'Automaton', 'Torus: the field wraps, top to bottom and side to side. Dead: everything past the edge is dead, and gliders crash into it.'),

  std('speed', 'Speed', 'Time', (v) => (SpeedFromParam(f32(v)) === 0 ? 'paused' : `${sig(SpeedFromParam(f32(v)))} gen/s`),
    'Generations a second: exactly 0 at the bottom (the field stands still; Step advances it), then 0.5 to 500, geometrically. A frame runs at most 32.'),
  std('smooth', 'Smooth', 'Time', (v) => `${fmt(SmoothFromParam(v), 2)} × period`,
    'A crossfade from the last generation into this one, as a share of a generation’s period (0.25 s when Speed is 0). 0 is a hard cut.'),
  bool('step', 'Step', 'Time', 'One generation, by hand. FF_TYPE_EVENT in the plugin; a toggle the page releases after one frame here. (The transport’s Step under the picture is one frame of time, not a generation.)'),

  opt('pattern', 'Pattern', PATTERN_NAMES.slice(0, 6), 'Seeding',
    'What the field starts from, and what Reseed lays down. Soup: random cells at Density. R-pentomino, Acorn, Diehard, Gosper Gun: one famous pattern in the middle. Gliders: a glider per 16 × 16 block with probability Density. Choosing one reseeds at once.', 'source'),
  opt('patternOver', 'Pattern', PATTERN_NAMES, 'Seeding',
    'What the field starts from, and what Reseed lays down. Clip (the Over’s default): the clip’s bright cells. Soup, the four famous patterns and Gliders as in the source. Choosing one reseeds at once.', 'over'),
  std('density', 'Density', 'Seeding', (v) => `${fmt(100 * DensityFromParam(v), 0)}%`, 'The soup’s fill, and the glider fleet’s.'),
  opt('seed', 'Seed', Array.from({ length: SEED_CHOICES }, (_, i) => String(i)), 'Seeding',
    'Which soup: the same Seed always gives the same first soup. FF_TYPE_INTEGER 0–9999 in the plugin; 0–99 here.'),
  bool('reseed', 'Reseed', 'Seeding', 'The next soup in the Seed’s sequence (or the pattern again, or the clip as it is now). FF_TYPE_EVENT in the plugin; a toggle the page releases after one frame here.'),
  bool('autoReseed', 'Auto Reseed', 'Seeding', 'Reseed when every generation’s population has repeated exactly, with a period of 30 or less, for 120 generations — or the field has been empty for 8. Never fires with Noise above 0.'),
  std('noise', 'Noise', 'Seeding', (v) => (NoiseFromParam(f32(v)) === 0 ? 'off' : `${NoiseFromParam(f32(v)).toExponential(1)} / cell / gen`),
    'Spontaneous births anywhere, outside the rule: off at the bottom, then one in a million to one in a hundred per cell per generation.'),
  std('threshold', 'Threshold', 'Seeding', (v) => fmt(ThresholdFromParam(v), 2),
    'How bright a part of the clip must be to be alive: its brightest channel (not its luma) times alpha, from the pixel under each cell’s centre.', 'over'),
  std('feed', 'Feed', 'Seeding', (v) => `${fmt(100 * FeedFromParam(f32(v)), 1)}% / gen`,
    'Every generation, each cell under a bright part of the clip is reborn with this chance (the slider squared).', 'over'),

  std('cellSize', 'Cell Size', 'Look', (v) => `${RowsFromParam(f32(v))} rows`,
    'Rows of the grid: 1080 (a cell a pixel tall at 1080p) to 16, geometrically. Columns follow the output’s shape. Changing it keeps the field, cropped or extended round its middle.'),
  std('gap', 'Gap', 'Look', (v) => `${fmt(GapFromParam(v), 2)} cell`, 'Dark space round each lit cell, 0 to half a cell.'),
  opt('palette', 'Palette', PALETTE_NAMES, 'Look', 'The colours from newborn to elder and of the trail. Spectrum turns the hue with age.'),
  std('ageSpan', 'Age Span', 'Look', (v) => `${sig(AgeSpanFromParam(f32(v)))} gen`,
    'Generations from the newborn colour to the elder one, 1 to 1000 (with Spectrum, one turn of the hue).'),
  std('trail', 'Trail', 'Look', (v) => (TrailFromParam(f32(v)) === 0 ? 'off' : `${sig(TrailFromParam(f32(v)))} gen`),
    'How long a cell that has just died keeps glowing: off, then 0.5 to 200 generations (its brightness falls by e every Trail).'),
  std('clipColour', 'Clip Colour', 'Look', (v) => fmt(v, 2), 'The cells take the clip’s colour under them. At 1 with Backdrop 0 the live cells are windows onto the clip.', 'over'),
  std('backdrop', 'Backdrop', 'Look', (v) => fmt(v, 2), 'How much of the clip shows behind the cells.', 'over'),
  std('mix', 'Mix', 'Look', (v) => fmt(v, 2), 'The whole effect against the clip. At 0 the clip is returned exactly, alpha and all.', 'over'),
];
for (const p of PARAMS) p.default = DEFAULTS[initialVariant][p.id];

let renderer = null;

const mounted = mountDemo({
  name: 'Conway',
  pluginId: 'LF01 · LF02',
  kind: ['source', 'effect'],
  tagline:
    'Conway’s Game of Life, and twelve of its relatives, on an integer texture. Every cell is alive or dead and all of them update at once by one rule over their eight neighbours; nothing is drawn as a picture of Life, so the gliders, the blinkers, Gosper’s gun, the methuselahs and a soup burning down to ash all fall out of the rule. A cell’s colour is its age, the just-dead glow, and the field reseeds itself when its population has settled. SW Conway plays a soup or a famous pattern; SW Conway Over lets the clip seed the field and keep feeding it.',
  repo: 'https://github.com/stoatworks-labs/conway',

  blurb:
    'It is Conway’s own seed, copy, step, count and composite passes, ported from the repository to WebGL2 and driven by a JavaScript port of the plugin’s CPU half — the generation clock, the buttons, the rule table, the grid, the salts and the settle law behind Auto Reseed — which only a reader checks. The population is counted by reading the plugin’s count pass back, not by its occlusion query. There is no audio here, so the audio controls are absent. SW Conway reads no video; SW Conway Over is seeded and fed by a generated clip.',

  // The source is transparent where nothing lives; the backdrop shows it.
  showBackdrop: true,

  variants: {
    label: 'Plugin',
    default: initialVariant,
    options: [
      { id: 'source', name: 'SW Conway (source)', hint: 'LF01, FF_SOURCE: a soup or a famous pattern. Reads no clip.' },
      { id: 'over', name: 'SW Conway Over (effect)', hint: 'LF02, FF_EFFECT: the clip’s bright cells seed the field, and Feed keeps breeding them.' },
    ],
  },

  // For the Over only. Lights on black and the geometry card give bright
  // shapes against a dark ground, which is what Threshold 0.5 wants.
  sources: ['spot', 'grid', 'scene', 'bars', 'alpha'],

  params: PARAMS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code, and nothing checks it but a reader. The generation clock (Speed × dt owed in double, whole generations taken, at most 32 a frame, the rest dropped), the buttons (Step; Reseed; a Seed change restarting the soup sequence before a Reseed press is read), Pattern reseeding, the rule table parsed from its notation, the RLE parser and the stamp laid the right way up, the grid from Cell Size and the output’s aspect with its centred re-grid, the salts (the plugin’s PCG), Smooth’s phase, the palettes and the settle law are translated to JavaScript here. The repository’s cwtest --clock-law, --settle-law and --resize check the C++ and have never heard of this page. When it was written it was compared once against the plugin itself, through cwtest on the same 240 × 135 grid: generation 0 cell for cell for Soup, Acorn, Gosper Gun, Gliders and the Over’s clip, and the population after up to 40 generations under Conway, Noise, Brian’s Brain, Star Wars with dead edges, a Seed change and Reseed, and the Over’s Feed — every one identical. That comparison does not run again.',
    'The GPU half is not a port: the seed, copy, step, count and composite passes are the plugin’s own GLSL, on the same R32UI state (texelFetch and integer arithmetic, exact in WebGL2 as in GL 4.1), and demo/tools/check_shaders.py fails the repository’s verify script if a character of them drifts.',
    'The population is counted differently. The plugin counts each generation with an occlusion query (GL_SAMPLES_PASSED); WebGL2 offers only ANY_SAMPLES_PASSED, a yes or no. So this page runs the plugin’s own count pass with colour writes on into an 8-bit target, reads it back and counts the marked cells in JavaScript — the same cells, counted exactly — and hands the counts to the settle law at the next frame, as the plugin’s queries come back. A frame reads back at most about two million cells; past that a generation goes uncounted, the settle history starts again (the law’s rule for a gap), and Auto Reseed waits. That only happens at the finest Cell Sizes run fast; at the defaults every generation is counted.',
    'Nothing audio. The plugin declares an Audio FFT buffer that Resolume fills, with Audio Steps (generations per onset) and Audio Seeds (a patch of soup per onset) over it. A browser has no Resolume FFT, so all three are absent from this panel rather than present and dead. With no spectrum the plugin’s analyser never fires, so leaving them out changes nothing the page draws.',
    'Step and Reseed are FF_TYPE_EVENT in the plugin. The kit has no event control, so they are toggles the page releases after one frame — one press, one rising edge, which is what the plugin counts. Seed is FF_TYPE_INTEGER (0–9999); it is a dropdown here offering 0–99. The About block is not on the panel.',
    'Switching the Plugin dropdown is a new instance, as it would be in Resolume: the field starts again and every control goes to that plugin’s constructor default. The Over declares five more controls (Threshold, Feed, Clip Colour, Backdrop, Mix) and a seventh Pattern, Clip, its default; Pattern is shown as two rows of the same name because the kit’s dropdowns have one fixed list, and only the chosen plugin’s is visible.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame. The plugin’s vote on whether the host sends seconds or milliseconds is not ported, because this host’s unit is known. Restart is a clock jump, which the plugin takes as no time passing, so the field carries on; switching the Plugin dropdown is what starts it again. With the page paused no frames run, so after a Step press Smooth’s crossfade stays at its start — step by hand with Speed at 0 and the page playing, or with Smooth at 0.',
    'Over: the clip is the kit’s generated one (or your own file), premultiplied, and the plugin reads the brightest channel times alpha — so over the transparency clip a half-transparent edge counts as alpha squared, where Resolume’s own clips may arrive straight. The grid follows the output’s aspect, so the 16:9 Composition presets all run the same grid; a file of another shape re-grids, keeping the middle.',
    'Nothing here is measured. The plugin’s harness asks it for the published facts of Life (the R-pentomino’s 116 cells at generation 1103, Diehard gone at 130, the gun’s period 30), holds all thirteen rules to an independent stepper bit for bit, checks the count, the settle law, the clock and the box filter, each with a negative control — and that harness, not this page, is the reason to believe the automaton. The plugin itself has never been loaded into Resolume.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad);
    return renderer;
  },
});

//---------------------------------------------------------------------------
// Which controls belong to which plugin, and whose defaults are in force.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule. Rows are
// found by their control's id (the two Pattern rows share a name), falling back
// to the name for the toggles, which carry no id.
//---------------------------------------------------------------------------
if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';
  let shownVariant = mounted.state.variant;

  // Copy link carries the plugin, since the two declare different controls.
  const toQuery = params.toQuery.bind(params);
  params.toQuery = () => {
    const q = toQuery();
    if (mounted.state.variant === 'over') q.set('plugin', 'over');
    return q;
  };

  const show = (node, on) => { if (node) node.style.display = on ? '' : 'none'; };
  const rowOf = (p) => document.getElementById(`p-${p.id}`)?.closest('.prow')
    ?? [...document.querySelectorAll('.prow')].find((row) => row.querySelector('.prow__name')?.textContent === p.name);

  function showForVariant(variant) {
    for (const p of PARAMS) {
      if (p.only) show(rowOf(p), p.only === variant);
    }
    const effect = variant === 'over';
    for (const field of document.querySelectorAll('.transport__field')) {
      if (field.querySelector('.transport__label')?.textContent === 'Clip') show(field, effect);
    }
    show(document.querySelector('.transport__file'), effect);
  }

  document.addEventListener('demo:state', () => {
    const variant = mounted.state.variant;
    if (variant !== shownVariant) {
      shownVariant = variant;
      // A new instance: that constructor's defaults, for Defaults too.
      params.defaults = { ...DEFAULTS[variant] };
      params.reset();
    }
    if (!embed) showForVariant(variant);
  });

  if (!embed) {
    showForVariant(shownVariant);

    //-----------------------------------------------------------------------
    // A status line: reports, measures nothing.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const line = document.createElement('p');
      line.className = 'stage__status';
      line.id = 'conway-stats';
      line.setAttribute('aria-live', 'off');
      stage.append(line);
      setInterval(() => {
        const s = renderer?.stats;
        if (!s || s.grid === undefined) return;
        const settled = s.reseeds === 0
          ? 'no auto reseed yet'
          : `${s.reseeds} auto reseed${s.reseeds === 1 ? '' : 's'}, the last when generation ${s.lastSettled} ${s.lastSettledPeriod === 0 ? 'had been empty for 8' : `repeated with period ${s.lastSettledPeriod}`}`;
        line.textContent =
          `${s.speed === 0 ? 'Paused (Speed 0)' : `${sig(s.speed)} generations a second`}`
          + ` · grid ${s.grid.cols} × ${s.grid.rows}`
          + ` · generation ${s.generation} since the last seed`
          + (s.count ? ` · ${s.count.population} live cell${s.count.population === 1 ? '' : 's'} at generation ${s.count.generation}` : '')
          + ` · ${settled}`
          + (s.uncounted > 0 ? ` · ${s.uncounted} generation${s.uncounted === 1 ? '' : 's'} not counted (read-back limit)` : '');
      }, 250);
    }
  }
}
