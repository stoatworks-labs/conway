#pragma once

#include <algorithm>
#include <cmath>

/**
    When generations happen. No GL, so `cwtest --offline` checks it in CI.

    The host's elapsed time, in DOUBLE, times Speed is owed generations; each
    frame runs the whole ones and carries the fraction. So after any number of
    frames at any frame rate the count is floor( Speed x elapsed ) -- to a
    double's rounding, which is 1e-12 of a generation over hours -- and never
    drifts the way a per-frame float multiply would.

    Resolume's clock has been measured at ~499 million ms, where a float
    resolves 32 ms: the elapsed time is taken frame to frame in double by the
    plugin (Conway.cpp), never from the host's absolute value as a float.
*/
namespace conway
{
class GenerationClock
{
public:
	/// Owe Speed x dt more generations and take the whole ones, at most
	/// `maxSteps` (the rest is dropped, not banked: a host at 2 fps does not
	/// earn a burst of 250 generations on its next frame).
	int Advance( double gensPerSecond, double dt, int maxSteps )
	{
		if( gensPerSecond <= 0.0 || dt <= 0.0 )
			return 0;
		owed += gensPerSecond * dt;
		double whole = std::floor( owed );
		if( whole > maxSteps )
		{
			whole = maxSteps;
			owed  = std::min( owed - whole, 0.999999 );
		}
		else
			owed -= whole;
		return static_cast< int >( whole );
	}

	/// How far into the next generation the clock is, 0..1.
	double Fraction() const
	{
		return owed;
	}

	void Reset()
	{
		owed = 0.0;
	}

private:
	double owed = 0.0;
};

} // namespace conway
