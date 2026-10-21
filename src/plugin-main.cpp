/*
Clip Atlas
Copyright (C) 2026 sxnfer

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include <exception>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// OBS calls these from C code, so an exception must not unwind into it. Catch and log instead.
bool obs_module_load(void)
{
	try {
		obs_log(LOG_INFO, "loaded v%s", PLUGIN_VERSION);
		return true;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "load failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "load failed: unknown exception");
	}
	return false;
}

void obs_module_unload(void)
{
	try {
		obs_log(LOG_INFO, "unloaded");
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "unload failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "unload failed: unknown exception");
	}
}

const char *obs_module_name(void)
{
	return obs_module_text("ModuleName");
}

const char *obs_module_description(void)
{
	return obs_module_text("ModuleDescription");
}
