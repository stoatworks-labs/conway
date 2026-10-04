#pragma once

#include "Audio.h"
#include "Clock.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Settle.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace conway
{
/**
    The plugin: the source (a soup or a famous pattern) and, with `isEffect`,
    the Over effect (the clip seeds and feeds the field). One class, two
    registrations; see SourcePlugin.cpp and EffectPlugin.cpp.

    Host indices are NOT ParamIds: each plugin declares its own dense list,
    `HostOrder( isEffect )`. The host-facing calls translate; everything
    inside works in ParamIds.
*/
class ConwayPlugin : public CFFGLPlugin
{
public:
	explicit ConwayPlugin( bool isEffect );
	~ConwayPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	/// The base class's stub fails, and a failed default deletes the instance
	/// -- so without this no real host can load the plugin.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	FFResult SetTime( double time ) override;

	bool IsEffect() const
	{
		return isEffect;
	}
	unsigned int ParamCount() const
	{
		return static_cast< unsigned int >( hostOrder.size() );
	}
	/// The host index of a ParamId, or -1 if this plugin does not declare it.
	int HostIndexOf( unsigned int id ) const
	{
		return id < PT_COUNT ? idToHost[ id ] : -1;
	}
	/// Set by ParamId, as the host would through its index.
	void SetById( unsigned int id, float value );
	float GetById( unsigned int id ) const
	{
		return id < PT_COUNT ? params[ id ] : 0.0f;
	}

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these.
	//-------------------------------------------------------------------
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	double ClockScale() const
	{
		return clockScale;
	}
	/// A grid of exactly this size, whatever Cell Size and the raster say.
	void SetGridForTest( int cols, int rows )
	{
		gridOverride = Grid { cols, rows };
	}
	/// On the next frame, before any step: the state is exactly `cells`
	/// (cols x rows, row 0 at the bottom), in both buffers, generation 0.
	void LoadStateForTest( const std::vector< uint32_t >& cells )
	{
		pendingLoad = cells;
		loadPending = true;
	}
	/// On the next frame, run exactly this many generations more than the
	/// clock owes, with no per-frame cap.
	void RequestStepsForTest( int steps )
	{
		testSteps += steps;
	}
	/// Read every generation's population as soon as its query is issued
	/// (blocking), so the settle law runs at an exact generation.
	void SetBlockingCountsForTest( bool on )
	{
		blockingCounts = on;
	}
	/// Keep every (generation, population) the queries return.
	void SetPopulationLogForTest( bool on )
	{
		logPopulations = on;
	}
	const std::vector< std::pair< uint64_t, uint32_t > >& PopulationLog() const
	{
		return populationLog;
	}
	void ClearPopulationLogForTest()
	{
		populationLog.clear();
	}
	/// --patterns' negative control: the cell counts itself as a neighbour.
	void SetCountSelfForTest( bool on )
	{
		countSelfForTest = on;
	}
	/// --reference's negative control: the torus wraps one cell short.
	void SetWrapSkewForTest( int skew )
	{
		wrapSkewForTest = skew;
	}
	/// --rules' negative control: a rule's birth mask with one bit flipped.
	void SetBirthFlipForTest( uint32_t bits )
	{
		birthFlipForTest = bits;
	}
	/// --count's negative control: the count also takes the just-dead.
	void SetCountDyingForTest( bool on )
	{
		countDyingForTest = on;
	}
	/// --settle's negative control: look only for a constant population.
	void SetSettleMaxPeriodForTest( int period )
	{
		settle.SetMaxPeriodForTest( period );
	}
	/// --coverage's negative control: the cell under each pixel's centre.
	void SetPointSampleForTest( bool on )
	{
		pointSampleForTest = on;
	}
	/// --over-check's negative control: the clip laid under the grid mirrored.
	void SetMirrorForTest( bool on )
	{
		mirrorForTest = on;
	}
	/// --clock's negative control: elapsed time from the host clock in float.
	void SetFloatClockForTest( bool on )
	{
		floatClockForTest = on;
	}
	/// --resize's negative control: the state cleared on a resize.
	void SetClearOnResizeForTest( bool on )
	{
		clearOnResizeForTest = on;
	}
	/// --prime's negative control: the analyser deaf on its first frame too.
	void SetUnprimedForTest( bool on )
	{
		unprimed = on;
	}

	GLuint StateTextureID() const
	{
		return current.TextureID();
	}
	GLuint PreviousTextureID() const
	{
		return previous.TextureID();
	}
	Grid CurrentGrid() const
	{
		return grid;
	}
	/// Generations since the field was last seeded (or loaded).
	uint64_t Generation() const
	{
		return generation;
	}
	/// Every generation this instance has run.
	uint64_t TotalGenerations() const
	{
		return totalGenerations;
	}
	unsigned long long Reseeds() const
	{
		return reseeds;
	}
	/// The generation whose count found the field settled, last time.
	uint64_t LastSettledGeneration() const
	{
		return lastSettled;
	}
	int LastSettledPeriod() const
	{
		return lastSettledPeriod;
	}
	unsigned long long Onsets() const
	{
		return onsetsUsed;
	}
	double LastDt() const
	{
		return frameDt;
	}
	double LastPhase() const
	{
		return phase;
	}

private:
	void UpdateClock();
	bool ensureBuffers( const Grid& want );
	void seed();
	void step( bool ruleOn, bool withPatch );
	void count();
	void collectCounts( bool wait );
	void composite( const FFGLTextureStruct* input, const GLint* hostViewport, GLuint hostFBO, int width,
	                int height );
	uint32_t seedSalt() const;

	const bool isEffect;
	const std::vector< unsigned int >& hostOrder;
	int idToHost[ PT_COUNT ];
	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader seedShader, copyShader, stepShader, countShader, compositeShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer current, previous;
	PassBuffer oldCurrent, oldPrevious;
	PassBuffer countTarget;
	GLuint stampTexture = 0, blankClip = 0;
	int stampSize[ 2 ]  = { 0, 0 };
	/// This frame's clip (Over; 0 in the source) and its raster.
	GLuint clipTexture  = 0;
	int clipRaster[ 2 ] = { 1, 1 };
	Grid grid;
	Grid gridOverride;

	//Time. Resolume has sent both seconds and milliseconds (see millpond).
	double hostTime = -1.0, lastRawTime = -1.0, lastWallTime = -1.0, wallStart = -1.0;
	double clockScale = 0.0;
	int secondsVotes = 0, millisVotes = 0;
	double now = 0.0, lastNow = -1.0;
	bool settledJump = false;
	double frameDt   = 0.0;
	GenerationClock clock;
	/// Seconds since the last generation, for Smooth.
	double sinceStep = 0.0;
	double phase     = 1.0;

	//The field.
	uint64_t generation       = 0;
	uint64_t totalGenerations = 0;
	uint32_t epoch            = 0;
	bool needSeed             = true;
	uint32_t seedSerial       = 0;
	int lastPattern           = -1;
	int lastSeed              = -1;
	bool stepHeld = false, reseedHeld = false;
	int pendingSteps = 0;
	int testSteps    = 0;
	std::vector< uint32_t > pendingLoad;
	bool loadPending = false;
	int lastWidth = 0, lastHeight = 0;

	//The audio patch.
	bool patchPending = false;
	int patchCentre[ 2 ] = { 0, 0 };
	int patchRadius      = 0;
	uint32_t patchSalt   = 0;

	//Populations, by occlusion query, read back when they are ready.
	struct PendingCount
	{
		GLuint query;
		uint64_t generation;
		uint32_t epoch;
	};
	std::deque< PendingCount > pendingCounts;
	std::vector< GLuint > freeQueries;
	SettleDetector settle;
	unsigned long long reseeds = 0;
	uint64_t lastSettled       = 0;
	int lastSettledPeriod      = -1;
	std::vector< std::pair< uint64_t, uint32_t > > populationLog;

	audio::Analyser analyser;
	bool unprimed = false;
	unsigned long long onsetsUsed = 0;

	bool blockingCounts       = false;
	bool logPopulations       = false;
	bool countSelfForTest     = false;
	int wrapSkewForTest       = 0;
	uint32_t birthFlipForTest = 0;
	bool countDyingForTest    = false;
	bool pointSampleForTest   = false;
	bool mirrorForTest        = false;
	bool floatClockForTest    = false;
	bool clearOnResizeForTest = false;
};

} // namespace conway
