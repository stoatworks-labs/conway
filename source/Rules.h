#pragma once

#include <cstdint>
#include <string>

/**
    The rules: Conway's B3/S23 and its family.

    A Life-like rule is two sets of neighbour counts (0..8, the Moore
    neighbourhood, the cell itself not counted): **B**, the counts at which a
    dead cell is born, and **S**, the counts at which a live one survives.
    Everything else dies or stays dead. A **Generations** rule adds C, the
    number of states: a cell that fails to survive spends C - 2 generations
    dying (refractory: it neither counts as a neighbour nor can be born)
    before it is dead. C = 2 is plain Life-like.

    Each rule is written down ONCE, as its notation (`B3/S23`, `B2/S/C3`), and
    parsed into the bitmasks the shader uses. A typo in a mask is then a typo
    in a published rule string, which `cwtest --literature` and `--rules`
    find by its behaviour, not by comparing it with a second copy.
*/
namespace conway::rules
{
struct Masks
{
	uint32_t birth   = 0;///< bit n set: a dead cell with n live neighbours is born
	uint32_t survive = 0;///< bit n set: a live cell with n live neighbours survives
	uint32_t states  = 2;///< Generations' C; 2 for a Life-like rule
	bool valid       = false;

	/// Generations a cell spends dying before it can be born again.
	uint32_t Refractory() const
	{
		return states > 2 ? states - 2 : 0;
	}
};

struct Rule
{
	const char* name;    ///< what the host's Rule dropdown shows
	const char* notation;///< B.../S...[/C...]
};

/// The Rule dropdown, in order. Conway first: index 0 is the default.
constexpr Rule kRules[] = {
	{ "Conway", "B3/S23" },
	{ "HighLife", "B36/S23" },
	{ "Day & Night", "B3678/S34678" },
	{ "Seeds", "B2/S" },
	{ "Life without Death", "B3/S012345678" },
	{ "Maze", "B3/S12345" },
	{ "Replicator", "B1357/S1357" },
	{ "Diamoeba", "B35678/S5678" },
	{ "2x2", "B36/S125" },
	{ "Morley", "B368/S245" },
	{ "Anneal", "B4678/S35678" },
	{ "Brian's Brain", "B2/S/C3" },
	{ "Star Wars", "B2/S345/C4" },
};
constexpr int kRuleCount = static_cast< int >( sizeof( kRules ) / sizeof( kRules[ 0 ] ) );

/// The index of a rule by name, or -1.
int IndexOf( const char* name );

/// Parse `B<digits>/S<digits>[/C<number>]`, case-insensitive, digits 0..8
/// once each, C from 2 to 65535. `valid` is false on anything else.
Masks Parse( const std::string& notation );

/// The parsed masks of kRules[ index ] (cached; index clamped).
const Masks& MasksOf( int index );

} // namespace conway::rules
