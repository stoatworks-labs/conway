/**
    cwtest -- render Conway offline, and measure what the automaton is doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context. The automaton
    checks read the state the shipped shaders computed (one R32UI texel per
    cell, read back), and the picture checks read the output.

        cwtest --out /tmp/life.png       the source, the defaults
        cwtest --over --out /tmp/o.png   the Over effect on the harness's card
        cwtest --list                    every parameter and its default
        cwtest --pipe [--frames N]       the source's frames, raw RGBA on stdout
        cwtest --over --pipe             raw frames in, raw frames out
        cwtest --film N                  N frames, raw RGBA on stdout
        cwtest --offline                 the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value` lines,
    held before the first key and after the last. A STANDARD (0..1) control is
    linear between its keys; an option, a boolean, an event or an integer
    STEPS: it holds each key's value until the next key's frame. So a button
    press is three keys (0, 1, 0) and a dropdown never passes through the
    options between two keys.

    CWTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.

    The claims, one flag each -- see README "Building and testing". The
    literature numbers are the LifeWiki's; `--literature` reproduces every one
    on an unbounded plane with the harness's own sparse stepper before
    `--patterns` asks the plugin for them.
*/

#include "Clock.h"
#include "Controls.h"
#include "Conway.h"
#include "Hash.h"
#include "Patterns.h"
#include "Rules.h"
#include "Settle.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace conway;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;
using Cells  = std::vector< uint32_t >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, which is the only place anything here flips. Premultiplied
/// output is written as it is: a viewer shows the source over black.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				//Alpha written as 1: a premultiplied source over black is what
				//Resolume's Add shows, and a viewer that honours alpha would
				//otherwise show the dead cells as the page behind.
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context. CWTEST_RENDERER=software asks for Apple's software renderer
// by id (plotter's recipe, via radar): what a GPU-less runner falls back to.
//---------------------------------------------------------------------------
bool g_software = false;

/// The 640 x 640 methuselah runs take minutes on the software renderer and
/// seconds on a GPU. They repeat the step and count passes that --reference
/// and --count hold bit for bit there, in integer arithmetic GLSL 4.10 makes
/// exact, so the software pass skips them -- loudly -- unless asked.
bool heavyRuns()
{
	return !g_software || std::getenv( "CWTEST_HEAVY" ) != nullptr;
}

void Skip( const std::string& message )
{
	std::printf( "  skip  %s\n", message.c_str() );
}

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "CWTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "cwtest: CWTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a press or a count: cues
/// STEP between keys for these, and only a standard control ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( parts.back().c_str(), nullptr );
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

/// The value at `frame`: held before the first key and after the last; between
/// two keys linear if `ramp`, otherwise the earlier key's value until the
/// later key's frame.
float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Pictures.
//---------------------------------------------------------------------------
double hash01( uint32_t a, uint32_t b = 0 )
{
	return Pcg( a * 2654435761u ^ Pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// The Over effect's card: a night street -- a dark ground, a lit skyline
/// along the top, bright windows, a ring, a coloured sign (saturated: luma
/// would miss it, the brightest channel does not). Rows bottom first.
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			double r = 0.04 + 0.03 * ( 0.5 + v ), g = r, b = r * 1.3;
			if( v > 0.22 && v < 0.42 )
			{
				const int block = static_cast< int >( std::floor( ( u + 2.0 ) * 14.0 ) );
				const int row   = static_cast< int >( std::floor( ( v - 0.22 ) * 40.0 ) );
				if( hash01( static_cast< uint32_t >( block * 31 + row ), 3 ) > 0.45 )
					r = g = b = 0.6 + 0.4 * hash01( static_cast< uint32_t >( block ), 4 );
			}
			const double rr = std::sqrt( ( u + 0.4 ) * ( u + 0.4 ) + ( v + 0.15 ) * ( v + 0.15 ) );
			if( std::fabs( rr - 0.14 ) < 0.02 )
				r = g = b = 0.95;
			if( std::fabs( u - 0.35 ) < 0.18 && std::fabs( v + 0.2 ) < 0.07 )
			{
				r = 0.05;
				g = 0.2;
				b = 0.95;
			}
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( r );
			o[ 1 ]   = static_cast< float >( g );
			o[ 2 ]   = static_cast< float >( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// Audio, written into the Audio buffer the way the host writes it.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses///< a bass-heavy spectrum with a hit every half second
};

void feedAudio( ConwayPlugin& plugin, double seconds, AudioFeed feed )
{
	const int host = plugin.HostIndexOf( PT_AUDIO );
	if( host < 0 )
		return;
	const double beat  = std::fmod( std::max( seconds, 0.0 ), 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( static_cast< unsigned int >( host ), static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock.
//---------------------------------------------------------------------------
struct Rig
{
	ConwayPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0, readFBO = 0;
	int frame          = 0;
	double fps         = 60.0;
	double clockOffset = 0.0;
	/// The host's clock unit: 1 sends seconds, 1000 milliseconds.
	double hostUnit = 1.0;
	AudioFeed feed  = AudioFeed::Silence;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	explicit Rig( bool effect = false ) : plugin( effect )
	{
	}

	~Rig()
	{
		plugin.DeInitGL();
		release();
		if( readFBO )
			glDeleteFramebuffers( 1, &readFBO );
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO = outputFBO;
		if( plugin.IsEffect() )
		{
			const Floats card = picture ? *picture : buildCard( width, height );
			sourceTexture     = makeTexture( width, height, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
			inputStruct.Handle                              = sourceTexture;
			inputs[ 0 ]                                     = &inputStruct;
			process.numInputTextures                        = 1;
			process.inputTextures                           = inputs;
		}
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr, bool fixClock = true )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/conway for which shader\n" );
			return false;
		}
		if( fixClock )
			plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL (Arena
	/// calls neither InitGL nor FF_RESIZE on a running clip).
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetById( id, value );
	}

	double TimeOf( int f ) const
	{
		return clockOffset + static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = TimeOf( frame );
			plugin.SetTime( seconds * hostUnit );
			feedAudio( plugin, seconds, feed );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// Exactly `n` generations, in one frame, the clock aside.
	bool Steps( int n )
	{
		plugin.RequestStepsForTest( n );
		return Render( 1 );
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// The whole state, cols x rows, row 0 at the bottom.
	Cells State( bool previous = false )
	{
		const Grid grid = plugin.CurrentGrid();
		Cells cells( static_cast< size_t >( grid.cols ) * grid.rows );
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		                        previous ? plugin.PreviousTextureID() : plugin.StateTextureID(), 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, grid.cols, grid.rows, GL_RED_INTEGER, GL_UNSIGNED_INT, cells.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return cells;
	}
};

/// A rig stepped by hand: the automaton runs only when asked, its clock
/// stopped, the look plain, no reseeding behind the check's back.
void manual( Rig& rig )
{
	rig.Set( PT_SPEED, 0.0f );
	rig.Set( PT_AUTO_RESEED, 0.0f );
	rig.Set( PT_NOISE, 0.0f );
	rig.Set( PT_SMOOTH, 0.0f );
	if( rig.plugin.IsEffect() )
		rig.Set( PT_FEED, 0.0f );
}

//---------------------------------------------------------------------------
// Fields: cells by alive bit, pattern placement, comparison.
//---------------------------------------------------------------------------
bool alive( uint32_t s )
{
	return ( s & 0xFFFFu ) != 0u;
}

int population( const Cells& cells )
{
	int n = 0;
	for( uint32_t s : cells )
		n += alive( s );
	return n;
}

/// Only the alive bit, as 0/1.
std::vector< uint8_t > aliveSet( const Cells& cells )
{
	std::vector< uint8_t > out( cells.size() );
	for( size_t i = 0; i < cells.size(); ++i )
		out[ i ] = alive( cells[ i ] ) ? 1 : 0;
	return out;
}

/// Stamp an RLE pattern so its written top-left lands at grid column `x`,
/// grid row `top` (rows count up from the bottom; the pattern's rows go down).
void place( Cells& cells, int cols, int rows, const char* rle, int x, int top )
{
	const patterns::Shape shape = patterns::Parse( rle );
	for( const patterns::Cell& c : shape.cells )
	{
		const int gx = ( ( x + c.x ) % cols + cols ) % cols;
		const int gy = ( ( top - c.y ) % rows + rows ) % rows;
		cells[ static_cast< size_t >( gy ) * cols + gx ] = 1u;
	}
}

/// `b` is `a` translated by ( dx, dy ) on the torus, alive bits only.
bool translated( const std::vector< uint8_t >& a, const std::vector< uint8_t >& b, int cols, int rows, int dx, int dy )
{
	for( int y = 0; y < rows; ++y )
		for( int x = 0; x < cols; ++x )
		{
			const int sx = ( ( x - dx ) % cols + cols ) % cols, sy = ( ( y - dy ) % rows + rows ) % rows;
			if( b[ static_cast< size_t >( y ) * cols + x ] != a[ static_cast< size_t >( sy ) * cols + sx ] )
				return false;
		}
	return true;
}

/// A soup, by the harness's own hash.
Cells soup( int cols, int rows, double density, uint32_t salt )
{
	Cells cells( static_cast< size_t >( cols ) * rows, 0u );
	const uint32_t threshold = ThresholdU32( density );
	for( int y = 0; y < rows; ++y )
		for( int x = 0; x < cols; ++x )
			if( CellHash( x, y, salt ) < threshold )
				cells[ static_cast< size_t >( y ) * cols + x ] = 1u;
	return cells;
}

//---------------------------------------------------------------------------
// The harness's own steppers. Written from the rule's definition, not from
// the GLSL; `--literature` holds them to the published numbers first.
//---------------------------------------------------------------------------

/// A bounded grid with the plugin's full state (age, time since death), on a
/// torus or with dead edges, under any Life-like or Generations rule.
Cells referenceStep( const Cells& in, int cols, int rows, bool torus, const rules::Masks& masks )
{
	Cells out( in.size() );
	const uint32_t refractory = masks.Refractory();
	for( int y = 0; y < rows; ++y )
		for( int x = 0; x < cols; ++x )
		{
			int n = 0;
			for( int dy = -1; dy <= 1; ++dy )
				for( int dx = -1; dx <= 1; ++dx )
				{
					if( dx == 0 && dy == 0 )
						continue;
					int nx = x + dx, ny = y + dy;
					if( torus )
					{
						nx = ( nx + cols ) % cols;
						ny = ( ny + rows ) % rows;
					}
					else if( nx < 0 || ny < 0 || nx >= cols || ny >= rows )
						continue;
					n += alive( in[ static_cast< size_t >( ny ) * cols + nx ] ) ? 1 : 0;
				}
			const uint32_t s     = in[ static_cast< size_t >( y ) * cols + x ];
			const uint32_t age   = s & 0xFFFFu;
			const uint32_t since = s >> 16;
			const bool live      = age != 0;
			bool next;
			if( live )
				next = ( masks.survive >> n ) & 1u;
			else
				next = ( ( masks.birth >> n ) & 1u ) && ( since == 0 || since > refractory );
			uint32_t result;
			if( live && next )
				result = std::min( age + 1u, 0xFFFFu );
			else if( next )
				result = 1u;
			else if( live )
				result = 1u << 16;
			else
				result = since == 0 ? 0u : std::min( since + 1u, 0xFFFFu ) << 16;
			out[ static_cast< size_t >( y ) * cols + x ] = result;
		}
	return out;
}

/// An unbounded plane, live cells only, Life-like rules. For the literature.
struct Plane
{
	std::unordered_set< uint64_t > cells;

	static uint64_t key( int x, int y )
	{
		return ( static_cast< uint64_t >( static_cast< uint32_t >( x ) ) << 32 ) | static_cast< uint32_t >( y );
	}
	static int xOf( uint64_t k )
	{
		return static_cast< int >( static_cast< uint32_t >( k >> 32 ) );
	}
	static int yOf( uint64_t k )
	{
		return static_cast< int >( static_cast< uint32_t >( k ) );
	}

	explicit Plane( const char* rle )
	{
		for( const patterns::Cell& c : patterns::Parse( rle ).cells )
			cells.insert( key( c.x, c.y ) );
	}

	void Step( const rules::Masks& masks )
	{
		std::unordered_map< uint64_t, int > counts;
		counts.reserve( cells.size() * 9 );
		for( uint64_t k : cells )
			for( int dy = -1; dy <= 1; ++dy )
				for( int dx = -1; dx <= 1; ++dx )
					if( dx || dy )
						++counts[ key( xOf( k ) + dx, yOf( k ) + dy ) ];
		std::unordered_set< uint64_t > next;
		next.reserve( cells.size() * 2 );
		for( const auto& entry : counts )
		{
			const bool live = cells.count( entry.first ) != 0;
			if( live ? ( ( masks.survive >> entry.second ) & 1u ) : ( ( masks.birth >> entry.second ) & 1u ) )
				next.insert( entry.first );
		}
		//A live cell with no live neighbour never appears in `counts`.
		if( masks.survive & 1u )
			for( uint64_t k : cells )
				if( !counts.count( k ) )
					next.insert( k );
		cells.swap( next );
	}

	size_t Population() const
	{
		return cells.size();
	}

	/// The cells, translated so the bounding box starts at 0, sorted.
	std::vector< uint64_t > Normalised() const
	{
		int x0 = INT32_MAX, y0 = INT32_MAX;
		for( uint64_t k : cells )
		{
			x0 = std::min( x0, xOf( k ) );
			y0 = std::min( y0, yOf( k ) );
		}
		std::vector< uint64_t > out;
		for( uint64_t k : cells )
			out.push_back( key( xOf( k ) - x0, yOf( k ) - y0 ) );
		std::sort( out.begin(), out.end() );
		return out;
	}
	void BoxOrigin( int& x0, int& y0 ) const
	{
		x0 = y0 = INT32_MAX;
		for( uint64_t k : cells )
		{
			x0 = std::min( x0, xOf( k ) );
			y0 = std::min( y0, yOf( k ) );
		}
	}
};

//---------------------------------------------------------------------------
// The test patterns: the LifeWiki's RLE for each, and what the literature
// says it does. Rows as written, top first.
//---------------------------------------------------------------------------
struct StillLife
{
	const char* name;
	const char* rle;
};
const StillLife kStillLifes[] = {
	{ "block", "2o$2o!" }, { "beehive", "b2o$o2bo$b2o!" }, { "loaf", "b2o$o2bo$bobo$2bo!" },
	{ "boat", "2o$obo$bo!" }, { "tub", "bo$obo$bo!" },
};

struct Oscillator
{
	const char* name;
	const char* rle;
	int period;
};
const Oscillator kOscillators[] = {
	{ "blinker", "3o!", 2 },
	{ "toad", "b3o$3o!", 2 },
	{ "beacon", "2o$2o$2b2o$2b2o!", 2 },
	{ "pulsar", "2b3o3b3o2b2$o4bobo4bo$o4bobo4bo$o4bobo4bo$2b3o3b3o2b2$2b3o3b3o2b$o4bobo4bo$o4bobo4bo$o4bobo4bo2$2b3o3b3o!", 3 },
	{ "pentadecathlon", "2bo4bo$2ob4ob2o$2bo4bo!", 15 },
};

struct Ship
{
	const char* name;
	const char* rle;
	int dx, dy;///< per period of 4, as written (y DOWN)
};
const Ship kShips[] = {
	{ "glider", patterns::kGlider, 1, 1 },
	{ "LWSS", "bo2bo$o$o3bo$4o!", -2, 0 },
};

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( ConwayPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

bool applySetting( ConwayPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

/// The cue sheet bound to a plugin's parameters, or an error naming the cue.
bool bindScript( ConwayPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock.
//===========================================================================
/// `readStdin`: the Over effect's frames come in on stdin, one out per one in,
/// until a partial frame or EOF. Otherwise frames are made -- `count` of them,
/// or, with `count` 0, until the reader hangs up (so that mode only ever ends
/// with exit 1; use a count for a take that can end cleanly).
int runPipe( bool effect, int width, int height, double fps, const std::string& scriptPath, int count, bool readStdin,
             bool beat, const std::vector< std::string >& settings )
{
	Rig rig( effect );
	rig.fps = fps;
	if( !rig.Init( width, height ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	//A misspelt cue that silently did nothing would film a take that looks
	//deliberate and is wrong: refuse any name that is not a parameter.
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || count <= 0 || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			if( effect )
				rig.Upload( picture );
		}

		//Through the plugin's own setter, so a cue moves what a slider would.
		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
			{
				//The source is premultiplied: written over black, alpha opaque.
				const float v = ( !effect && x % 4 == 3 ) ? 1.0f : out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ];
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
			}
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone (`| head -c 1`, ffmpeg dying). SIGPIPE is
			//ignored in main(), so this is EPIPE and not a silent 141: say so
			//and stop, rather than render on into a closed pipe.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	std::printf( "\n=== bench: after 120 frames of warm-up, each frame timed with glFinish both sides\n" );
	struct Case
	{
		bool effect;
		int rows;
		const char* what;
	};
	const Case cases[] = { { false, 135, "defaults (135 rows)" }, { true, 135, "defaults (135 rows)" },
		                   { false, 1080, "Cell Size 0 (1080 rows)" } };
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig( c.effect );
			if( !rig.Init( size.w, size.h ) )
				return 1;
			rig.Set( PT_CELL_SIZE, ParamFromRows( c.rows ) );
			rig.Set( PT_AUTO_RESEED, 0.0f );
			if( !rig.Render( 120 ) )
				return 1;
			glFinish();
			constexpr int kTimed = 120;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				if( !rig.Render( 1 ) )
					return 1;
				glFinish();
				times.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			double mean = 0.0;
			for( double t : times )
				mean += t / kTimed;
			std::sort( times.begin(), times.end() );
			std::printf( "  %-15s %-24s %-6s median %5.2f ms/frame, mean %5.2f, worst %5.2f  (median %4.1f%% of a 60 fps frame)\n",
			             c.effect ? "SW Conway Over" : "SW Conway", c.what, size.name, times[ kTimed / 2 ], mean, times.back(),
			             100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the literature or the stated law; `--negative` sets one
// field at a time to a deliberately wrong MODEL (a test hook in the plugin,
// so the shipped shader computes the wrong thing) and requires the check to
// FAIL.
//===========================================================================
struct Perturb
{
	bool literatureHighLife = false;///< --literature: the plane steps HighLife's masks for Conway's
	bool countSelf          = false;///< --patterns: a cell counts itself as a neighbour
	bool ruleFlip           = false;///< --rules: one bit of each rule's mask flipped
	bool wrapSkew           = false;///< --reference: the torus wraps one cell short
	bool countDying         = false;///< --count: the query also takes the just-dead
	bool settleConstant     = false;///< --settle: only a constant population settles
	bool lawConstant        = false;///< --settle-law: the detector with period 1 only
	bool clockFloat         = false;///< --clock: elapsed time from a float host clock
	bool lawPerFrame        = false;///< --clock-law: a float count per frame, rounded each time
	bool pointSample        = false;///< --coverage: the cell under the pixel's centre
	bool overMirrored       = false;///< --over-check: the clip laid under the grid mirrored
	bool primeOff           = false;///< --prime: the analyser unprimed
	bool resizeClears       = false;///< --resize: the state cleared on a resize
	bool cuesRamp           = false;///< --cues: every control ramps between keys
};

using CheckFn = int ( * )( const Perturb& );

/// The rasters every check runs at: the one developed at, and CI's.
struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

/// A rig at `raster` with a grid of exactly cols x rows, stepped by hand,
/// loaded with `cells` (generation 0 on the next frame).
bool loadRig( Rig& rig, const Raster& raster, int cols, int rows, const Cells& cells, int rule = 0, bool torus = true )
{
	if( !rig.Init( raster.w, raster.h ) )
		return false;
	manual( rig );
	rig.Set( PT_RULE, static_cast< float >( rule ) );
	rig.Set( PT_EDGES, static_cast< float >( torus ? Edges::Torus : Edges::Dead ) );
	rig.plugin.SetGridForTest( cols, rows );
	rig.plugin.LoadStateForTest( cells );
	return rig.Render( 1 );
}

//===========================================================================
// --literature (no GL): the harness's plane reproduces the published numbers
// with the shipped rule table's Conway, so the reference and the table are
// both right before anything asks the plugin.
//===========================================================================
int runLiterature( const Perturb& perturb )
{
	std::printf( "\n=== literature: an unbounded plane, stepped by the harness under the shipped table's Conway (B3/S23)\n" );
	const rules::Masks conwayMasks = rules::MasksOf( rules::IndexOf( perturb.literatureHighLife ? "HighLife" : "Conway" ) );
	{
		Plane r( patterns::kRPentomino );
		size_t at1102 = 0;
		int wrong     = 0;
		for( int g = 1; g <= 1200; ++g )
		{
			r.Step( conwayMasks );
			if( g == 1102 )
				at1102 = r.Population();
			if( g >= 1103 && r.Population() != 116 )
				++wrong;
		}
		Check( at1102 == 118 && wrong == 0,
		       fmt( "R-pentomino: %zu cells at generation 1102 (118), 116 at every generation 1103..1200 (%d not)", at1102, wrong ) );
	}
	{
		Plane d( patterns::kDiehard );
		size_t at129 = 0, at130 = 0;
		for( int g = 1; g <= 130; ++g )
		{
			d.Step( conwayMasks );
			if( g == 129 )
				at129 = d.Population();
		}
		at130 = d.Population();
		Check( at129 > 0 && at130 == 0, fmt( "Diehard: %zu cells at generation 129, %zu at 130 (gone at 130)", at129, at130 ) );
	}
	{
		Plane a( patterns::kAcorn );
		size_t at5205 = 0;
		int wrong     = 0;
		for( int g = 1; g <= 5210; ++g )
		{
			a.Step( conwayMasks );
			if( g == 5205 )
				at5205 = a.Population();
			if( g >= 5206 && a.Population() != 633 )
				++wrong;
		}
		Check( at5205 != 633 && wrong == 0,
		       fmt( "acorn: %zu cells at 5205, 633 at every generation 5206..5210 (%d not)", at5205, wrong ) );
	}
	{
		Plane gun( patterns::kGosperGun );
		size_t last = gun.Population();
		int wrong   = 0;
		std::string seen;
		for( int k = 1; k <= 10; ++k )
		{
			for( int g = 0; g < 30; ++g )
				gun.Step( conwayMasks );
			wrong += gun.Population() != last + 5;
			seen += fmt( " %zu", gun.Population() );
			last = gun.Population();
		}
		Check( wrong == 0, fmt( "Gosper gun: +5 cells (a glider) every 30 generations, 10 periods:%s", seen.c_str() ) );
	}
	{
		int bad = 0;
		std::string what;
		for( const StillLife& s : kStillLifes )
		{
			Plane p( s.rle );
			const auto before = p.Normalised();
			p.Step( conwayMasks );
			const bool still = p.Normalised() == before && p.Population() == before.size();
			bad += !still;
			what += fmt( " %s%s", s.name, still ? "" : "(MOVED)" );
		}
		Check( bad == 0, fmt( "still lifes unchanged by a generation:%s", what.c_str() ) );
	}
	{
		int bad = 0;
		std::string what;
		for( const Oscillator& o : kOscillators )
		{
			Plane p( o.rle );
			const auto start = p.Normalised();
			int x0, y0;
			p.BoxOrigin( x0, y0 );
			int period = 0;
			for( int g = 1; g <= 60 && period == 0; ++g )
			{
				p.Step( conwayMasks );
				int x1, y1;
				p.BoxOrigin( x1, y1 );
				if( p.Normalised() == start && x1 == x0 && y1 == y0 )
					period = g;
			}
			bad += period != o.period;
			what += fmt( " %s p%d (%d)", o.name, period, o.period );
		}
		Check( bad == 0, fmt( "oscillators back at their period and not before:%s", what.c_str() ) );
	}
	{
		int bad = 0;
		std::string what;
		for( const Ship& s : kShips )
		{
			Plane p( s.rle );
			const auto start = p.Normalised();
			int x0, y0;
			p.BoxOrigin( x0, y0 );
			for( int g = 0; g < 4; ++g )
				p.Step( conwayMasks );
			int x1, y1;
			p.BoxOrigin( x1, y1 );
			const bool ok = p.Normalised() == start && x1 - x0 == s.dx && y1 - y0 == s.dy;
			bad += !ok;
			what += fmt( " %s moved (%d,%d) in 4 (%d,%d)", s.name, x1 - x0, y1 - y0, s.dx, s.dy );
		}
		Check( bad == 0, fmt( "spaceships: the same shape, translated, every 4 generations:%s", what.c_str() ) );
	}
	{
		//The parser: the table parses, and what is not a rule is refused.
		int bad = 0;
		for( int i = 0; i < rules::kRuleCount; ++i )
			bad += !rules::MasksOf( i ).valid;
		const char* refused[] = { "B9/S", "B33/S23", "S23/B3", "B3/S23/C1", "B3", "B3/S2x", "" };
		for( const char* r : refused )
			bad += rules::Parse( r ).valid;
		const rules::Masks brain = rules::Parse( "B2/S/C3" );
		bad += !( brain.valid && brain.birth == 4u && brain.survive == 0u && brain.Refractory() == 1u );
		Check( bad == 0, fmt( "the rule table: all %d rules parse, 7 malformed notations are refused, B2/S/C3 is "
		                      "birth {2}, survive {}, one refractory generation (%d wrong)",
		                      rules::kRuleCount, bad ) );
	}
	return Verdict();
}

//===========================================================================
// --patterns: the literature, asked of the plugin.
//===========================================================================
int runPatterns( const Perturb& perturb )
{
	std::printf( "\n=== patterns: still lifes, oscillators, spaceships, the gun, Diehard and the R-pentomino, through the plugin\n" );
	for( const Raster& raster : kRasters )
	{
		//Still lifes and oscillators, each on its own 48 x 40 torus.
		{
			int bad = 0;
			std::string what;
			for( const StillLife& s : kStillLifes )
			{
				Cells cells( 48 * 40, 0u );
				place( cells, 48, 40, s.rle, 20, 22 );
				Rig rig;
				rig.plugin.SetCountSelfForTest( perturb.countSelf );
				if( !loadRig( rig, raster, 48, 40, cells ) )
					return 1;
				const auto start = aliveSet( rig.State() );
				bool still       = true;
				for( int g = 1; g <= 10; ++g )
				{
					rig.Steps( 1 );
					still = still && aliveSet( rig.State() ) == start;
				}
				bad += !still;
				what += fmt( " %s%s", s.name, still ? "" : "(MOVED)" );
			}
			for( const Oscillator& o : kOscillators )
			{
				Cells cells( 48 * 40, 0u );
				place( cells, 48, 40, o.rle, 17, 26 );
				Rig rig;
				rig.plugin.SetCountSelfForTest( perturb.countSelf );
				if( !loadRig( rig, raster, 48, 40, cells ) )
					return 1;
				const auto start = aliveSet( rig.State() );
				int period       = 0;
				for( int g = 1; g <= 30 && period == 0; ++g )
				{
					rig.Steps( 1 );
					if( aliveSet( rig.State() ) == start )
						period = g;
				}
				bad += period != o.period;
				what += fmt( " %s p%d", o.name, period );
			}
			Check( bad == 0, fmt( "%dx%d  still for 10 generations, and back at the period and not before:%s", raster.w,
			                      raster.h, what.c_str() ) );
		}
		//Spaceships: translated by their published speed; the glider once
		//round a 24 x 24 torus in 96 generations.
		{
			int bad = 0;
			std::string what;
			for( const Ship& s : kShips )
			{
				Cells cells( 40 * 30, 0u );
				place( cells, 40, 30, s.rle, 18, 16 );
				Rig rig;
				rig.plugin.SetCountSelfForTest( perturb.countSelf );
				if( !loadRig( rig, raster, 40, 30, cells ) )
					return 1;
				const auto start = aliveSet( rig.State() );
				rig.Steps( 4 );
				//Written y down; the grid's rows count up.
				const bool ok = translated( start, aliveSet( rig.State() ), 40, 30, s.dx, -s.dy );
				bad += !ok;
				what += fmt( " %s %s", s.name, ok ? "translated" : "NOT translated" );
			}
			{
				Cells cells( 24 * 24, 0u );
				place( cells, 24, 24, patterns::kGlider, 5, 18 );
				Rig rig;
				rig.plugin.SetCountSelfForTest( perturb.countSelf );
				if( !loadRig( rig, raster, 24, 24, cells ) )
					return 1;
				const auto start = aliveSet( rig.State() );
				rig.Steps( 95 );
				const bool early = aliveSet( rig.State() ) == start;
				rig.Steps( 1 );
				const bool home = aliveSet( rig.State() ) == start;
				bad += !home || early;
				what += fmt( "; the glider home on a 24x24 torus at 96 (%s), not at 95 (%s)", home ? "yes" : "NO", early ? "WAS" : "no" );
			}
			Check( bad == 0, fmt( "%dx%d  spaceships, 4 generations:%s", raster.w, raster.h, what.c_str() ) );
		}
		//The gun on a 160 x 160 torus: the stream has not wrapped by 300.
		{
			Cells cells( 160 * 160, 0u );
			place( cells, 160, 160, patterns::kGosperGun, 10, 150 );
			Rig rig;
			rig.plugin.SetCountSelfForTest( perturb.countSelf );
			if( !loadRig( rig, raster, 160, 160, cells ) )
				return 1;
			int last  = population( rig.State() );
			int wrong = 0;
			std::string seen;
			for( int k = 1; k <= 10; ++k )
			{
				rig.Steps( 30 );
				const int p = population( rig.State() );
				wrong += p != last + 5;
				seen += fmt( " %d", p );
				last = p;
			}
			Check( wrong == 0, fmt( "%dx%d  Gosper gun: +5 every 30 generations:%s", raster.w, raster.h, seen.c_str() ) );
		}
		//Diehard on a 96 x 96 torus, dead edges too.
		for( bool torus : { true, false } )
		{
			Cells cells( 96 * 96, 0u );
			place( cells, 96, 96, patterns::kDiehard, 44, 50 );
			Rig rig;
			rig.plugin.SetCountSelfForTest( perturb.countSelf );
			if( !loadRig( rig, raster, 96, 96, cells, 0, torus ) )
				return 1;
			rig.Steps( 129 );
			const int at129 = population( rig.State() );
			rig.Steps( 1 );
			const int at130 = population( rig.State() );
			Check( at129 > 0 && at130 == 0, fmt( "%dx%d  Diehard (%s): %d cells at 129, %d at 130", raster.w, raster.h,
			                                     torus ? "torus" : "dead edges", at129, at130 ) );
		}
		//The R-pentomino on a 640 x 640 torus: its whole spread to generation
		//1200 (549 x 573 cells on the plane) fits with a cell to spare across
		//every seam, so the torus IS the plane until then.
		if( !heavyRuns() )
			Skip( fmt( "%dx%d  R-pentomino: 1200 generations of 640x640 on the software renderer (CWTEST_HEAVY=1 runs it)",
			           raster.w, raster.h ) );
		else
		{
			Cells cells( 640 * 640, 0u );
			place( cells, 640, 640, patterns::kRPentomino, 300, 329 );
			Rig rig;
			rig.plugin.SetCountSelfForTest( perturb.countSelf );
			if( !loadRig( rig, raster, 640, 640, cells ) )
				return 1;
			rig.Steps( 1102 );
			const int at1102 = population( rig.State() );
			int wrong = 0, first = -1;
			for( int g = 1103; g <= 1200; ++g )
			{
				rig.Steps( 1 );
				const int p = population( rig.State() );
				if( p != 116 )
				{
					++wrong;
					if( first < 0 )
						first = p;
				}
			}
			Check( at1102 == 118 && wrong == 0,
			       fmt( "%dx%d  R-pentomino: %d cells at 1102 (118); 116 at every generation 1103..1200 (%d not%s)", raster.w,
			            raster.h, at1102, wrong, first >= 0 ? fmt( ", first %d", first ).c_str() : "" ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --rules: what each family member must do, measured, not its mask.
//===========================================================================
int runRules( const Perturb& perturb )
{
	std::printf( "\n=== rules: Day & Night's and Anneal's symmetry, Life without Death, Seeds, the Replicator, Brian's Brain\n" );
	const int cols = 61, rows = 47;
	auto flipped = [ & ]( Rig& rig, uint32_t bits ) {
		if( perturb.ruleFlip )
			rig.plugin.SetBirthFlipForTest( bits );
	};
	for( const Raster& raster : kRasters )
	{
		//Day & Night and Anneal: the complement of a field steps to the
		//complement of its step, because each is its own complement-dual
		//(B = { 8 - s : s not in S }).
		for( const char* name : { "Day & Night", "Anneal" } )
		{
			const int dn     = rules::IndexOf( name );
			const Cells a    = soup( cols, rows, 0.5, 77 );
			Cells complement = a;
			for( uint32_t& s : complement )
				s = s ? 0u : 1u;
			int mismatches = 0;
			Rig ra, rb;
			flipped( ra, 1u << 6 );
			flipped( rb, 1u << 6 );
			if( !loadRig( ra, raster, cols, rows, a, dn ) || !loadRig( rb, raster, cols, rows, complement, dn ) )
				return 1;
			for( int g = 1; g <= 20; ++g )
			{
				ra.Steps( 1 );
				rb.Steps( 1 );
				const auto x = aliveSet( ra.State() ), y = aliveSet( rb.State() );
				for( size_t i = 0; i < x.size(); ++i )
					mismatches += x[ i ] == y[ i ];
			}
			Check( mismatches == 0, fmt( "%dx%d  %s: step( not x ) = not step( x ) for 20 generations of a 50%% soup "
			                             "(%d cells disagree)", raster.w, raster.h, name, mismatches ) );
		}
		//Life without Death: nothing ever dies.
		{
			Rig rig;
			if( perturb.ruleFlip )
				rig.plugin.SetBirthFlipForTest( 0 );
			if( !loadRig( rig, raster, cols, rows, soup( cols, rows, 0.08, 5 ), rules::IndexOf( "Life without Death" ) ) )
				return 1;
			//No hook flips a survive bit, so the perturbed model is Conway.
			if( perturb.ruleFlip )
				rig.Set( PT_RULE, 0.0f );
			int deaths = 0;
			auto before = aliveSet( rig.State() );
			for( int g = 1; g <= 30; ++g )
			{
				rig.Steps( 1 );
				const auto after = aliveSet( rig.State() );
				for( size_t i = 0; i < after.size(); ++i )
					deaths += before[ i ] && !after[ i ];
				before = after;
			}
			Check( deaths == 0, fmt( "%dx%d  Life without Death: %d deaths in 30 generations", raster.w, raster.h, deaths ) );
		}
		//Seeds: no live cell is alive a generation later.
		{
			Rig rig;
			if( !loadRig( rig, raster, cols, rows, soup( cols, rows, 0.1, 9 ), rules::IndexOf( "Seeds" ) ) )
				return 1;
			if( perturb.ruleFlip )
				rig.Set( PT_RULE, 0.0f );
			int survivors = 0, births = 0;
			auto before   = aliveSet( rig.State() );
			for( int g = 1; g <= 30; ++g )
			{
				rig.Steps( 1 );
				const auto after = aliveSet( rig.State() );
				for( size_t i = 0; i < after.size(); ++i )
				{
					survivors += before[ i ] && after[ i ];
					births += !before[ i ] && after[ i ];
				}
				before = after;
			}
			Check( survivors == 0 && births > 0, fmt( "%dx%d  Seeds: %d survivals, %d births in 30 generations", raster.w,
			                                         raster.h, survivors, births ) );
		}
		//The Replicator: B1357/S1357 is the parity of the eight neighbours,
		//linear over GF(2), so 2^k generations put 8 copies of any pattern at
		//2^k in the eight directions. A 7 x 7 pattern, 8 generations.
		{
			const int n = 64;
			Cells cells( static_cast< size_t >( n ) * n, 0u );
			std::vector< std::pair< int, int > > pattern;
			for( int y = 0; y < 7; ++y )
				for( int x = 0; x < 7; ++x )
					if( hash01( static_cast< uint32_t >( y * 7 + x ), 41 ) < 0.45 )
					{
						pattern.emplace_back( x, y );
						cells[ static_cast< size_t >( 29 + y ) * n + 29 + x ] = 1u;
					}
			Rig rig;
			flipped( rig, 1u << 1 );
			if( !loadRig( rig, raster, n, n, cells, rules::IndexOf( "Replicator" ) ) )
				return 1;
			rig.Steps( 8 );
			std::vector< uint8_t > expect( cells.size(), 0 );
			for( int dy = -1; dy <= 1; ++dy )
				for( int dx = -1; dx <= 1; ++dx )
					if( dx || dy )
						for( const auto& p : pattern )
							expect[ static_cast< size_t >( 29 + p.second + 8 * dy ) * n + 29 + p.first + 8 * dx ] ^= 1;
			const auto got = aliveSet( rig.State() );
			int wrong      = 0;
			for( size_t i = 0; i < got.size(); ++i )
				wrong += got[ i ] != expect[ i ];
			Check( wrong == 0, fmt( "%dx%d  Replicator: a %zu-cell 7x7 pattern is 8 copies at +-8 after 8 generations (%d "
			                        "cells wrong)", raster.w, raster.h, pattern.size(), wrong ) );
		}
		//Brian's Brain (B2/S/C3): every live cell dies, and a cell that has
		//just died is not born again on the next generation.
		{
			Rig rig;
			if( !loadRig( rig, raster, cols, rows, soup( cols, rows, 0.3, 13 ), rules::IndexOf( "Brian's Brain" ) ) )
				return 1;
			//The perturbed model: B2/S with no refractory generation, which is
			//Seeds.
			if( perturb.ruleFlip )
				rig.Set( PT_RULE, static_cast< float >( rules::IndexOf( "Seeds" ) ) );
			int survivors = 0, rebirths = 0, births = 0;
			auto twoBack  = aliveSet( rig.State() );
			rig.Steps( 1 );
			auto oneBack = aliveSet( rig.State() );
			for( int g = 2; g <= 30; ++g )
			{
				rig.Steps( 1 );
				const auto now = aliveSet( rig.State() );
				for( size_t i = 0; i < now.size(); ++i )
				{
					survivors += oneBack[ i ] && now[ i ];
					rebirths += twoBack[ i ] && !oneBack[ i ] && now[ i ];
					births += !oneBack[ i ] && now[ i ];
				}
				twoBack = oneBack;
				oneBack = now;
			}
			Check( survivors == 0 && rebirths == 0 && births > 0,
			       fmt( "%dx%d  Brian's Brain: %d survivals, %d births straight after a death, %d births in 30 generations",
			            raster.w, raster.h, survivors, rebirths, births ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --reference: every rule, both edges, against the harness's own stepper,
// state for state -- ages and times since death included.
//===========================================================================
int runReference( const Perturb& perturb )
{
	std::printf( "\n=== reference: every rule, torus and dead edges, a 157x93 grid, 64 generations, bit for bit\n" );
	const int cols = 157, rows = 93;
	for( const Raster& raster : kRasters )
	{
		int bad = 0, worstRule = -1;
		std::string what;
		for( int rule = 0; rule < rules::kRuleCount; ++rule )
			for( bool torus : { true, false } )
			{
				Rig rig;
				rig.plugin.SetWrapSkewForTest( perturb.wrapSkew ? 1 : 0 );
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				manual( rig );
				rig.Set( PT_RULE, static_cast< float >( rule ) );
				rig.Set( PT_EDGES, static_cast< float >( torus ? Edges::Torus : Edges::Dead ) );
				rig.Set( PT_DENSITY, 0.35f );
				rig.Set( PT_SEED, static_cast< float >( 100 + rule ) );
				rig.plugin.SetGridForTest( cols, rows );
				if( !rig.Render( 1 ) )//seeded by the plugin's own soup
					return 1;
				Cells expect   = rig.State();
				int mismatched = 0;
				for( int g = 1; g <= 64; ++g )
				{
					expect = referenceStep( expect, cols, rows, torus, rules::MasksOf( rule ) );
					rig.Steps( 1 );
					const Cells got = rig.State();
					for( size_t i = 0; i < got.size(); ++i )
						mismatched += got[ i ] != expect[ i ];
				}
				if( mismatched )
				{
					++bad;
					worstRule = rule;
					what += fmt( " %s/%s(%d)", rules::kRules[ rule ].name, torus ? "torus" : "dead", mismatched );
				}
			}
		Check( bad == 0, fmt( "%dx%d  %d rules x 2 edges: %d disagree with the reference%s", raster.w, raster.h,
		                      rules::kRuleCount, bad, worstRule >= 0 ? what.c_str() : "" ) );
	}
	return Verdict();
}

//===========================================================================
// --count: the population the occlusion query reports is the population.
//===========================================================================
int runCount( const Perturb& perturb )
{
	std::printf( "\n=== count: the occlusion query's population equals the state's, every generation\n" );
	for( const Raster& raster : kRasters )
	{
		const int cols = 200, rows = 113;
		Rig rig;
		rig.plugin.SetCountDyingForTest( perturb.countDying );
		rig.plugin.SetBlockingCountsForTest( true );
		rig.plugin.SetPopulationLogForTest( true );
		if( !loadRig( rig, raster, cols, rows, soup( cols, rows, 0.35, 3 ) ) )
			return 1;
		std::map< uint64_t, int > truth;
		truth[ 0 ] = population( rig.State() );
		for( int g = 1; g <= 300; ++g )
		{
			rig.Steps( 1 );
			truth[ static_cast< uint64_t >( g ) ] = population( rig.State() );
		}
		int wrong = 0, seen = 0;
		for( const auto& entry : rig.plugin.PopulationLog() )
		{
			const auto t = truth.find( entry.first );
			if( t == truth.end() )
				continue;
			++seen;
			wrong += static_cast< int >( entry.second ) != t->second;
		}
		Check( seen == 301 && wrong == 0, fmt( "%dx%d  generations 0..300: %d counted, %d wrong (first %d, last %d cells)",
		                                       raster.w, raster.h, seen, wrong, truth[ 0 ], truth[ 300 ] ) );
	}
	return Verdict();
}

//===========================================================================
// --settle-law (no GL): the detector, on populations made up to the purpose.
//===========================================================================
int runSettleLaw( const Perturb& perturb )
{
	std::printf( "\n=== settle-law: settled after %d generations of an exact period <= %d, or %d of nothing\n",
	             SettleDetector::kWindow, SettleDetector::kMaxPeriod, SettleDetector::kExtinct );
	auto firstSettle = [ & ]( const std::function< uint32_t( int ) >& sequence, int limit, int* period ) {
		SettleDetector d;
		if( perturb.lawConstant )
			d.SetMaxPeriodForTest( 1 );
		for( int g = 0; g < limit; ++g )
			if( d.Push( static_cast< uint64_t >( g ), sequence( g ) ) )
			{
				if( period )
					*period = d.Period();
				return g;
			}
		return -1;
	};
	int p1 = 0, p7 = 0, p30 = 0;
	const int constant = firstSettle( []( int ) { return 500u; }, 1000, &p1 );
	const int seven    = firstSettle( []( int g ) { return 400u + static_cast< uint32_t >( ( g * 37 ) % 7 ); }, 1000, &p7 );
	const int thirty   = firstSettle( []( int g ) { return 100u + static_cast< uint32_t >( g % 2 ) * 3u + static_cast< uint32_t >( g % 3 ) * 5u + static_cast< uint32_t >( g % 15 == 0 ) * 11u; }, 1000, &p30 );
	const int boiling  = firstSettle( []( int g ) { return 1000u + ( Pcg( static_cast< uint32_t >( g ) ) % 50u ); }, 20000, nullptr );
	const int empty    = firstSettle( []( int ) { return 0u; }, 1000, nullptr );
	//A soup that dies after 300 generations of activity.
	const int dying = firstSettle( []( int g ) { return g < 300 ? 900u + ( Pcg( static_cast< uint32_t >( g ) ) % 40u ) : 0u; }, 1000, nullptr );
	const int w = SettleDetector::kWindow;
	Check( constant == w && p1 == 1, fmt( "a constant population settles at generation %d (%d), period %d", constant, w, p1 ) );
	Check( seven == w + 6 && p7 == 7, fmt( "a period-7 population settles at %d (%d), period %d", seven, w + 6, p7 ) );
	Check( thirty == w + 29 && p30 == 30, fmt( "a period-30 population (2, 3 and 15 together) settles at %d (%d), period %d",
	                                           thirty, w + 29, p30 ) );
	Check( boiling < 0, fmt( "a population that never repeats for %d generations never settles in 20000 (%d)", w, boiling ) );
	Check( empty == SettleDetector::kExtinct - 1 && dying == 300 + SettleDetector::kExtinct - 1,
	       fmt( "nothing settles at generation %d (%d); a field that dies at 300 at %d (%d)", empty,
	            SettleDetector::kExtinct - 1, dying, 300 + SettleDetector::kExtinct - 1 ) );
	return Verdict();
}

//===========================================================================
// --settle: Auto Reseed, through the plugin's own counts.
//===========================================================================
int runSettle( const Perturb& perturb )
{
	std::printf( "\n=== settle: ash reseeds when the law says; a methuselah boiling, never\n" );
	for( const Raster& raster : kRasters )
	{
		//Ash: two still lifes and five oscillators of periods 2, 3 and 15 on a
		//128 x 96 torus. The harness's own stepper finds the field's
		//population period; the plugin must settle at kWindow + period - 1.
		{
			const int cols = 128, rows = 96;
			Cells cells( static_cast< size_t >( cols ) * rows, 0u );
			place( cells, cols, rows, kStillLifes[ 0 ].rle, 8, 88 );
			place( cells, cols, rows, kStillLifes[ 1 ].rle, 24, 88 );
			int x = 8;
			for( const Oscillator& o : kOscillators )
			{
				place( cells, cols, rows, o.rle, x, 60 );
				x += 24;
			}
			std::vector< int > pops;
			Cells field = cells;
			for( int g = 0; g < 400; ++g )
			{
				pops.push_back( population( field ) );
				field = referenceStep( field, cols, rows, true, rules::MasksOf( 0 ) );
			}
			int period = 0;
			for( int p = 1; p <= 60 && period == 0; ++p )
			{
				bool repeats = true;
				for( size_t g = static_cast< size_t >( p ); g < pops.size() && repeats; ++g )
					repeats = pops[ g ] == pops[ g - static_cast< size_t >( p ) ];
				if( repeats )
					period = p;
			}
			Rig rig;
			rig.plugin.SetBlockingCountsForTest( true );
			if( perturb.settleConstant )
				rig.plugin.SetSettleMaxPeriodForTest( 1 );
			if( !loadRig( rig, raster, cols, rows, cells ) )
				return 1;
			rig.Set( PT_AUTO_RESEED, 1.0f );
			rig.Steps( 400 );
			const int expect = SettleDetector::kWindow + period - 1;
			const bool fired = rig.plugin.Reseeds() == 1;
			Check( fired && static_cast< int >( rig.plugin.LastSettledGeneration() ) == expect
			           && rig.plugin.LastSettledPeriod() == period,
			       fmt( "%dx%d  ash (population period %d by the harness's stepper): settled at generation %llu (%d), "
			            "period %d, %llu reseed",
			            raster.w, raster.h, period, static_cast< unsigned long long >( rig.plugin.LastSettledGeneration() ),
			            expect, rig.plugin.LastSettledPeriod(), rig.plugin.Reseeds() ) );
			rig.Render( 1 );
			Check( population( rig.State() ) > 2000, fmt( "%dx%d  and the reseed is the Pattern (Soup): %d cells", raster.w,
			                                              raster.h, population( rig.State() ) ) );
		}
		//An empty field, and an R-pentomino in its first 1000 generations.
		{
			Rig rig;
			rig.plugin.SetBlockingCountsForTest( true );
			if( !loadRig( rig, raster, 64, 64, Cells( 64 * 64, 0u ) ) )
				return 1;
			rig.Set( PT_AUTO_RESEED, 1.0f );
			rig.Steps( 50 );
			Check( rig.plugin.Reseeds() == 1 && static_cast< int >( rig.plugin.LastSettledGeneration() ) == SettleDetector::kExtinct - 1,
			       fmt( "%dx%d  an empty field reseeds at generation %llu (%d)", raster.w, raster.h,
			            static_cast< unsigned long long >( rig.plugin.LastSettledGeneration() ), SettleDetector::kExtinct - 1 ) );
		}
		if( !heavyRuns() )
			Skip( fmt( "%dx%d  the R-pentomino's 1000 generations of 640x640 on the software renderer (CWTEST_HEAVY=1 runs it)",
			           raster.w, raster.h ) );
		else
		{
			Cells cells( 640 * 640, 0u );
			place( cells, 640, 640, patterns::kRPentomino, 300, 329 );
			Rig rig;
			rig.plugin.SetBlockingCountsForTest( true );
			if( !loadRig( rig, raster, 640, 640, cells ) )
				return 1;
			rig.Set( PT_AUTO_RESEED, 1.0f );
			rig.Steps( 1000 );
			Check( rig.plugin.Reseeds() == 0 && rig.plugin.Generation() == 1000,
			       fmt( "%dx%d  the R-pentomino boils for 1000 generations without settling (%llu reseeds)", raster.w,
			            raster.h, rig.plugin.Reseeds() ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --clock-law (no GL): the generation clock at any frame rate.
//===========================================================================
int runClockLaw( const Perturb& perturb )
{
	std::printf( "\n=== clock-law: generations = floor( Speed x elapsed ) at any frame rate, in double\n" );
	struct Case
	{
		double speed, fps;
		bool jitter;
	};
	const Case cases[] = { { 13.37, 60.0, false }, { 13.37, 23.976, false }, { 61.3, 59.94, true }, { 0.71, 30.0, true } };
	for( const Case& c : cases )
	{
		GenerationClock clock;
		long long total = 0;
		float floatOwed = 0.0f;
		double elapsed  = 0.0;
		int wrong = 0, near = 0;
		for( int f = 1; f <= 36000; ++f )
		{
			double dt = 1.0 / c.fps;
			if( c.jitter )
				dt *= 0.5 + hash01( static_cast< uint32_t >( f ), 17 );
			elapsed += dt;
			if( perturb.lawPerFrame )
			{
				//The wrong model: the owed fraction carried in float.
				floatOwed += static_cast< float >( c.speed * dt );
				const float whole = std::floor( floatOwed );
				floatOwed -= whole;
				total += static_cast< long long >( whole );
			}
			else
				total += clock.Advance( c.speed, dt, 1 << 20 );
			const double ideal = c.speed * elapsed;
			if( std::fabs( ideal - std::round( ideal ) ) < 1e-6 )
				++near;//too close to a whole generation to call; not scored
			else if( total != static_cast< long long >( std::floor( ideal ) ) )
				++wrong;
		}
		Check( wrong == 0, fmt( "Speed %g at %g fps%s, 36000 frames (%.0f s): %d frames off floor( Speed x elapsed ), %d "
		                        "within 1e-6 of a whole generation and not scored",
		                        c.speed, c.fps, c.jitter ? " jittered" : "", elapsed, wrong, near ) );
	}
	return Verdict();
}

//===========================================================================
// --clock: at Resolume's ~499 million ms, through the plugin.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: a host clock at 499,000,000 ms (a float resolves 32 ms there) runs generations exactly\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h, nullptr, false ) )
			return 1;
		rig.Set( PT_AUTO_RESEED, 0.0f );
		rig.Set( PT_SPEED, 0.0f );
		rig.plugin.SetGridForTest( 64, 36 );
		rig.plugin.SetFloatClockForTest( perturb.clockFloat );
		rig.hostUnit    = 1000.0;//milliseconds
		rig.clockOffset = 499000.0;
		//The unit vote needs the wall clock to agree: the first frames are
		//paced in real time, as a host would pace them.
		int paced = 0;
		while( rig.plugin.ClockScale() == 0.0 && paced < 60 )
		{
			rig.Render( 1 );
			std::this_thread::sleep_for( std::chrono::microseconds( 16667 ) );
			++paced;
		}
		rig.Render( 2 );
		const bool voted = rig.plugin.ClockScale() == 0.001;
		const double speed = 13.37;
		rig.Set( PT_SPEED, ParamFromSpeed( speed ) );
		const double effective = SpeedFromParam( ParamFromSpeed( speed ) );
		const int m            = rig.frame;//the first frame that owes generations
		const uint64_t base    = rig.plugin.TotalGenerations();
		int wrong = 0, near = 0;
		double worstDt = 0.0;
		std::string seen;
		for( int stop = 0; stop < 10; ++stop )
		{
			for( int f = 0; f < 60; ++f )
			{
				rig.Render( 1 );
				worstDt = std::max( worstDt, std::fabs( rig.plugin.LastDt() - 1.0 / 60.0 ) );
			}
			const int n        = rig.frame - 1;
			const double ideal = effective * ( rig.TimeOf( n ) - rig.TimeOf( m - 1 ) );
			const long long got = static_cast< long long >( rig.plugin.TotalGenerations() - base );
			if( std::fabs( ideal - std::round( ideal ) ) < 1e-6 )
				++near;
			else if( got != static_cast< long long >( std::floor( ideal ) ) )
				++wrong;
			if( stop % 3 == 0 )
				seen += fmt( " %lld", got );
		}
		//Double at 4.99e8 ms: an ulp is 6e-8 ms, so a frame's dt is right to
		//1e-10 s. Elapsed time telescopes, so a float clock can land the
		//count right at a checkpoint; its dt (0 or 32 ms) cannot hide.
		Check( voted && wrong == 0 && near < 10 && worstDt <= 1e-9,
		       fmt( "%dx%d  the vote settled on ms (%s, after %d paced frames); every dt within %.1e s of 1/60 (bound "
		            "1e-9); Speed %.4f for 600 frames: the count is floor( Speed x elapsed ) at %d of %d checkpoints (%s ...)",
		            raster.w, raster.h, voted ? "yes" : "NO", paced, worstDt, effective, 10 - near - wrong, 10 - near, seen.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --coverage: the picture is the grid box-filtered.
//===========================================================================
int runCoverage( const Perturb& perturb )
{
	std::printf( "\n=== coverage: each pixel is the live area it covers (Mono, no trail, no crossfade)\n" );
	for( const Raster& raster : kRasters )
	{
		struct Case
		{
			int rows;
			float gap;
		};
		std::vector< Case > cases = { { raster.h / 8, 0.2f }, { 97, 0.0f }, { 97, 0.3f }, { raster.h * 2, 0.0f } };
		for( const Case& c : cases )
		{
			const Grid grid = GridFor( c.rows, raster.w, raster.h );
			Rig rig;
			rig.plugin.SetPointSampleForTest( perturb.pointSample );
			if( !loadRig( rig, raster, grid.cols, grid.rows, soup( grid.cols, grid.rows, 0.4, 21 ) ) )
				return 1;
			rig.Set( PT_PALETTE, static_cast< float >( Palette::Mono ) );
			rig.Set( PT_TRAIL, 0.0f );
			rig.Set( PT_GAP, c.gap );
			rig.Render( 1 );
			const Cells state = rig.State();
			const Floats out  = rig.Output();
			const double sx = static_cast< double >( grid.cols ) / raster.w, sy = static_cast< double >( grid.rows ) / raster.h;
			const double inset = 0.5 * GapFromParam( c.gap );
			//Every edge the shader computes is a float at up to max( cols,
			//rows ): an ulp there, four edges, over the pixel's width in cells.
			const double maxCoord = std::max( grid.cols, grid.rows );
			const double ulp      = std::ldexp( 1.0, static_cast< int >( std::floor( std::log2( maxCoord ) ) ) - 23 );
			const double bound    = 4.0 * ulp / std::min( sx, sy ) + 4.0 * std::ldexp( 1.0, -23 );
			double worst          = 0.0;
			long long exact = 0, total = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const double lx = x * sx, hx = ( x + 1 ) * sx, ly = y * sy, hy = ( y + 1 ) * sy;
					double lit = 0.0;
					for( int cy = static_cast< int >( std::floor( ly ) ); cy < std::min( grid.rows, static_cast< int >( std::ceil( hy ) ) ); ++cy )
						for( int cx = static_cast< int >( std::floor( lx ) ); cx < std::min( grid.cols, static_cast< int >( std::ceil( hx ) ) ); ++cx )
						{
							if( !alive( state[ static_cast< size_t >( cy ) * grid.cols + cx ] ) )
								continue;
							const double wx = std::max( 0.0, std::min( hx, cx + 1 - inset ) - std::max( lx, cx + inset ) );
							const double wy = std::max( 0.0, std::min( hy, cy + 1 - inset ) - std::max( ly, cy + inset ) );
							lit += wx * wy;
						}
					const double expect = lit / ( sx * sy );
					for( int ch = 0; ch < 4; ++ch )
					{
						const double got = out[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + ch ];
						worst            = std::max( worst, std::fabs( got - expect ) );
						exact += got == static_cast< float >( expect );
						++total;
					}
				}
			Check( worst <= bound, fmt( "%dx%d  %dx%d cells (%.3f px each), Gap %.2f: worst %.2e (bound %.1e from float "
			                            "edges), %.1f%% of values exact",
			                            raster.w, raster.h, grid.cols, grid.rows, raster.h / static_cast< double >( grid.rows ),
			                            GapFromParam( c.gap ), worst, bound, 100.0 * static_cast< double >( exact ) / static_cast< double >( total ) ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --over-check: the clip seeds the cells under its bright parts, the right
// way round; Mix 0 is the clip; Feed 1 keeps them alive.
//===========================================================================
int runOver( const Perturb& perturb )
{
	std::printf( "\n=== over: the clip's bright cells seed the field, unmirrored; Mix 0 is the clip; Feed 1 holds them\n" );
	for( const Raster& raster : kRasters )
	{
		//Black, with three bright rectangles placed asymmetrically, and one
		//saturated blue (luma 0.07; the brightest channel 0.95).
		const int w = raster.w, h = raster.h;
		struct Box
		{
			double x0, y0, x1, y1;
			float r, g, b;
		};
		const Box boxes[] = { { 0.05, 0.10, 0.30, 0.45, 1, 1, 1 }, { 0.62, 0.55, 0.70, 0.95, 0.9, 0.8, 0.7 },
			                  { 0.40, 0.05, 0.52, 0.20, 1, 1, 1 }, { 0.80, 0.10, 0.95, 0.30, 0.02, 0.05, 0.95 } };
		Floats card( static_cast< size_t >( w ) * h * 4, 0.0f );
		for( int y = 0; y < h; ++y )
			for( int x = 0; x < w; ++x )
			{
				float* o = &card[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
				o[ 3 ]   = 1.0f;
				for( const Box& b : boxes )
					if( x >= b.x0 * w && x < b.x1 * w && y >= b.y0 * h && y < b.y1 * h )
					{
						o[ 0 ] = b.r;
						o[ 1 ] = b.g;
						o[ 2 ] = b.b;
					}
			}
		const int rows  = 90;
		const Grid grid = GridFor( rows, w, h );
		auto brightAt   = [ & ]( int cx, int cy ) {
            const int px    = ( ( 2 * cx + 1 ) * w ) / ( 2 * grid.cols );
            const int py    = ( ( 2 * cy + 1 ) * h ) / ( 2 * grid.rows );
            const float* t  = &card[ ( static_cast< size_t >( py ) * w + px ) * 4 ];
            return std::max( std::max( t[ 0 ], t[ 1 ] ), t[ 2 ] ) * t[ 3 ] > 0.5f;
		};
		{
			Rig rig( true );
			rig.plugin.SetMirrorForTest( perturb.overMirrored );
			if( !rig.Init( w, h, &card ) )
				return 1;
			manual( rig );
			rig.plugin.SetGridForTest( grid.cols, grid.rows );
			rig.Render( 1 );//the first frame seeds from the clip
			const Cells state = rig.State();
			int wrong = 0, bright = 0;
			for( int cy = 0; cy < grid.rows; ++cy )
				for( int cx = 0; cx < grid.cols; ++cx )
				{
					const bool b = brightAt( cx, cy );
					bright += b;
					wrong += b != alive( state[ static_cast< size_t >( cy ) * grid.cols + cx ] );
				}
			Check( wrong == 0 && bright > 100,
			       fmt( "%dx%d  Reseed from the clip: %d cells under a bright part, %d cells disagree (the blue box counts)",
			            w, h, bright, wrong ) );

			rig.Set( PT_MIX, 0.0f );
			rig.Render( 1 );
			const Floats out = rig.Output();
			int differ       = 0;
			for( size_t i = 0; i < out.size(); ++i )
				differ += out[ i ] != card[ i ];
			Check( differ == 0, fmt( "%dx%d  Mix 0 returns the clip bit for bit (%d floats differ)", w, h, differ ) );
		}
		{
			Rig rig( true );
			rig.plugin.SetMirrorForTest( perturb.overMirrored );
			if( !rig.Init( w, h, &card ) )
				return 1;
			manual( rig );
			rig.Set( PT_FEED, 1.0f );
			rig.plugin.SetGridForTest( grid.cols, grid.rows );
			rig.Render( 1 );
			int missing = 0, grown = 0;
			for( int g = 1; g <= 10; ++g )
			{
				rig.Steps( 1 );
				const Cells state = rig.State();
				for( int cy = 0; cy < grid.rows; ++cy )
					for( int cx = 0; cx < grid.cols; ++cx )
					{
						const bool live = alive( state[ static_cast< size_t >( cy ) * grid.cols + cx ] );
						if( brightAt( cx, cy ) )
							missing += !live;
						else if( g == 10 )
							grown += live;
					}
			}
			Check( missing == 0 && grown > 0, fmt( "%dx%d  Feed 1: every bright cell alive after each of 10 generations (%d "
			                                       "missing); %d cells grown outside by generation 10",
			                                       w, h, missing, grown ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --prime: no audio event on the first frame after a clip trigger.
//===========================================================================
int runPrime( const Perturb& perturb )
{
	std::printf( "\n=== prime: loud audio already playing when the clip is triggered fires nothing on the trigger frame\n" );
	for( bool effect : { false, true } )
		for( const Raster& raster : kRasters )
		{
			Rig rig( effect );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			manual( rig );
			rig.plugin.SetGridForTest( 64, 36 );
			rig.plugin.SetUnprimedForTest( perturb.primeOff );
			rig.Set( PT_AUDIO_STEPS, 2.0f );
			rig.feed = AudioFeed::Pulses;
			rig.Render( 100 );//1.67 s of beats
			const unsigned long long before = rig.plugin.Onsets();
			const uint64_t genBefore        = rig.plugin.TotalGenerations();
			//The trigger: the host's clock goes back to 0.02 s -- just after a
			//hit, the music still loud, which is the case that deafened the
			//fleet's unprimed analysers. The next hit is at 0.5 s, 29 frames on.
			rig.clockOffset = 0.02 - static_cast< double >( rig.frame ) / rig.fps;
			rig.Render( 1 );
			const unsigned long long atTrigger = rig.plugin.Onsets();
			rig.Render( 35 );
			const unsigned long long after = rig.plugin.Onsets();
			Check( before >= 3 && genBefore == 2 * before && atTrigger == before && after == atTrigger + 1,
			       fmt( "%s %dx%d: %llu onsets from 4 beats before (%llu generations: 2 each); %llu on the trigger frame; "
			            "%llu in the next 35 frames (one hit, at 0.5 s)",
			            effect ? "Over  " : "source", raster.w, raster.h, before, static_cast< unsigned long long >( genBefore ),
			            atTrigger - before, after - atTrigger ) );
		}
	return Verdict();
}

//===========================================================================
// --resize: the field survives the host's raster changing.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: a resize of the same shape keeps the field to the bit; a new shape keeps the overlap\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 1280, 720 ) )
			return 1;
		rig.plugin.SetClearOnResizeForTest( perturb.resizeClears );
		rig.Set( PT_AUTO_RESEED, 0.0f );
		if( effect )
			rig.Set( PT_PATTERN, static_cast< float >( Pattern::Soup ) );
		if( !rig.Render( 40 ) )
			return 1;
		rig.Set( PT_SPEED, 0.0f );
		if( effect )
			rig.Set( PT_FEED, 0.0f );
		rig.Render( 1 );
		const Grid g0     = rig.plugin.CurrentGrid();
		const Cells start = rig.State();
		//Same shape, a quarter of the size.
		rig.Resize( 320, 180 );
		rig.Render( 1 );
		const Grid g1 = rig.plugin.CurrentGrid();
		const bool same = g1 == g0 && rig.State() == start;
		//4:3: fewer columns, the middle kept.
		rig.Resize( 960, 720 );
		rig.Render( 1 );
		const Grid g2       = rig.plugin.CurrentGrid();
		const Cells narrow  = rig.State();
		const int offset    = ( g0.cols - g2.cols ) / 2;
		int lostInOverlap = 0;
		for( int y = 0; y < g2.rows; ++y )
			for( int x = 0; x < g2.cols; ++x )
				lostInOverlap += narrow[ static_cast< size_t >( y ) * g2.cols + x ] != start[ static_cast< size_t >( y ) * g0.cols + x + offset ];
		//And back: the middle as it was, the margins empty.
		rig.Resize( 1280, 720 );
		rig.Render( 1 );
		const Cells wide = rig.State();
		int wrongBack    = 0;
		for( int y = 0; y < g0.rows; ++y )
			for( int x = 0; x < g0.cols; ++x )
			{
				const bool inside = x >= offset && x < offset + g2.cols;
				const uint32_t expect = inside ? start[ static_cast< size_t >( y ) * g0.cols + x ] : 0u;
				wrongBack += wide[ static_cast< size_t >( y ) * g0.cols + x ] != expect;
			}
		Check( same && lostInOverlap == 0 && wrongBack == 0 && population( start ) > 1000,
		       fmt( "%s: %dx%d grid, %d live: 1280x720 -> 320x180 %s; -> 960x720 (%dx%d) %d cells of the overlap changed; "
		            "-> 1280x720 %d cells not the middle as it was and empty margins",
		            effect ? "Over  " : "source", g0.cols, g0.rows, population( start ), same ? "bit-identical" : "CHANGED",
		            g2.cols, g2.rows, lostInOverlap, wrongBack ) );
	}
	return Verdict();
}

//===========================================================================
// --cues (no GL): options, booleans, events and integers step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options, booleans, events and integers, and ramps standard controls\n" );
	std::istringstream sheet( "0 Rule 0\n60 Rule 2\n0 Auto Reseed 0\n30 Auto Reseed 1\n"
	                          "0 Audio Steps 2\n60 Audio Steps 8\n0 Gap 0\n60 Gap 1\n0 Reseed 0\n30 Reseed 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	ConwayPlugin plugin( false );
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const int bFrame      = found->second.back().first;
		const float atB       = valueAt( found->second, bFrame, ramp );
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b && atB == b ) : ( mid == a && valueAt( found->second, bFrame - 1, ramp ) == a && atB == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	for( bool effect : { false, true } )
	{
		ConwayPlugin plugin( effect );
		std::map< std::string, int > seen, address;
		int longNames = 0, dupes = 0, clashes = 0;
		for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
		{
			const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
			if( name.size() > 16 )
			{
				std::printf( "    too long: %s\n", name.c_str() );
				++longNames;
			}
			if( seen[ name ]++ > 0 )
				++dupes;
			//Arena's OSC/REST address: lower case, spaces removed.
			std::string key;
			for( char c : name )
				if( c != ' ' )
					key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			if( address[ key ]++ > 0 )
				++clashes;
		}
		//The About block is last, so a "User guide" button added later moves
		//no control's index.
		const unsigned int aboutFirst = plugin.ParamCount() - stoatworks::about::kParamCount;
		const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
		Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
		       fmt( "%s: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
		            effect ? "SW Conway Over" : "SW Conway", plugin.ParamCount(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 320, 180 ) )
			return 1;
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 3; ++frame )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 7, 5, 300, 170 );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 320, 180 );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 10; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		Check( problems == 0, fmt( "%s, three frames: viewport, vertex array, array buffer, program, active unit, framebuffer, "
		                           "blend, scissor, clear colour, colour mask, ten texture units (%d wrong:%s)",
		                           effect ? "Over" : "source", problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "literature", runLiterature, true }, { "patterns", runPatterns, false }, { "rules", runRules, false },
		{ "reference", runReference, false },  { "count", runCount, false },       { "settle-law", runSettleLaw, true },
		{ "settle", runSettle, false },        { "clock-law", runClockLaw, true }, { "clock", runClock, false },
		{ "coverage", runCoverage, false },    { "over", runOver, false },         { "prime", runPrime, false },
		{ "resize", runResize, false },        { "cues", runCues, true },          { "names", runNames, true },
		{ "state", runState, false },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	add( "literature", runLiterature, "step the plane with HighLife's masks (B36/S23)", []( Perturb& p ) { p.literatureHighLife = true; } );
	add( "patterns", runPatterns, "count the cell itself as a neighbour", []( Perturb& p ) { p.countSelf = true; } );
	add( "rules", runRules, "a bit of each rule flipped (Day & Night and Anneal B6, Replicator B1; the rest: the wrong rule)", []( Perturb& p ) { p.ruleFlip = true; } );
	add( "reference", runReference, "the torus wraps one cell short", []( Perturb& p ) { p.wrapSkew = true; } );
	add( "count", runCount, "the query also counts the just-dead", []( Perturb& p ) { p.countDying = true; } );
	add( "settle-law", runSettleLaw, "the detector looks for a constant population only", []( Perturb& p ) { p.lawConstant = true; } );
	add( "settle", runSettle, "the plugin's detector looks for a constant population only", []( Perturb& p ) { p.settleConstant = true; } );
	add( "clock-law", runClockLaw, "the owed fraction carried in float", []( Perturb& p ) { p.lawPerFrame = true; } );
	add( "clock", runClock, "take elapsed time from the host clock in float", []( Perturb& p ) { p.clockFloat = true; } );
	add( "coverage", runCoverage, "point-sample the cell under each pixel's centre", []( Perturb& p ) { p.pointSample = true; } );
	add( "over", runOver, "lay the clip under the grid mirrored", []( Perturb& p ) { p.overMirrored = true; } );
	add( "prime", runPrime, "run the analyser unprimed", []( Perturb& p ) { p.primeOff = true; } );
	add( "resize", runResize, "clear the state on a resize", []( Perturb& p ) { p.resizeClears = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/conway.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 300;
	double fps = 60.0;
	bool beat = false, effect = false;
	std::string mode, scriptPath, clipPath;
	int filmFrames = -1;
	bool sizeGiven = false, framesGiven = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "cwtest -- render Conway offline and measure its automaton\n\n"
			             "  --out PATH        render and write a PNG (default /tmp/conway.png)\n"
			             "  --over            the Over effect, on the harness's night-street card\n"
			             "  --clip FILE       (with --over --out) a raw RGBA frame of --size to use as the clip\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 300: 5 s, 75 generations)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --beat            feed a beat every half second into the Audio buffer\n"
			             "  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA out: the source makes --frames N (0/absent: until the\n"
			             "                    reader hangs up); the Over effect takes frames in on stdin\n"
			             "  --film N          N frames, raw RGBA on stdout (the Over on its card)\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n"
			             "  --expect          a draft of the fleet Arena gate's expectation\n\n"
			             "  checks: --patterns --rules --reference --count --settle --clock --coverage --over-check --prime\n"
			             "          --resize --state   no GL: --literature --settle-law --clock-law --cues --names\n"
			             "          --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
		{
			frames      = std::atoi( argv[ ++i ] );
			framesGiven = true;
		}
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--over" )
			effect = true;
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument == "--over-check" )
			mode = "over";
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "expect" )
	{
		//A DRAFT of the fleet gate's expectation, from what the plugins
		//really declare: defaults are the floats the constructors set, not
		//rounded literals (containment's trap).
		std::printf( "{\n  \"plugin\": \"conway\",\n  \"dlls\": [\"Conway.dll\", \"Conway Over.dll\"],\n"
		             "  \"register\": [\n    {\"name\": \"SW Conway\", \"uid\": \"LF01\", \"kind\": \"source\"},\n"
		             "    {\"name\": \"SW Conway Over\", \"uid\": \"LF02\", \"kind\": \"effect\"}\n  ],\n  \"params\": {\n" );
		for( bool over : { false, true } )
		{
			ConwayPlugin plugin( over );
			std::printf( "    \"%s\": [\n", over ? "SW Conway Over" : "SW Conway" );
			const std::vector< NamedParameter > list = listParameters( plugin );
			for( size_t i = 0; i < list.size(); ++i )
			{
				const NamedParameter& p = list[ i ];
				std::string line        = "      {\"name\": \"" + p.name + "\", ";
				if( p.type == FF_TYPE_OPTION )
					line += "\"type\": \"ParamChoice\", \"default\": \"" + std::string( plugin.GetParamElementName( p.index, static_cast< unsigned int >( std::lround( p.value ) ) ) ) + "\"";
				else if( p.type == FF_TYPE_BUFFER )
					line += "\"type\": \"ParamChoice\", \"default\": \"Composition\", \"requires\": \"audio\"";
				else if( p.type == FF_TYPE_BOOLEAN )
					line += std::string( "\"type\": \"ParamBoolean\", \"default\": " ) + ( p.value > 0.5f ? "true" : "false" );
				else if( p.type == FF_TYPE_EVENT )
					line += "\"type\": \"ParamEvent\"";
				else if( p.type == FF_TYPE_TEXT )
					line += "\"type\": \"ParamString\", \"default_pattern\": \"^Conway\\\\ v{version}\\\\ \\\\-\\\\ MIT\\\\ \\\\-\\\\ Stoatworks\\\\ Labs,\\\\ stoatworks\\\\-labs\\\\.com$\"";
				else
				{
					const float lo = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).min : 0.0f;
					const float hi = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).max : 1.0f;
					line += fmt( "\"type\": \"ParamRange\", \"min\": %.1f, \"max\": %.1f, \"default\": %.17g", lo, hi, static_cast< double >( p.value ) );
				}
				if( p.name.rfind( "Audio ", 0 ) == 0 )
					line += ", \"requires\": \"audio\"";
				if( p.name == "Speed" || p.name == "Smooth" || p.name == "Step" || p.name == "Auto Reseed" || p.name == "Noise"
				    || p.name == "Feed" || p.name == "Edges" || p.name == "Rule" )
					line += ", \"note\": \"acts over generations: single grabs of a living field may read it inconclusive\"";
				line += i + 1 < list.size() ? "},\n" : "}\n";
				std::printf( "%s", line.c_str() );
			}
			std::printf( over ? "    ]\n" : "    ],\n" );
		}
		std::printf( "  }\n}\n" );
		return 0;
	}
	if( mode == "list" )
	{
		ConwayPlugin plugin( effect );
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		//No context at all: what a runner with no accelerated GL can run. The
		//skip is loud, so a green run is not read as one that checked pixels.
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The automaton and picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n", failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		//The fleet's two shapes: an effect is frames in, frames out (toner's);
		//a source makes --frames of them, or runs until the reader hangs up
		//(pattern's).
		result = runPipe( effect, width, height, fps, scriptPath, framesGiven ? frames : 0, effect, beat, settings );
	else if( mode == "film" )
		result = runPipe( effect, width, height, fps, scriptPath, filmFrames, false, beat, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig( effect );
		rig.fps = fps;
		Floats card;
		if( effect && !clipPath.empty() )
		{
			std::ifstream file( clipPath, std::ios::binary );
			Bytes raw( static_cast< size_t >( width ) * height * 4 );
			if( !file.read( reinterpret_cast< char* >( raw.data() ), static_cast< std::streamsize >( raw.size() ) ) )
			{
				std::fprintf( stderr, "--clip %s: not %dx%d RGBA\n", clipPath.c_str(), width, height );
				return 2;
			}
			card.resize( raw.size() );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					card[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = raw[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
		}
		else if( effect )
			card = buildCard( width, height );
		if( !rig.Init( width, height, effect ? &card : nullptr ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;
			if( !rig.Render( std::max( frames, 1 ) ) )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), generation %llu\n", outPath.c_str(), width, height,
				             frames, fps, frames / fps, static_cast< unsigned long long >( rig.plugin.Generation() ) );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
