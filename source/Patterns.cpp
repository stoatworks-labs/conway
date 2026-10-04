#include "Patterns.h"

#include <algorithm>

namespace conway::patterns
{
const char* const kRPentomino = "b2o$2o$bo!";
const char* const kAcorn      = "bo$3bo$2o2b3o!";
const char* const kDiehard    = "6bo$2o$bo3b3o!";
const char* const kGosperGun  = "24bo$22bobo$12b2o6b2o12b2o$11bo3bo4b2o12b2o$2o8bo5bo3b2o$2o8bo3bob2o4bobo$10bo5bo7bo$"
                                "11bo3bo$12b2o!";
const char* const kGlider     = "bo$2bo$3o!";

Shape Parse( const char* rle )
{
	Shape shape;
	int x = 0, y = 0, run = 0;
	for( const char* p = rle; p && *p && *p != '!'; ++p )
	{
		const char c = *p;
		if( c >= '0' && c <= '9' )
		{
			run = run * 10 + ( c - '0' );
			continue;
		}
		const int count = run > 0 ? run : 1;
		run             = 0;
		if( c == 'b' || c == '.' )
			x += count;
		else if( c == 'o' || c == 'A' )
		{
			for( int i = 0; i < count; ++i )
				shape.cells.push_back( { x++, y } );
		}
		else if( c == '$' )
		{
			y += count;
			x = 0;
		}
		shape.width = std::max( shape.width, x );
	}
	shape.height = y + 1;
	for( const Cell& cell : shape.cells )
	{
		shape.width  = std::max( shape.width, cell.x + 1 );
		shape.height = std::max( shape.height, cell.y + 1 );
	}
	return shape;
}

} // namespace conway::patterns
