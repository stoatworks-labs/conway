#pragma once

#include "StoatworksAboutLinks.h"

#include <cstdint>
#include <vector>

/**
    The host's parameters, and what they mean in the automaton's units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` could
    widen it. The conversions live in Controls.cpp, one function per control.
    Option, boolean and event parameters hold the element value itself; Seed
    and Audio Steps are real integers (`FF_TYPE_INTEGER`).

    Units: generations, generations per second, cells (rows of the grid), and
    probabilities as 32-bit integer thresholds (a hash below the threshold
    fires), so the GLSL and the harness compare integers and never round.
*/
namespace conway
{
/**
    Every control either plugin has, by what it is -- NOT by the index a host
    sees. Each plugin declares its own subset, densely, in the order of
    `HostOrder()`, with the About block LAST in both (radar's shape: the day a
    user guide adds a button, no control moves, and neither plugin declares a
    control it ignores).
*/
enum ParamId : unsigned int
{
	// -- Automaton -----------------------------------------------------------
	PT_RULE = 0,
	PT_EDGES,

	// -- Time ----------------------------------------------------------------
	PT_SPEED,
	PT_SMOOTH,
	PT_STEP,

	// -- Seeding -------------------------------------------------------------
	PT_PATTERN,
	PT_DENSITY,
	PT_SEED,
	PT_RESEED,
	PT_AUTO_RESEED,
	PT_NOISE,
	PT_THRESHOLD,///< Over only: how bright a part of the clip must be to be alive
	PT_FEED,     ///< Over only: births per generation where the clip is bright

	// -- Audio ---------------------------------------------------------------
	PT_AUDIO,
	PT_AUDIO_STEPS,
	PT_AUDIO_SEEDS,

	// -- Look ----------------------------------------------------------------
	PT_CELL_SIZE,
	PT_GAP,
	PT_PALETTE,
	PT_AGE_SPAN,
	PT_TRAIL,
	PT_CLIP_COLOUR,///< Over only
	PT_BACKDROP,   ///< Over only
	PT_MIX,        ///< Over only

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_TEXT,
	PT_COUNT = PT_ABOUT_TEXT + 1 + stoatworks::about::kButtonCount
};

/// The ids a plugin declares, in the order the host sees them. Host index i
/// is `HostOrder( effect )[ i ]`. The About block is last in both.
const std::vector< unsigned int >& HostOrder( bool effect );

/// The group each control is shown under.
const char* GroupOf( unsigned int id );

/// The display name of a control.
const char* NameOf( unsigned int id );

enum class Edges
{
	Torus = 0,///< the grid wraps: a glider leaving the right comes back on the left
	Dead,     ///< past the edge every cell is dead
	Count
};

/// What Reseed lays down. The source has the first six; the Over adds Clip,
/// LAST, so the shared patterns keep their indices in both plugins.
enum class Pattern
{
	Soup = 0,  ///< random cells at Density over the whole field
	RPentomino,///< five cells that boil for 1103 generations
	Acorn,     ///< seven cells that boil for 5206
	Diehard,   ///< seven cells that vanish at generation 130
	GosperGun, ///< a glider every 30 generations
	Gliders,   ///< a fleet: one glider per 16 x 16 block, with probability Density
	Clip,      ///< Over only: the clip's bright cells
	Count
};
int PatternCount( bool effect );
const char* PatternName( Pattern pattern );

enum class Palette
{
	Phosphor = 0,
	Heat,
	Ice,
	Mono,
	Spectrum,
	Count
};
const char* PaletteName( Palette palette );

/// The grid's rows: Cell Size 0 is a cell a pixel tall at 1080p, 1 is sixteen
/// rows. The columns follow the host's aspect (see `GridFor`).
constexpr int kMaxRows = 1080;
constexpr int kMinRows = 16;
/// A host frame runs at most this many generations (the clock drops the rest,
/// so at Speed 500 under 16 fps the automaton runs slower than Speed).
constexpr int kMaxStepsPerFrame = 32;
/// The age and the time since death are 16-bit counters in one R32UI texel.
constexpr uint32_t kSaturate = 0xFFFFu;

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------

/// Generations per second: 0 at the bottom (paused), then 0.5 to 500,
/// geometrically.
double SpeedFromParam( float v );
float ParamFromSpeed( double gensPerSecond );
/// The crossfade from the last generation into this one, as a fraction of a
/// generation's period. 0 = a hard cut on every generation.
double SmoothFromParam( float v );
/// The soup's fill, 0..1: the probability a cell starts alive (Soup) or a
/// 16 x 16 block holds a glider (Gliders).
double DensityFromParam( float v );
/// Spontaneous births per cell per generation: 0, then 1e-6 to 1e-2,
/// geometrically.
double NoiseFromParam( float v );
/// The clip's brightest channel x alpha above which a cell is alive.
double ThresholdFromParam( float v );
/// Births per generation where the clip is bright: Feed squared, so the low
/// end of the slider has room.
double FeedFromParam( float v );
/// A patch of soup per onset: its radius as a fraction of the rows, 0 to 1/4.
double AudioSeedRadiusFromParam( float v );
/// Rows of the grid, an integer from kMaxRows (v = 0) to kMinRows (v = 1),
/// geometrically.
int RowsFromParam( float v );
float ParamFromRows( int rows );
/// The dark gap round each lit cell, as a fraction of the cell: 0 to 0.5.
double GapFromParam( float v );
/// Generations from the newborn colour to the elder one: 1 to 1000.
double AgeSpanFromParam( float v );
float ParamFromAgeSpan( double generations );
/// The trail's time constant in generations: 0 (no trail) at the bottom,
/// then 0.5 to 200, geometrically.
double TrailFromParam( float v );
float ParamFromTrail( double generations );

/// A probability as the integer threshold a 32-bit hash is compared with:
/// round( p x 2^32 ), clamped to 2^32 - 1. The GLSL compares `pcg( .. ) <
/// threshold`, the harness the same, so neither ever converts to float.
uint32_t ThresholdU32( double probability );

struct Grid
{
	int cols = 0, rows = 0;
	bool operator==( const Grid& other ) const
	{
		return cols == other.cols && rows == other.rows;
	}
	bool operator!=( const Grid& other ) const
	{
		return !( *this == other );
	}
};
/// The grid for a raster: `rows` rows, and the columns that keep cells as
/// square as a whole number of them allows.
Grid GridFor( int rows, int width, int height );

int OptionIndex( float value, int count );

} // namespace conway
