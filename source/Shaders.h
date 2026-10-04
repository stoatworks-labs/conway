#pragma once

#include <string>

/**
    The GLSL, as text.

    `kCommon` is a LIBRARY, not a shader: no #version, no main. Every pass is
    assembled as kVersion + kCommon + its pieces, so the harness's checks run
    the text the plugin runs (`cwtest` drives the plugin; it has no shaders of
    its own). Pieces stay under MSVC's ~16 KB literal cap; tools/glslc.sh
    reassembles them for glslc.

    The state is one R32UI texel per cell: the low 16 bits its AGE in
    generations (0 = dead), the high 16 bits the generations SINCE it died
    (0 = never alive). Every pass that reads it uses texelFetch and integer
    arithmetic only, so the automaton is bit-exact on any GLSL 4.10
    implementation; floats appear only in the composite, which colours it.

    Passes:

      seed       (on Reseed) Soup, a stamped pattern, a glider fleet, or the
                 clip's bright cells (Over)
      copy       (on a re-grid) the old grid's overlap, centred, cell for cell
      step       one generation: the rule over the eight neighbours, on a
                 torus or with dead edges; then births from outside the rule
                 (Noise, Feed, an audio patch)
      count      the live cells of a generation, under an occlusion query
      composite  the grid box-filtered over each pixel, coloured by age, the
                 last generation crossfaded into this one; the clip (Over)
*/
namespace conway::shaders
{
extern const char* const kVersion;
extern const char* const kCommon;
extern const char* const kClipCommon;

extern const char* const kQuadVertex;
extern const char* const kSeedFragment;
extern const char* const kCopyFragment;
extern const char* const kStepFragment;
extern const char* const kCountFragment;
extern const char* const kCompositeFragment;

/// kVersion + kCommon + the pieces, in order.
std::string Assemble( const char* a, const char* b = nullptr );

} // namespace conway::shaders
