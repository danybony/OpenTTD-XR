/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file unix_main.cpp Main entry for Unix. */

#include "../../stdafx.h"
#include "../../openttd.h"
#include "../../crashlog.h"
#include "../../core/random_func.hpp"
#include "../../string_func.h"

#include <time.h>
#include <signal.h>

#ifdef __ANDROID__
#	include <SDL.h>
#	include <unistd.h>
#endif

#include "../../safeguards.h"

#ifdef __ANDROID__
/** Keep everything inside the app's private storage, where the activity also unpacked the game data. */
static void SetupAndroidPaths()
{
	const std::string base = SDL_AndroidGetInternalStoragePath();
	setenv("HOME", base.c_str(), 1);
	setenv("XDG_DATA_HOME", (base + "/data").c_str(), 1);
	setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
	setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
	if (chdir((base + "/game").c_str()) != 0) SDL_Log("No game data in %s", base.c_str());
}
#endif

int CDECL main(int argc, char *argv[])
{
#ifdef __ANDROID__
	SetupAndroidPaths();
#endif

	/* Make sure our arguments contain only valid UTF-8 characters. */
	std::vector<std::string_view> params;
	for (int i = 0; i < argc; ++i) {
		StrMakeValidInPlace(argv[i]);
		params.emplace_back(argv[i]);
	}

	CrashLog::InitialiseCrashLog();

	SetRandomSeed(time(nullptr));

	signal(SIGPIPE, SIG_IGN);

	return openttd_main(params);
}
