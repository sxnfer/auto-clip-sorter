// Clip Atlas — the dock's file actions on platforms without the Windows Shell.
//
// Nothing is opened, revealed, renamed or moved to the Recycle Bin here. Each call answers at once, on the calling thread,
// with "not supported on this platform" (a rename with "" as the new path), so the log says why the dock did nothing.
// CMake compiles this file instead of shell-ops-win.cpp on every platform but Windows.
#include "shell-ops.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <exception>
#include <memory>
#include <string>

namespace clip_atlas {

namespace {

constexpr const char kNotSupported[] = "not supported on this platform";

// Same as the Windows version: an exception from the callback is logged, not thrown.
void answer_done(const ShellOps::Done &done)
{
	if (!done)
		return;
	try {
		done(kNotSupported);
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "shell action callback failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "shell action callback failed: unknown exception");
	}
}

void answer_rename(const ShellOps::RenameDone &done)
{
	if (!done)
		return;
	try {
		done(kNotSupported, std::string());
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "shell action callback failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "shell action callback failed: unknown exception");
	}
}

} // namespace

struct ShellOps::Impl {};

ShellOps::ShellOps() : impl_(std::make_unique<Impl>()) {}

ShellOps::~ShellOps()
{
	shutdown();
}

void ShellOps::open(const std::string &, Done done)
{
	answer_done(done);
}

void ShellOps::open_folder(const std::string &, Done done)
{
	answer_done(done);
}

void ShellOps::reveal(const std::string &, Done done)
{
	answer_done(done);
}

void ShellOps::rename(const std::string &, const std::string &, RenameDone done)
{
	answer_rename(done);
}

void ShellOps::recycle(const std::string &, Done done)
{
	answer_done(done);
}

void ShellOps::shutdown()
{
	// Nothing is queued or started here, so there is nothing to stop.
}

ShellOps &shell_ops()
{
	// Deliberately never deleted, as in shell-ops-win.cpp.
	static ShellOps *ops = new ShellOps;
	return *ops;
}

} // namespace clip_atlas
