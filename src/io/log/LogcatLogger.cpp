/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "io/log/LogcatLogger.h"
#include "SDL_log.h"

using namespace std;

namespace logger {

    void Logcat::log(const Source & file, int line, Logger::LogLevel level, std::string_view str) {
        string result;
        switch(level) {
            case Logger::Debug:    result += "[D]"; break;
            case Logger::Info:     result += "[I]"; break;
            case Logger::Console:  result += "[C]"; break;
            case Logger::Warning:  result += "[W]"; break;
            case Logger::Error:    result += "[E]"; break;
            case Logger::Critical: result += "[CRITICAL]"; break;
            case Logger::None: arx_unreachable();
        }
        result += ' ' + std::string (file.name) + ':';
        result += "  " + string (str);
        SDL_Log(result.c_str());
    }

    Backend * Logcat::get() {
        return new Logcat;
    }

} // namespace logger
