#include "Conway.h"

/**
    The effect: the clip seeds the field. Its bright cells are the live ones
    on Reseed, and Feed keeps breeding cells wherever it stays bright, so Life
    spills out of the picture's highlights.

    See SourcePlugin.cpp for why this file is listed in its own target.
*/
namespace
{
class ConwayEffect : public conway::ConwayPlugin
{
public:
	ConwayEffect() :
		ConwayPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< ConwayEffect >,// Create method
	"LF02",                       // Plugin unique ID of maximum length 4
	"SW Conway Over",             // Plugin name
	2,                            // API major version number
	1,                            // API minor version number
	0,                            // Plugin major version number
	1,                            // Plugin minor version number
	FF_EFFECT,                    // Plugin type
	"Conway's Game of Life grown from the clip: its bright cells seed the field, Feed keeps breeding cells where it "
	"stays bright, and the rule takes it from there. Threshold picks what is alive; Mix brings the clip back.",
	"Conway FFGL effect"          // About
);

extern "C" const char* ConwayEffectBuildStamp()
{
	return "conway " CONWAY_VERSION " effect, built " __DATE__ " " __TIME__;
}
