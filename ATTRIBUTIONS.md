# Attributions

Conway is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Source-plus-Over shape, parameter table, GL state and harness — Stoatworks radar

<https://github.com/stoatworks-labs/radar>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect, HostOrder() with the About block last, the host-clock unit vote, GLState.h, Diag, the harness's rig, cue sheets and --pipe, tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py are radar's (which carries them from boreal, flyback, downpour, tinsel and plotter).

### Audio analyser — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is radar's copy of millpond's copy of rosette's analyser (itself from macroblock's), with its primed first frame, which cwtest --prime checks.

### An integer-format PassBuffer — Stoatworks stencil

<https://github.com/stoatworks-labs/stencil>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.{h,cpp} is stencil's, cut down: a texture and a framebuffer whose upload format follows an integer internal format, which ffglex::FFGLFBO cannot allocate.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Conway's Game of Life — Wikipedia

<https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life>

Checked 2026-10-04 for the facts the harness asks of the plugin: the R-pentomino stabilises after 1103 generations with a population of 116, the acorn after 5206 with 633, Diehard disappears after 130, and the Gosper glider gun (Gosper's group at MIT, November 1970) emits a glider every 30 generations. The harness reproduces each on an unbounded plane with its own stepper first. The LifeWiki, where the patterns' RLE is published, refuses automated reads.

### Life-like cellular automaton; Highlife; Day and Night; Brian's Brain — Wikipedia

<https://en.wikipedia.org/wiki/Life-like_cellular_automaton>

Checked 2026-10-04 for the rules in the Rule dropdown and their credits: HighLife B36/S23 (Nathan Thompson, 1994), Day & Night B3678/S34678 (Nathan Thompson, 1997, studied by David I. Bell), symmetric under exchanging live and dead cells, as is Anneal; Replicator B1357/S1357 (Edward Fredkin), every pattern replaced by copies of itself; Seeds and Brian's Brain (Brian Silverman), Brian's Brain's dying cells neither counting nor able to be born.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The Game of Life

John Horton Conway's cellular automaton, published by Martin Gardner in Mathematical Games, Scientific American, October 1970. Implemented from the rule's definition; the patterns are the Life community's, in their standard RLE. Nothing is copied from anyone's source.

## Standards and published specifications

What the implementation is measured against.

- **B/S rule notation and Generations' C** — a Life-like rule as the neighbour counts that give birth (B) and survival (S), with Generations rules adding C, the number of states, so a dying cell is refractory for C - 2 generations. The plugin parses each rule from this notation.
- **Run-length encoded (RLE) patterns** — b for a dead cell, o for a live one, $ for the end of a row, a count in front, ! at the end: the Life community's file format for patterns.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind every soup, Noise, Feed and the audio patches, compared with integer thresholds on the GPU and in the harness.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
