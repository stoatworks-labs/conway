#include "Shaders.h"

namespace conway::shaders
{
const char* const kVersion = "#version 410 core\n";

//===========================================================================
// The library. No #version, no main.
//===========================================================================
const char* const kCommon = R"(
//= mirrored in the harness (cwtest), Pcg(). Integer only: the same on every GPU.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//A cell's hash under a salt: three rounds, so neighbouring cells, and the same
//cell in consecutive generations, are unrelated. Compared with an integer
//threshold (`hash < p x 2^32`), never converted to a float.
uint cellHash( ivec2 c, uint salt )
{
	return pcg( uint( c.x ) ^ pcg( uint( c.y ) ^ pcg( salt ) ) );
}

const uint AGE_MASK = 0xFFFFu;

bool aliveOf( uint s )
{
	return ( s & AGE_MASK ) != 0u;
}
)";

//===========================================================================
// The clip, for the passes that read it as cells (seed and step). The cell's
// value is the clip's texel holding the cell's centre, found in INTEGER
// arithmetic -- floor( ( c + 1/2 ) x raster / grid ) as ( 2c + 1 ) raster /
// ( 2 grid ) -- so which pixel decides a cell is exact on any GPU. Brightness
// is the brightest channel times alpha, not luma: a saturated blue has luma
// 0.07 and would never be alive (radar's lesson).
//===========================================================================
const char* const kClipCommon = R"(
uniform sampler2D Clip;
uniform ivec2 ClipRaster;
uniform float Threshold;
uniform int MirrorForTest;

ivec2 clipPixelOf( ivec2 c, ivec2 grid )
{
	ivec2 p = ( ( 2 * c + 1 ) * ClipRaster ) / ( 2 * grid );
	if( MirrorForTest == 1 )
		p.x = ClipRaster.x - 1 - p.x;
	return p;
}

bool clipBright( ivec2 c, ivec2 grid )
{
	vec4 t = texelFetch( Clip, clipPixelOf( c, grid ), 0 );
	return max( max( t.r, t.g ), t.b ) * t.a > Threshold;
}
)";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//===========================================================================
// Seed. Every cell starts newborn (age 1) or never-alive (0).
//===========================================================================
const char* const kSeedFragment = R"(
uniform ivec2 Grid;
uniform int Mode;          //0 soup, 1 stamp, 2 glider fleet, 3 the clip
uniform uint DensityU;     //soup: a cell; fleet: a 16 x 16 block
uniform uint Salt;
uniform usampler2D Stamp;  //the pattern, 1 = alive, row 0 its BOTTOM row
uniform ivec2 StampOrigin; //the grid cell under the stamp's bottom-left
uniform ivec2 StampSize;
out uint State;

void main()
{
	ivec2 c    = ivec2( gl_FragCoord.xy );
	bool alive = false;
	if( Mode == 0 )
		alive = cellHash( c, Salt ) < DensityU;
	else if( Mode == 1 )
	{
		ivec2 s = c - StampOrigin;
		if( all( greaterThanEqual( s, ivec2( 0 ) ) ) && all( lessThan( s, StampSize ) ) )
			alive = texelFetch( Stamp, s, 0 ).r != 0u;
	}
	else if( Mode == 2 )
	{
		//A glider at the block's middle, flipped by two bits of its hash into
		//one of the four diagonal headings. Rows bottom-up: ooo / ..o / .o.
		ivec2 b = c / 16;
		ivec2 l = c - b * 16 - ivec2( 6 );
		uint h  = cellHash( b, Salt );
		if( h < DensityU && all( greaterThanEqual( l, ivec2( 0 ) ) ) && all( lessThan( l, ivec2( 3 ) ) ) )
		{
			uint k = pcg( h );
			if( ( k & 1u ) != 0u )
				l.x = 2 - l.x;
			if( ( k & 2u ) != 0u )
				l.y = 2 - l.y;
			alive = l.y == 0 || ( l.y == 1 && l.x == 2 ) || ( l.y == 2 && l.x == 1 );
		}
	}
	else if( Mode == 3 )
		alive = clipBright( c, Grid );
	State = alive ? 1u : 0u;
}
)";

//===========================================================================
// Copy: a new grid takes the old one's overlap, centred, cell for cell.
//===========================================================================
const char* const kCopyFragment = R"(
uniform usampler2D Old;
uniform ivec2 OldGrid;
uniform ivec2 Offset;//new cell c is old cell c + Offset
out uint State;

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy ) + Offset;
	bool inside = c.x >= 0 && c.y >= 0 && c.x < OldGrid.x && c.y < OldGrid.y;
	State = inside ? texelFetch( Old, c, 0 ).r : 0u;
}
)";

//===========================================================================
// One generation.
//
// The rule is B/S over the eight neighbours: a dead cell with n live
// neighbours is born if bit n of Birth is set, a live one survives if bit n
// of Survive is. A Generations rule's dying cells (Refractory generations
// after death) neither count nor can be born. Then births from outside the
// rule: Noise anywhere, Feed where the clip is bright, a patch of soup on an
// audio onset. With RuleOn 0 only those apply (a patch while paused).
//
// The age counts up while a cell lives; on death it becomes the time SINCE,
// which counts up while it stays dead. Both saturate at 65535.
//===========================================================================
const char* const kStepFragment = R"(
uniform usampler2D Current;
uniform ivec2 Grid;
uniform int Torus;
uniform uint Birth;
uniform uint Survive;
uniform uint Refractory;
uniform int RuleOn;
uniform uint NoiseU;
uniform uint NoiseSalt;
uniform int FeedOn;
uniform uint FeedU;
uniform uint FeedSalt;
uniform int PatchRadius;
uniform ivec2 PatchCentre;
uniform uint PatchU;
uniform uint PatchSalt;
uniform int CountSelfForTest;
uniform int WrapSkewForTest;
out uint Next;

//1 if the neighbour at c is alive. c is at most one cell outside the grid,
//so the torus is one add or subtract per axis -- never a % of a negative
//int, which GLSL leaves undefined.
uint neighbour( ivec2 c )
{
	if( Torus == 1 )
	{
		if( c.x < 0 )
			c.x += Grid.x - WrapSkewForTest;
		else if( c.x >= Grid.x )
			c.x -= Grid.x;
		if( c.y < 0 )
			c.y += Grid.y - WrapSkewForTest;
		else if( c.y >= Grid.y )
			c.y -= Grid.y;
	}
	else if( c.x < 0 || c.y < 0 || c.x >= Grid.x || c.y >= Grid.y )
		return 0u;
	return aliveOf( texelFetch( Current, c, 0 ).r ) ? 1u : 0u;
}

void main()
{
	ivec2 c    = ivec2( gl_FragCoord.xy );
	uint s     = texelFetch( Current, c, 0 ).r;
	uint age   = s & AGE_MASK;
	uint since = s >> 16;
	bool alive = age != 0u;
	bool next  = alive;
	if( RuleOn == 1 )
	{
		uint n = 0u;
		for( int dy = -1; dy <= 1; ++dy )
			for( int dx = -1; dx <= 1; ++dx )
				if( dx != 0 || dy != 0 || CountSelfForTest == 1 )
					n += neighbour( c + ivec2( dx, dy ) );
		if( alive )
			next = ( ( Survive >> n ) & 1u ) != 0u;
		else
			next = ( ( Birth >> n ) & 1u ) != 0u && ( since == 0u || since > Refractory );
	}
	bool kept = alive && next;

	bool forced = false;
	if( NoiseU != 0u && cellHash( c, NoiseSalt ) < NoiseU )
		forced = true;
	if( FeedOn == 1 && cellHash( c, FeedSalt ) < FeedU && clipBright( c, Grid ) )
		forced = true;
	if( PatchRadius > 0 )
	{
		ivec2 d = c - PatchCentre;
		if( d.x * d.x + d.y * d.y <= PatchRadius * PatchRadius && cellHash( c, PatchSalt ) < PatchU )
			forced = true;
	}

	if( kept )
		Next = min( age + 1u, AGE_MASK );
	else if( next || forced )
		Next = 1u;
	else if( alive )
		Next = 1u << 16;
	else
		Next = since == 0u ? 0u : ( min( since + 1u, AGE_MASK ) << 16 );
}
)";

//===========================================================================
// Count: a fragment per live cell, the rest discarded, under GL_SAMPLES_PASSED.
// With no multisampling a fragment is one sample, so the query's result is
// the population, exactly (GL 4.1, 4.1.7; the inexact query is the
// _CONSERVATIVE one, 4.3). Nothing is written: the colour mask is off.
//===========================================================================
const char* const kCountFragment = R"(
uniform usampler2D Current;
uniform int CountDyingForTest;
out vec4 Mark;

void main()
{
	uint s       = texelFetch( Current, ivec2( gl_FragCoord.xy ), 0 ).r;
	bool counted = aliveOf( s ) || ( CountDyingForTest == 1 && ( s >> 16 ) == 1u );
	if( !counted )
		discard;
	Mark = vec4( 1.0 );
}
)";

//===========================================================================
// Composite. Each output pixel is the cell grid BOX-FILTERED over its
// footprint: every cell it overlaps weighted by the exact area of overlap
// with the cell's lit square (the cell inset by Gap / 2 each side). So a
// whole-pixel cell size is crisp, a fractional one is evenly antialiased, a
// cell smaller than a pixel averages, and the picture's integral is the lit
// area (`cwtest --coverage`). At most kSpan cells per axis per pixel.
//
// A cell's colour is premultiplied: a live one by its age, from Newborn to
// Elder over Age Span generations (Spectrum: a hue that turns with age); a
// dead one glows in TrailColour at exp( -since / Trail ), its alpha the same.
// The last generation is crossfaded into this one by Phase.
//===========================================================================
const char* const kCompositeFragment = R"(
uniform usampler2D Current;
uniform usampler2D Previous;
uniform sampler2D InputTexture;
uniform ivec2 Grid;
uniform vec2 Raster;
uniform vec2 ViewOrigin;
uniform float Phase;
uniform float Gap;
uniform float AgeSpan;
uniform float Trail;
uniform int Spectrum;
uniform vec3 Newborn;
uniform vec3 Elder;
uniform vec3 TrailColour;
uniform int IsEffect;
uniform float ClipColour;
uniform float Backdrop;
uniform float MixAmount;
uniform int PointSampleForTest;
out vec4 FragColor;

const int kSpan = 12;

//A hue wheel from bounded operations only (no trig): red, green, blue apart
//by a third of a turn, each a clamped triangle.
vec3 hueOf( float h )
{
	return clamp( abs( fract( vec3( h ) + vec3( 0.0, 2.0 / 3.0, 1.0 / 3.0 ) ) * 6.0 - 3.0 ) - 1.0, 0.0, 1.0 );
}

vec4 look( uint s )
{
	uint age = s & AGE_MASK;
	if( age != 0u )
	{
		float t = float( age - 1u ) / AgeSpan;
		vec3 c  = Spectrum == 1 ? hueOf( fract( t ) ) : mix( Newborn, Elder, min( t, 1.0 ) );
		return vec4( c, 1.0 );
	}
	uint since = s >> 16;
	if( since == 0u || Trail <= 0.0 )
		return vec4( 0.0 );
	float level = exp( -float( since ) / Trail );
	return vec4( TrailColour * level, level );
}

vec4 cellAt( ivec2 c )
{
	return mix( look( texelFetch( Previous, c, 0 ).r ), look( texelFetch( Current, c, 0 ).r ), Phase );
}

void main()
{
	vec2 p     = gl_FragCoord.xy - ViewOrigin;//the pixel's centre
	vec2 scale = vec2( Grid ) / Raster;      //cells per pixel
	vec4 life  = vec4( 0.0 );
	if( PointSampleForTest == 1 )
	{
		//The negative control: the cell under the pixel's centre, unfiltered.
		ivec2 c = clamp( ivec2( floor( p * scale ) ), ivec2( 0 ), Grid - 1 );
		life    = cellAt( c );
	}
	else
	{
		vec2 lo  = ( p - 0.5 ) * scale;
		vec2 hi  = ( p + 0.5 ) * scale;
		ivec2 c0 = clamp( ivec2( floor( lo ) ), ivec2( 0 ), Grid - 1 );
		ivec2 c1 = clamp( ivec2( ceil( hi ) ) - 1, c0, min( Grid - 1, c0 + kSpan - 1 ) );
		float inset = 0.5 * Gap;
		vec4 sum    = vec4( 0.0 );
		for( int y = c0.y; y <= c1.y; ++y )
			for( int x = c0.x; x <= c1.x; ++x )
			{
				vec2 a = max( lo, vec2( x, y ) + inset );
				vec2 b = min( hi, vec2( x + 1, y + 1 ) - inset );
				vec2 w = max( b - a, vec2( 0.0 ) );
				float area = w.x * w.y;
				if( area > 0.0 )
					sum += area * cellAt( ivec2( x, y ) );
			}
		life = sum / ( scale.x * scale.y );
	}
	life = min( life, vec4( 1.0 ) );

	if( IsEffect == 0 )
	{
		//Premultiplied: where nothing lives the source is transparent, so it
		//layers over whatever is below it in the composition.
		FragColor = life;
		return;
	}
	//The clip's own texel, fetched, not filtered, so Mix 0 hands it back bit
	//for bit (radar's lesson: a filtered read at a texel centre was not exact
	//on the software renderer).
	vec4 clip = texelFetch( InputTexture, ivec2( p ), 0 );
	life.rgb  = mix( life.rgb, clip.rgb * life.a, ClipColour );
	vec3 over = life.rgb + clip.rgb * Backdrop * ( 1.0 - life.a );
	FragColor = vec4( mix( clip.rgb, over, MixAmount ), mix( clip.a, 1.0, MixAmount ) );
}
)";

std::string Assemble( const char* a, const char* b )
{
	std::string s = kVersion;
	s += kCommon;
	for( const char* piece : { a, b } )
		if( piece )
			s += piece;
	return s;
}

} // namespace conway::shaders
