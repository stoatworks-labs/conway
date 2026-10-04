# Attributions

Conway is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is a provisional hand copy written before this repo was registered in the fleet;
registration replaces it with the generated file (`scripts/sync-attributions.py` in
the `stoatworks-backend` repo, `--adopt` once, since this is a hand copy).

## The idea and the patterns

### The Game of Life — John Horton Conway, 1970

Published by Martin Gardner in "Mathematical Games", *Scientific American*, October
1970. The rule B3/S23 is Conway's. The other rules in the Rule dropdown are the Life
community's: HighLife (B36/S23, Nathan Thompson, 1994), Day & Night (B3678/S34678,
Nathan Thompson, 1997, studied by David I. Bell), Seeds and Brian's Brain (Brian
Silverman), Life without Death, Maze, Replicator (Edward Fredkin's parity rule),
Diamoeba, 2x2, Morley, Anneal and Star Wars, by their usual names.

### The patterns and the numbers

The RLE for the R-pentomino, the acorn, Diehard, Bill Gosper's glider gun (found by
Gosper's group at MIT, November 1970), the glider, the lightweight spaceship, the
still lifes and the oscillators is the Life community's standard notation, as the
LifeWiki (<https://conwaylife.com/wiki/>) publishes it. The facts the harness checks
-- the R-pentomino stabilises after 1103 generations with 116 cells, the acorn after
5206 with 633, Diehard disappears after 130, the gun emits a glider every 30 -- and
the credits above were checked on 2026-10-04 against Wikipedia's "Conway's Game of
Life", "Highlife (cellular automaton)", "Day and Night (cellular automaton)" and
"Brian's Brain" (the LifeWiki refuses automated reads). The harness then reproduces
each number with its own stepper on an unbounded plane (`cwtest --literature`)
before it asks the plugin (`--patterns`).

## Code we derived from other people's work

### Source-plus-Over shape, harness, parameter table — Stoatworks radar

<https://github.com/stoatworks-labs/radar>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect, `HostOrder()` with the About
block last, the clock-unit vote, GLState.h, Diag, the harness's rig, cue sheets and
`--pipe`, tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py, all
adapted from radar (which carries them from boreal, flyback, downpour, tinsel and
plotter).

### Audio analyser — Stoatworks rosette, via radar

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is radar's copy of millpond's copy of rosette's analyser
(itself from macroblock's), with its primed first frame, which `cwtest --prime`
checks.

### An integer-format PassBuffer — Stoatworks stencil

<https://github.com/stoatworks-labs/stencil>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.{h,cpp} is stencil's, cut down: a texture and a framebuffer whose
upload format follows an integer internal format, which `ffglex::FFGLFBO` cannot
allocate.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the fleet.
