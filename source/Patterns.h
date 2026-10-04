#pragma once

#include <vector>

/**
    Patterns, written the way the Life community writes them: RLE, `b` a dead
    cell, `o` a live one, `$` the end of a row, a number in front repeating
    what follows, `!` the end. Rows run top to bottom as written; the plugin
    stamps them the right way up on screen (the state's row 0 is the BOTTOM,
    GL's order, so row y of the RLE lands on row height - 1 - y).

    The four famous ones the Pattern dropdown offers are here, from the
    LifeWiki's RLE. The harness checks each against the literature on an
    unbounded plane (`--literature`) and through the plugin (`--patterns`).
*/
namespace conway::patterns
{
struct Cell
{
	int x, y;///< x right, y DOWN, as written
};

struct Shape
{
	int width = 0, height = 0;
	std::vector< Cell > cells;
};

/// Parse RLE (the body only, no `x = ...` header). Unknown characters are
/// ignored; parsing stops at `!`.
Shape Parse( const char* rle );

/// The R-pentomino: 1103 generations to stabilise, 116 cells then.
extern const char* const kRPentomino;
/// The acorn: 5206 generations, 633 cells.
extern const char* const kAcorn;
/// Diehard: gone at generation 130.
extern const char* const kDiehard;
/// Bill Gosper's glider gun, 1970: period 30, one glider per period.
extern const char* const kGosperGun;
/// A glider, heading down and right as written.
extern const char* const kGlider;

} // namespace conway::patterns
