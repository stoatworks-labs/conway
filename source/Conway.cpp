#include "Conway.h"

#include "Diag.h"
#include "GLState.h"
#include "Hash.h"
#include "Patterns.h"
#include "Rules.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace conway
{
namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

constexpr int kClockVotes = 4;
/// Host seconds. A bigger forward step, or any backward one, is a jump -- a
/// clip trigger or a scrub -- and no time passes across it. One second, as
/// radar: an automaton at 2 fps should still run.
constexpr double kMaxFrameDelta = 1.0;
/// Queries in flight before the oldest are given up on (a driver that never
/// answers must not grow this without bound).
constexpr size_t kMaxPendingCounts = 512;
/// Smooth's crossfade when nothing sets a period (Speed 0, stepping by hand).
constexpr double kPausedPeriod = 0.25;

const char* const kEdgeNames[] = { "Torus", "Dead" };

struct PaletteLook
{
	float newborn[ 3 ], elder[ 3 ], trail[ 3 ];
};

const PaletteLook& lookOf( Palette palette )
{
	//Linear light. A newborn is the brightest a cell gets; the elder colour
	//is where a cell settles after Age Span generations, so still lifes (old)
	//and the boiling front (young) read apart at a glance.
	static const PaletteLook looks[] = {
		{ { 0.80f, 1.00f, 0.80f }, { 0.08f, 0.70f, 0.22f }, { 0.04f, 0.30f, 0.08f } },//Phosphor
		{ { 1.00f, 0.95f, 0.70f }, { 0.80f, 0.12f, 0.02f }, { 0.50f, 0.10f, 0.02f } },//Heat
		{ { 0.92f, 0.98f, 1.00f }, { 0.10f, 0.35f, 0.95f }, { 0.04f, 0.14f, 0.42f } },//Ice
		{ { 1.00f, 1.00f, 1.00f }, { 1.00f, 1.00f, 1.00f }, { 0.50f, 0.50f, 0.50f } },//Mono
		{ { 1.00f, 1.00f, 1.00f }, { 1.00f, 1.00f, 1.00f }, { 0.30f, 0.30f, 0.36f } },//Spectrum (hue by age)
	};
	return looks[ static_cast< int >( palette ) ];
}

void setUint( FFGLShader& shader, const char* name, uint32_t value )
{
	glUniform1ui( glGetUniformLocation( shader.GetGLID(), name ), value );
}

void setIvec2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( glGetUniformLocation( shader.GetGLID(), name ), x, y );
}

int floorDiv( int a, int b )
{
	return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
}

const char* stampOf( Pattern pattern )
{
	switch( pattern )
	{
	case Pattern::RPentomino: return patterns::kRPentomino;
	case Pattern::Acorn: return patterns::kAcorn;
	case Pattern::Diehard: return patterns::kDiehard;
	case Pattern::GosperGun: return patterns::kGosperGun;
	default: return nullptr;
	}
}
} // namespace

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
ConwayPlugin::ConwayPlugin( bool effect ) :
	isEffect( effect ),
	hostOrder( HostOrder( effect ) )
{
	SetMinInputs( isEffect ? 1 : 0 );
	SetMaxInputs( isEffect ? 1 : 0 );
	SetTimeSupported( true );

	std::fill( std::begin( idToHost ), std::end( idToHost ), -1 );
	for( size_t i = 0; i < hostOrder.size(); ++i )
		idToHost[ hostOrder[ i ] ] = static_cast< int >( i );

	//-------------------------------------------------------------------
	// Defaults. The source: Conway on a torus, a 35% soup, 15 generations a
	// second, cells 8 px tall at 1080p, re-seeding itself when the soup has
	// burnt down to ash. The Over: the clip's bright cells as the seed, Feed
	// breeding more where it stays bright, the clip dimmed behind.
	//-------------------------------------------------------------------
	params[ PT_RULE ]        = 0.0f;
	params[ PT_EDGES ]       = static_cast< float >( Edges::Torus );
	params[ PT_SPEED ]       = ParamFromSpeed( 15.0 );
	params[ PT_SMOOTH ]      = 0.35f;
	params[ PT_PATTERN ]     = static_cast< float >( isEffect ? Pattern::Clip : Pattern::Soup );
	params[ PT_DENSITY ]     = 0.35f;
	params[ PT_SEED ]        = 1.0f;
	params[ PT_AUTO_RESEED ] = 1.0f;
	params[ PT_NOISE ]       = 0.0f;
	params[ PT_THRESHOLD ]   = 0.5f;
	params[ PT_FEED ]        = 0.3f;
	params[ PT_AUDIO_STEPS ] = 0.0f;
	params[ PT_AUDIO_SEEDS ] = 0.0f;
	params[ PT_CELL_SIZE ]   = ParamFromRows( 135 );
	params[ PT_GAP ]         = 0.24f;
	params[ PT_PALETTE ]     = static_cast< float >( Palette::Phosphor );
	params[ PT_AGE_SPAN ]    = ParamFromAgeSpan( 40.0 );
	params[ PT_TRAIL ]       = ParamFromTrail( 3.0 );
	params[ PT_CLIP_COLOUR ] = 0.0f;
	params[ PT_BACKDROP ]    = 0.35f;
	params[ PT_MIX ]         = 1.0f;

	for( unsigned int host = 0; host < hostOrder.size(); ++host )
	{
		const unsigned int id = hostOrder[ host ];
		const char* name      = NameOf( id );
		switch( id )
		{
		case PT_RULE:
			SetOptionParamInfo( host, name, rules::kRuleCount, params[ id ] );
			for( int i = 0; i < rules::kRuleCount; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), rules::kRules[ i ].name, static_cast< float >( i ) );
			break;
		case PT_EDGES:
			SetOptionParamInfo( host, name, 2, params[ id ] );
			for( int i = 0; i < 2; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), kEdgeNames[ i ], static_cast< float >( i ) );
			break;
		case PT_PATTERN:
		{
			const int count = PatternCount( isEffect );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), PatternName( static_cast< Pattern >( i ) ),
				                     static_cast< float >( i ) );
			break;
		}
		case PT_PALETTE:
		{
			const int count = static_cast< int >( Palette::Count );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), PaletteName( static_cast< Palette >( i ) ),
				                     static_cast< float >( i ) );
			break;
		}
		case PT_SEED:
			//Only FF_TYPE_STANDARD has its default clamped into 0..1, so an
			//integer is declared with its real default and range.
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, 9999.0f );
			break;
		case PT_AUDIO_STEPS:
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, 8.0f );
			break;
		case PT_STEP:
		case PT_RESEED:
			SetParamInfo( host, name, FF_TYPE_EVENT, false );
			break;
		case PT_AUTO_RESEED:
			SetParamInfo( host, name, FF_TYPE_BOOLEAN, params[ id ] > 0.5f );
			break;
		case PT_AUDIO:
			//An FFT buffer: Resolume shows it as an audio-source picker.
			SetBufferParamInfo( host, name, audio::kBins, FF_USAGE_FFT );
			for( int i = 0; i < audio::kBins; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), "", 0.0f );
			break;
		case PT_ABOUT_TEXT:
			SetParamInfo( host, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
			break;
		default:
			if( id > PT_ABOUT_TEXT )
				SetParamInfo( host, stoatworks::about::buttons()[ id - PT_ABOUT_TEXT - 1 ].label, FF_TYPE_EVENT, false );
			else
				SetParamInfo( host, name, FF_TYPE_STANDARD, params[ id ] );
			break;
		}
		SetParamGroup( host, GroupOf( id ) );
	}
}

ConwayPlugin::~ConwayPlugin() = default;

//---------------------------------------------------------------------------
FFResult ConwayPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	using namespace shaders;
	const std::string quadVertex = std::string( kVersion ) + kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &seedShader, Assemble( kClipCommon, kSeedFragment ), "seed" },
		{ &copyShader, Assemble( kCopyFragment ), "copy" },
		{ &stepShader, Assemble( kClipCommon, kStepFragment ), "step" },
		{ &countShader, Assemble( kCountFragment ), "count" },
		{ &compositeShader, Assemble( kCompositeFragment ), "composite" },
	};
	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( quadVertex.c_str(), stage.fragment.c_str() ) )
			continue;
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Conway: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//A sampler bound to texture 0 is "unloadable" to Apple's GL (boreal's
	//trap): the source has no clip, so its clip samplers get this.
	glGenTextures( 1, &blankClip );
	glBindTexture( GL_TEXTURE_2D, blankClip );
	const unsigned char black[ 4 ] = { 0, 0, 0, 0 };
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glGenTextures( 1, &stampTexture );
	glBindTexture( GL_TEXTURE_2D, stampTexture );
	const GLuint none = 0;
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32UI, 1, 1, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &none );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	stampSize[ 0 ] = stampSize[ 1 ] = 1;

	diag::info( isEffect ? "initialised (Over)" : "initialised (source)" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void ConwayPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;
	const double raw = hostTime;

	//Resolume has been seen sending seconds and milliseconds through SetTime:
	//vote on the unit against the wall clock (rosette's code, via radar).
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale  = millisVotes > secondsVotes ? 0.001 : 1.0;
				settledJump = true;
			}
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;
	now          = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

uint32_t ConwayPlugin::seedSalt() const
{
	const uint32_t seed = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	return Pcg( seed * 2654435761u ^ Pcg( seedSerial + 0x9e3779b9u ) );
}

//---------------------------------------------------------------------------
bool ConwayPlugin::ensureBuffers( const Grid& want )
{
	if( current.IsValid() && ( current.Width() != want.cols || current.Height() != want.rows ) )
	{
		//The state moves out of the way; the new pair is allocated; the
		//overlap is copied back in (ProcessOpenGL) and the old freed.
		current.MoveTo( oldCurrent );
		previous.MoveTo( oldPrevious );
	}
	bool ok = current.Ensure( want.cols, want.rows, GL_R32UI );
	ok      = ok && previous.Ensure( want.cols, want.rows, GL_R32UI );
	ok      = ok && countTarget.Ensure( want.cols, want.rows, GL_R8 );
	return ok;
}

//---------------------------------------------------------------------------
void ConwayPlugin::seed()
{
	const Pattern pattern = static_cast< Pattern >( OptionIndex( params[ PT_PATTERN ], PatternCount( isEffect ) ) );
	int mode              = 0;
	if( pattern == Pattern::Gliders )
		mode = 2;
	else if( pattern == Pattern::Clip )
		mode = 3;
	else if( stampOf( pattern ) )
		mode = 1;

	int origin[ 2 ] = { 0, 0 };
	if( mode == 1 )
	{
		//The pattern as a stamp, right way up (its first row on top), centred.
		const patterns::Shape shape = patterns::Parse( stampOf( pattern ) );
		std::vector< GLuint > cells( static_cast< size_t >( shape.width ) * shape.height, 0u );
		for( const patterns::Cell& cell : shape.cells )
			cells[ static_cast< size_t >( shape.height - 1 - cell.y ) * shape.width + cell.x ] = 1u;
		GLint alignment = 4;
		glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glBindTexture( GL_TEXTURE_2D, stampTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_R32UI, shape.width, shape.height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, cells.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
		stampSize[ 0 ] = shape.width;
		stampSize[ 1 ] = shape.height;
		origin[ 0 ]    = floorDiv( grid.cols - shape.width, 2 );
		origin[ 1 ]    = floorDiv( grid.rows - shape.height, 2 );
	}

	ScopedShaderBinding shader( seedShader.GetGLID() );
	bindUnit( 0, stampTexture );
	bindUnit( 1, clipTexture != 0 ? clipTexture : blankClip );
	seedShader.Set( "Stamp", 0 );
	seedShader.Set( "Clip", 1 );
	setIvec2( seedShader, "Grid", grid.cols, grid.rows );
	seedShader.Set( "Mode", mode );
	setUint( seedShader, "DensityU", ThresholdU32( DensityFromParam( params[ PT_DENSITY ] ) ) );
	setUint( seedShader, "Salt", seedSalt() );
	setIvec2( seedShader, "StampOrigin", origin[ 0 ], origin[ 1 ] );
	setIvec2( seedShader, "StampSize", stampSize[ 0 ], stampSize[ 1 ] );
	setIvec2( seedShader, "ClipRaster", clipRaster[ 0 ], clipRaster[ 1 ] );
	seedShader.Set( "Threshold", static_cast< float >( ThresholdFromParam( params[ PT_THRESHOLD ] ) ) );
	seedShader.Set( "MirrorForTest", mirrorForTest ? 1 : 0 );
	//Both buffers: the crossfade from the last generation starts from this one.
	for( PassBuffer* target : { &current, &previous } )
	{
		target->BindForDrawing();
		quad.Draw();
	}
	unbindTextureUnits( 2 );

	generation = 0;
	++epoch;
	settle.Reset();
	count();
}

//---------------------------------------------------------------------------
void ConwayPlugin::step( bool ruleOn, bool withPatch )
{
	const rules::Masks& masks = rules::MasksOf( OptionIndex( params[ PT_RULE ], rules::kRuleCount ) );
	const uint32_t salt       = seedSalt();
	const uint32_t serial     = static_cast< uint32_t >( totalGenerations );
	const uint32_t feedU      = isEffect ? ThresholdU32( FeedFromParam( params[ PT_FEED ] ) ) : 0u;

	previous.BindForDrawing();
	{
		ScopedShaderBinding shader( stepShader.GetGLID() );
		bindUnit( 0, current.TextureID() );
		bindUnit( 1, clipTexture != 0 ? clipTexture : blankClip );
		stepShader.Set( "Current", 0 );
		stepShader.Set( "Clip", 1 );
		setIvec2( stepShader, "Grid", grid.cols, grid.rows );
		stepShader.Set( "Torus", OptionIndex( params[ PT_EDGES ], 2 ) == static_cast< int >( Edges::Torus ) ? 1 : 0 );
		setUint( stepShader, "Birth", masks.birth ^ birthFlipForTest );
		setUint( stepShader, "Survive", masks.survive );
		setUint( stepShader, "Refractory", masks.Refractory() );
		stepShader.Set( "RuleOn", ruleOn ? 1 : 0 );
		setUint( stepShader, "NoiseU", ruleOn ? ThresholdU32( NoiseFromParam( params[ PT_NOISE ] ) ) : 0u );
		setUint( stepShader, "NoiseSalt", Pcg( salt ^ Pcg( serial * 2u + 1u ) ) );
		stepShader.Set( "FeedOn", ruleOn && feedU != 0u && clipTexture != 0 ? 1 : 0 );
		setUint( stepShader, "FeedU", feedU );
		setUint( stepShader, "FeedSalt", Pcg( salt ^ Pcg( serial * 2u + 2u ) ) );
		setIvec2( stepShader, "ClipRaster", clipRaster[ 0 ], clipRaster[ 1 ] );
		stepShader.Set( "Threshold", static_cast< float >( ThresholdFromParam( params[ PT_THRESHOLD ] ) ) );
		stepShader.Set( "MirrorForTest", mirrorForTest ? 1 : 0 );
		stepShader.Set( "PatchRadius", withPatch ? patchRadius : 0 );
		setIvec2( stepShader, "PatchCentre", patchCentre[ 0 ], patchCentre[ 1 ] );
		setUint( stepShader, "PatchU", ThresholdU32( DensityFromParam( params[ PT_DENSITY ] ) ) );
		setUint( stepShader, "PatchSalt", patchSalt );
		stepShader.Set( "CountSelfForTest", countSelfForTest ? 1 : 0 );
		stepShader.Set( "WrapSkewForTest", wrapSkewForTest );
		quad.Draw();
		unbindTextureUnits( 2 );
	}
	//The new generation is current; the one before it stays as previous, for
	//the crossfade.
	std::swap( current, previous );
}

//---------------------------------------------------------------------------
void ConwayPlugin::count()
{
	GLuint query = 0;
	if( !freeQueries.empty() )
	{
		query = freeQueries.back();
		freeQueries.pop_back();
	}
	else
		glGenQueries( 1, &query );

	countTarget.BindForDrawing();
	glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	{
		ScopedShaderBinding shader( countShader.GetGLID() );
		bindUnit( 0, current.TextureID() );
		countShader.Set( "Current", 0 );
		countShader.Set( "CountDyingForTest", countDyingForTest ? 1 : 0 );
		glBeginQuery( GL_SAMPLES_PASSED, query );
		quad.Draw();
		glEndQuery( GL_SAMPLES_PASSED );
		unbindTextureUnits( 1 );
	}
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	pendingCounts.push_back( PendingCount { query, generation, epoch } );
	while( pendingCounts.size() > kMaxPendingCounts )
	{
		freeQueries.push_back( pendingCounts.front().query );
		pendingCounts.pop_front();
		settle.Reset();
	}
	if( blockingCounts )
		collectCounts( true );
}

void ConwayPlugin::collectCounts( bool wait )
{
	while( !pendingCounts.empty() )
	{
		const PendingCount pending = pendingCounts.front();
		if( pending.epoch != epoch )
		{
			//A field that has since been reseeded, loaded or re-gridded: its
			//counts say nothing about this one. A query object may be begun
			//again before its result is read.
			freeQueries.push_back( pending.query );
			pendingCounts.pop_front();
			continue;
		}
		if( !wait )
		{
			GLuint available = 0;
			glGetQueryObjectuiv( pending.query, GL_QUERY_RESULT_AVAILABLE, &available );
			if( !available )
				break;
		}
		GLuint population = 0;
		glGetQueryObjectuiv( pending.query, GL_QUERY_RESULT, &population );
		freeQueries.push_back( pending.query );
		pendingCounts.pop_front();

		if( logPopulations )
			populationLog.emplace_back( pending.generation, population );
		if( settle.Push( pending.generation, population ) && params[ PT_AUTO_RESEED ] > 0.5f && !needSeed )
		{
			needSeed          = true;
			lastSettled       = pending.generation;
			lastSettledPeriod = settle.Period();
			++seedSerial;
			++reseeds;
			++epoch;
			settle.Reset();
		}
	}
}

//---------------------------------------------------------------------------
FFResult ConwayPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct* input = nullptr;
	if( isEffect )
	{
		if( pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		input = pgl->inputTextures[ 0 ];
	}

	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = input ? static_cast< int >( input->Width ) : hostViewport[ 2 ];
	const int height          = input ? static_cast< int >( input->Height ) : hostViewport[ 3 ];
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );
	clipTexture     = input ? input->Handle : 0;
	clipRaster[ 0 ] = width;
	clipRaster[ 1 ] = height;

	//-------------------------------------------------------------------
	// Time: the host's, as real elapsed seconds, frame to frame, in double.
	// A backwards or large step is a jump: no time passes across it, and the
	// audio analyser starts over (primed, so the loud frame it lands on fires
	// nothing).
	//-------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	double dt = 0.0;
	if( lastNow >= 0.0 )
	{
		const double stepTime = floatClockForTest
		                            ? static_cast< double >( static_cast< float >( now ) - static_cast< float >( lastNow ) )
		                            : now - lastNow;
		if( stepTime < 0.0 || stepTime > kMaxFrameDelta )
			analyser.Reset();
		else
			dt = stepTime;
	}
	lastNow = now;
	frameDt = dt;

	//-------------------------------------------------------------------
	// The populations that have come back, which may settle the field.
	//-------------------------------------------------------------------
	collectCounts( false );

	//-------------------------------------------------------------------
	// Audio: an onset is Audio Steps generations and a patch of soup.
	//-------------------------------------------------------------------
	{
		float bins[ audio::kBins ] = {};
		int binCount               = 0;
		if( const ParamInfo* info = FindParamInfo( static_cast< unsigned int >( idToHost[ PT_AUDIO ] ) ) )
		{
			binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < binCount; ++i )
				bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		}
		audio::Settings settings;
		analyser.SetPrimingForTest( !unprimed );
		analyser.Update( bins, binCount, static_cast< float >( dt ), settings );
		const int audioSteps   = std::clamp( static_cast< int >( std::lround( params[ PT_AUDIO_STEPS ] ) ), 0, 8 );
		const double seedShare = AudioSeedRadiusFromParam( params[ PT_AUDIO_SEEDS ] );
		if( analyser.Fired() && ( audioSteps > 0 || seedShare > 0.0 ) )
		{
			++onsetsUsed;
			pendingSteps += audioSteps;
			if( seedShare > 0.0 && grid.rows > 0 )
			{
				const uint32_t h = Pcg( seedSalt() ^ Pcg( static_cast< uint32_t >( onsetsUsed ) * 7919u ) );
				patchRadius      = std::max( 1, static_cast< int >( std::lround( seedShare * grid.rows ) ) );
				const int spanX  = std::max( 1, grid.cols - 2 * patchRadius );
				const int spanY  = std::max( 1, grid.rows - 2 * patchRadius );
				patchCentre[ 0 ] = grid.cols > 2 * patchRadius ? patchRadius + static_cast< int >( h % static_cast< uint32_t >( spanX ) ) : grid.cols / 2;
				patchCentre[ 1 ] = grid.rows > 2 * patchRadius ? patchRadius + static_cast< int >( Pcg( h ) % static_cast< uint32_t >( spanY ) ) : grid.rows / 2;
				patchSalt        = Pcg( h ^ 0x5bd1e995u );
				patchPending     = true;
			}
		}
	}

	//-------------------------------------------------------------------
	// The buttons (a press is the rising edge), and what reseeds.
	//-------------------------------------------------------------------
	{
		const bool stepDown = params[ PT_STEP ] > 0.5f;
		if( stepDown && !stepHeld )
			++pendingSteps;
		stepHeld = stepDown;
		//A new Seed starts its sequence of soups again; a Reseed press in the
		//same frame then takes the next one. The other way round, the Seed
		//change would swallow the press (the sweep found it: Reseed held from
		//the first frame did nothing, because the first frame is a Seed change).
		const int seedValue = static_cast< int >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
		if( seedValue != lastSeed )
		{
			seedSerial = 0;
			needSeed   = true;
		}
		lastSeed              = seedValue;
		const bool reseedDown = params[ PT_RESEED ] > 0.5f;
		if( reseedDown && !reseedHeld )
		{
			++seedSerial;
			needSeed = true;
		}
		reseedHeld        = reseedDown;
		const int pattern = OptionIndex( params[ PT_PATTERN ], PatternCount( isEffect ) );
		if( pattern != lastPattern )
			needSeed = true;
		lastPattern = pattern;
	}

	//-------------------------------------------------------------------
	// How many generations this frame: what the clock owes, then the
	// presses and onsets, at most kMaxStepsPerFrame; the harness's on top.
	//-------------------------------------------------------------------
	const double speed   = SpeedFromParam( params[ PT_SPEED ] );
	const int clockSteps = clock.Advance( speed, dt, kMaxStepsPerFrame );
	int steps            = std::min( clockSteps + pendingSteps, kMaxStepsPerFrame ) + testSteps;
	if( clockSteps > 0 )
		sinceStep = clock.Fraction() / speed;
	else if( steps > 0 )
		sinceStep = 0.0;
	else
		sinceStep += dt;
	pendingSteps = 0;
	testSteps    = 0;

	//-------------------------------------------------------------------
	// Everything allocated before anything is bound. The grid follows Cell
	// Size and the raster's ASPECT, not its size: a resize of the same shape
	// leaves it alone, and a new shape keeps the overlap, centred.
	//-------------------------------------------------------------------
	const Grid want = gridOverride.cols > 0 ? gridOverride : GridFor( RowsFromParam( params[ PT_CELL_SIZE ] ), width, height );
	const bool first = !current.IsValid();
	if( !ensureBuffers( want ) )
	{
		diag::error( "could not allocate the state buffers" );
		return FF_FAIL;
	}
	if( oldCurrent.IsValid() )
	{
		ScopedShaderBinding shader( copyShader.GetGLID() );
		copyShader.Set( "Old", 0 );
		setIvec2( copyShader, "OldGrid", oldCurrent.Width(), oldCurrent.Height() );
		setIvec2( copyShader, "Offset", floorDiv( oldCurrent.Width() - want.cols, 2 ), floorDiv( oldCurrent.Height() - want.rows, 2 ) );
		for( auto pair : { std::make_pair( &oldCurrent, &current ), std::make_pair( &oldPrevious, &previous ) } )
		{
			pair.second->BindForDrawing();
			bindUnit( 0, pair.first->TextureID() );
			quad.Draw();
		}
		unbindTextureUnits( 1 );
		oldCurrent.Destroy();
		oldPrevious.Destroy();
	}
	const bool regridded = !first && want != grid;
	grid                 = want;
	if( ( width != lastWidth || height != lastHeight ) && lastWidth != 0 && clearOnResizeForTest )
	{
		current.Clear();
		previous.Clear();
	}
	lastWidth  = width;
	lastHeight = height;

	//-------------------------------------------------------------------
	// The field: seeded, loaded or carried over a re-grid; then the steps.
	//-------------------------------------------------------------------
	if( loadPending && pendingLoad.size() == static_cast< size_t >( grid.cols ) * grid.rows )
	{
		current.Upload( pendingLoad.data() );
		previous.Upload( pendingLoad.data() );
		loadPending = false;
		needSeed    = false;
		generation  = 0;
		++epoch;
		settle.Reset();
		count();
	}
	else if( needSeed )
	{
		needSeed = false;
		seed();
	}
	else if( regridded )
	{
		//The overlap is the same field, cropped or padded: its history of
		//populations is not.
		++epoch;
		settle.Reset();
		count();
	}

	for( int i = 0; i < steps && !needSeed; ++i )
	{
		step( true, patchPending );
		patchPending = false;
		++generation;
		++totalGenerations;
		count();
	}
	if( patchPending && !needSeed )
	{
		//An onset while paused still drops its patch, without a generation.
		step( false, true );
		patchPending = false;
	}

	//-------------------------------------------------------------------
	// Smooth: how far the crossfade from the last generation has got.
	//-------------------------------------------------------------------
	{
		const double smooth = SmoothFromParam( params[ PT_SMOOTH ] );
		const double period = speed > 0.0 ? 1.0 / speed : kPausedPeriod;
		phase               = smooth <= 0.0 ? 1.0 : std::clamp( sinceStep / ( smooth * period ), 0.0, 1.0 );
	}

	composite( input, hostViewport, pgl->HostFBO, width, height );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void ConwayPlugin::composite( const FFGLTextureStruct* input, const GLint* hostViewport, GLuint hostFBO, int width,
                              int height )
{
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	const Palette palette   = static_cast< Palette >( OptionIndex( params[ PT_PALETTE ], static_cast< int >( Palette::Count ) ) );
	const PaletteLook& look = lookOf( palette );
	ScopedShaderBinding shader( compositeShader.GetGLID() );
	bindUnit( 0, current.TextureID() );
	bindUnit( 1, previous.TextureID() );
	bindUnit( 2, input ? input->Handle : blankClip );
	compositeShader.Set( "Current", 0 );
	compositeShader.Set( "Previous", 1 );
	compositeShader.Set( "InputTexture", 2 );
	setIvec2( compositeShader, "Grid", grid.cols, grid.rows );
	compositeShader.Set( "Raster", static_cast< float >( width ), static_cast< float >( height ) );
	compositeShader.Set( "ViewOrigin", static_cast< float >( hostViewport[ 0 ] ), static_cast< float >( hostViewport[ 1 ] ) );
	compositeShader.Set( "Phase", static_cast< float >( phase ) );
	compositeShader.Set( "Gap", static_cast< float >( GapFromParam( params[ PT_GAP ] ) ) );
	compositeShader.Set( "AgeSpan", static_cast< float >( AgeSpanFromParam( params[ PT_AGE_SPAN ] ) ) );
	compositeShader.Set( "Trail", static_cast< float >( TrailFromParam( params[ PT_TRAIL ] ) ) );
	compositeShader.Set( "Spectrum", palette == Palette::Spectrum ? 1 : 0 );
	compositeShader.Set( "Newborn", look.newborn[ 0 ], look.newborn[ 1 ], look.newborn[ 2 ] );
	compositeShader.Set( "Elder", look.elder[ 0 ], look.elder[ 1 ], look.elder[ 2 ] );
	compositeShader.Set( "TrailColour", look.trail[ 0 ], look.trail[ 1 ], look.trail[ 2 ] );
	compositeShader.Set( "IsEffect", isEffect ? 1 : 0 );
	compositeShader.Set( "ClipColour", isEffect ? std::clamp( params[ PT_CLIP_COLOUR ], 0.0f, 1.0f ) : 0.0f );
	compositeShader.Set( "Backdrop", isEffect ? std::clamp( params[ PT_BACKDROP ], 0.0f, 1.0f ) : 0.0f );
	compositeShader.Set( "MixAmount", isEffect ? std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) : 1.0f );
	compositeShader.Set( "PointSampleForTest", pointSampleForTest ? 1 : 0 );
	quad.Draw();
	unbindTextureUnits( 3 );
}

//---------------------------------------------------------------------------
FFResult ConwayPlugin::DeInitGL()
{
	for( FFGLShader* shader : { &seedShader, &copyShader, &stepShader, &countShader, &compositeShader } )
		shader->FreeGLResources();
	quad.Release();
	for( PassBuffer* buffer : { &current, &previous, &oldCurrent, &oldPrevious, &countTarget } )
		buffer->Destroy();
	for( GLuint* texture : { &stampTexture, &blankClip } )
		if( *texture )
		{
			glDeleteTextures( 1, texture );
			*texture = 0;
		}
	for( const PendingCount& pending : pendingCounts )
		freeQueries.push_back( pending.query );
	pendingCounts.clear();
	if( !freeQueries.empty() )
		glDeleteQueries( static_cast< GLsizei >( freeQueries.size() ), freeQueries.data() );
	freeQueries.clear();
	needSeed = true;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult ConwayPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

char* ConwayPlugin::GetTextParameter( unsigned int index )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult ConwayPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult ConwayPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= hostOrder.size() )
		return FF_FAIL;
	const unsigned int id = hostOrder[ index ];
	if( id >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( id - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	params[ id ] = value;
	return FF_SUCCESS;
}

float ConwayPlugin::GetFloatParameter( unsigned int index )
{
	return index < hostOrder.size() ? params[ hostOrder[ index ] ] : 0.0f;
}

void ConwayPlugin::SetById( unsigned int id, float value )
{
	const int host = HostIndexOf( id );
	if( host >= 0 )
		SetFloatParameter( static_cast< unsigned int >( host ), value );
}

} // namespace conway
