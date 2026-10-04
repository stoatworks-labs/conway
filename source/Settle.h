#pragma once

#include <cstdint>
#include <deque>

/**
    Has the field settled? No GL, so `cwtest --offline` checks the law in CI.

    A soup on a torus ends as ash: still lifes, blinkers and the odd pulsar or
    pentadecathlon, with gliders wandering until they hit something. Every
    one of those has a population that is constant or periodic -- a glider is
    five cells in every phase, a blinker three, a beacon 6/8 (period 2), a
    pulsar 48/56/72 (3), a pentadecathlon's cycle is 15 -- so the population
    of the whole field repeats with a period dividing lcm( 2, 3, 15 ) = 30.

    The plugin counts the live cells of every generation exactly (an occlusion
    query, Conway.cpp) and feeds the counts here, in order. The field is
    SETTLED when the last `kWindow` counts each equal the count `p`
    generations earlier, for some p from 1 to kMaxPeriod -- or, sooner, when it
    has been empty for `kExtinct` generations. A soup still boiling changes
    its count far too irregularly to repeat exactly for 120 generations.
*/
namespace conway
{
class SettleDetector
{
public:
	static constexpr int kWindow    = 120;
	static constexpr int kMaxPeriod = 30;
	static constexpr int kExtinct   = 8;

	/// The population of `generation`. Counts must arrive in order; a gap
	/// (a generation that never arrived) starts the history again.
	/// Returns true on the count that makes the field settled.
	bool Push( uint64_t generation, uint32_t population )
	{
		if( !history.empty() && generation != last + 1 )
			history.clear();
		last = generation;
		history.push_back( population );
		while( history.size() > static_cast< size_t >( kWindow + kMaxPeriod ) )
			history.pop_front();

		int empty = 0;
		for( auto it = history.rbegin(); it != history.rend() && *it == 0u; ++it )
			++empty;
		if( empty >= kExtinct )
		{
			period = 0;
			return true;
		}

		const int n = static_cast< int >( history.size() );
		for( int p = 1; p <= maxPeriod; ++p )
		{
			if( n < kWindow + p )
				break;
			bool repeats = true;
			for( int i = n - kWindow; i < n && repeats; ++i )
				repeats = history[ static_cast< size_t >( i ) ] == history[ static_cast< size_t >( i - p ) ];
			if( repeats )
			{
				period = p;
				return true;
			}
		}
		return false;
	}

	void Reset()
	{
		history.clear();
		period = 0;
	}

	/// The period the last settle was found at (0: extinct, or not yet).
	int Period() const
	{
		return period;
	}

	/// --settle's negative control: look only for a constant population.
	void SetMaxPeriodForTest( int p )
	{
		maxPeriod = p;
	}

private:
	std::deque< uint32_t > history;
	uint64_t last = 0;
	int period    = 0;
	int maxPeriod = kMaxPeriod;
};

} // namespace conway
