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

#include "math/Random.h"

#include <ctime>
#if ANDROID
#include <ctime>
#include <cstdint>
#endif
thread_local Random::Generator * Random::rng = nullptr;

void Random::seed() {
#if ANDROID
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    u64 seedVal = (static_cast<u64>(ts.tv_sec) * 1000000000ULL) + static_cast<u64>(ts.tv_nsec);
    seed(seedVal);
#else    
	seed(u64(std::time(nullptr)));
#endif    
}

void Random::seed(u64 seedVal) {
	if(rng) {
		rng->seed(seedVal);
	} else {
		rng = new Generator(seedVal);
	}
}

void Random::shutdown() {
	delete rng;
	rng = nullptr;
}
