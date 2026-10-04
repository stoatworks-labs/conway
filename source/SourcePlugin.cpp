#include "Conway.h"

/**
    The source: Conway's Game of Life on its own, seeded with a soup or a
    famous pattern.

    Listed directly in the ConwaySource target, not in conway_core: both
    plugins share the class and not the `CFFGLPluginInfo` below, and putting
    either registration in the shared library would register both plugins into
    both bundles. The core is an OBJECT library because this registers itself
    from a file-scope constructor nothing references (see CMakeLists.txt).

    `SW Conway` is nine characters; the FFGL name field is char[ 16 ] and not
    null-terminated. `oxbow probe` reads it back the way a host does.
*/
namespace
{
class ConwaySource : public conway::ConwayPlugin
{
public:
	ConwaySource() :
		ConwayPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< ConwaySource >,// Create method
	"LF01",                       // Plugin unique ID of maximum length 4
	"SW Conway",                  // Plugin name
	2,                            // API major version number
	1,                            // API minor version number
	0,                            // Plugin major version number
	1,                            // Plugin minor version number
	FF_SOURCE,                    // Plugin type
	"Conway's Game of Life and its family of rules. Every cell is born, survives or dies by its eight neighbours, "
	"all at once, and the gliders, oscillators, guns and the soup settling into ash all fall out of that one rule. "
	"Seed it with a soup or a famous pattern; it reseeds itself when the field has settled.",
	"Conway FFGL source"          // About
);

extern "C" const char* ConwaySourceBuildStamp()
{
	return "conway " CONWAY_VERSION " source, built " __DATE__ " " __TIME__;
}
