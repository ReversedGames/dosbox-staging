/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2021-2021  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "fs_utils.h"

#include <algorithm>
#include <deque>
#include <string_view>

#include "control.h"
#include "std_filesystem.h"

std::deque<std_fs::path> common_paths(const std::string_view subdir)
{
	std::deque<std_fs::path> paths = {};

    auto maybe_emplace = [&](const std_fs::path& p) {
        const auto p_with_subdir = p / subdir;
        if (std_fs::exists(p_with_subdir))
            paths.emplace_back(p_with_subdir);
    };

#if defined(MACOSX)
    maybe_emplace(control->cmdline->GetExecutablePath() / "../Resources");
#else
	maybe_emplace(control->cmdline->GetExecutablePath());
#endif
    maybe_emplace("/usr/share/dosbox");
    maybe_emplace("/usr/local/share/dosbox");
	maybe_emplace(CROSS_GetPlatformConfigDir());
	maybe_emplace(std_fs::current_path());

	return paths;
}
