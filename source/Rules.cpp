#include "Rules.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace conway::rules
{
int IndexOf( const char* name )
{
	for( int i = 0; i < kRuleCount; ++i )
		if( std::strcmp( kRules[ i ].name, name ) == 0 )
			return i;
	return -1;
}

Masks Parse( const std::string& notation )
{
	Masks masks;
	//Split on '/', each part a letter and its digits.
	std::vector< std::string > parts;
	std::string part;
	for( char c : notation )
	{
		if( c == '/' )
		{
			parts.push_back( part );
			part.clear();
		}
		else
			part += static_cast< char >( std::toupper( static_cast< unsigned char >( c ) ) );
	}
	parts.push_back( part );
	if( parts.size() < 2 || parts.size() > 3 || parts[ 0 ].empty() || parts[ 0 ][ 0 ] != 'B' || parts[ 1 ].empty()
	    || parts[ 1 ][ 0 ] != 'S' )
		return masks;

	auto counts = []( const std::string& digits, uint32_t& out ) {
		for( char c : digits )
		{
			if( c < '0' || c > '8' )
				return false;
			const uint32_t bit = 1u << ( c - '0' );
			if( out & bit )
				return false;//a count written twice is a typo, not a rule
			out |= bit;
		}
		return true;
	};
	if( !counts( parts[ 0 ].substr( 1 ), masks.birth ) || !counts( parts[ 1 ].substr( 1 ), masks.survive ) )
		return masks;
	if( parts.size() == 3 )
	{
		if( parts[ 2 ].size() < 2 || parts[ 2 ][ 0 ] != 'C' )
			return masks;
		unsigned long states = 0;
		for( size_t i = 1; i < parts[ 2 ].size(); ++i )
		{
			if( !std::isdigit( static_cast< unsigned char >( parts[ 2 ][ i ] ) ) || states > 65535 )
				return masks;
			states = states * 10 + static_cast< unsigned long >( parts[ 2 ][ i ] - '0' );
		}
		if( states < 2 || states > 65535 )
			return masks;
		masks.states = static_cast< uint32_t >( states );
	}
	masks.valid = true;
	return masks;
}

const Masks& MasksOf( int index )
{
	static const std::vector< Masks > table = [] {
		std::vector< Masks > out;
		for( const Rule& rule : kRules )
			out.push_back( Parse( rule.notation ) );
		return out;
	}();
	return table[ static_cast< size_t >( std::clamp( index, 0, kRuleCount - 1 ) ) ];
}

} // namespace conway::rules
