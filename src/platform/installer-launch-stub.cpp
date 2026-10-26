// Clip Atlas — the installer launch on platforms without one.
//
// There is no installer outside Windows, so neither path is known and nothing is ever started. CMake compiles this
// file instead of installer-launch-win.cpp on every platform but Windows.
#include "installer-launch.hpp"

#include <string>

namespace clip_atlas {

std::string own_module_path_utf8()
{
	return "";
}

std::string program_data_folder_utf8()
{
	return "";
}

std::string launch_installer(const std::string &, const std::string &)
{
	return "not supported on this platform";
}

} // namespace clip_atlas
