/*
 * Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
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

#ifndef ARX_CORE_FRAMESPIKES_H
#define ARX_CORE_FRAMESPIKES_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include "io/log/Logger.h"

/*!
 * Frame spike report: a frame much longer than the ones before it gets one log line
 *
 *   Frame spike: 412.3 ms | outside 0.4, sleep 0.0, poll 0.1, input 0.2, update 3.1, scene 401.0,
 *   swap 6.9, flush 0.0, rest 0.6 | draws 734 calls 398.2 ms (slowest 391.7) | programs 0 ...
 *
 * The phases are the parts of ArxGame::doFrame() (outside: from the end of the last frame to the
 * start of this one - the event loop), the counters what the frame spent in work that can stall
 * at any moment of the gameplay: draw calls (a new GPU pipeline is made inside the draw under
 * ANGLE), program builds, texture loads and uploads, resource reads, sound decoding and the wait
 * for the audio lock (the sound thread holds it while it decodes the streams).
 * The counters are atomic: some of them are counted on the sound thread too.
 */
namespace frameSpikes {

enum Phase {
	Outside,
	Sleep,
	Poll,
	Input,
	Update,
	Scene,
	Swap,
	Flush,
	PhaseCount
};

enum Counter {
	Draws,
	ProgramBuilds,
	TextureLoads,
	TextureUploads,
	ResourceReads,
	SoundDecode,
	AudioLockWait,
	AudioUpdate,
	CounterCount
};

struct CounterState {
	std::atomic<std::int64_t> calls{0};
	std::atomic<std::int64_t> us{0};
	std::atomic<std::int64_t> slowestUs{0};
};

inline CounterState g_counters[CounterCount];
inline std::int64_t g_phaseUs[PhaseCount] = { };  // main thread only
inline const char * g_note = nullptr;            // main thread only

[[nodiscard]] inline std::int64_t nowUs() noexcept {
	return std::chrono::duration_cast<std::chrono::microseconds>(
	         std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void record(Counter counter, std::int64_t us) noexcept {
	CounterState & state = g_counters[counter];
	state.calls.fetch_add(1, std::memory_order_relaxed);
	state.us.fetch_add(us, std::memory_order_relaxed);
	std::int64_t slowest = state.slowestUs.load(std::memory_order_relaxed);
	while(us > slowest
	      && !state.slowestUs.compare_exchange_weak(slowest, us, std::memory_order_relaxed)) { }
}

//! Times its own lifetime into a counter
class Scope {

	Counter m_counter;
	std::int64_t m_start;

public:

	explicit Scope(Counter counter) noexcept : m_counter(counter), m_start(nowUs()) { }
	~Scope() { record(m_counter, nowUs() - m_start); }

	Scope(const Scope &) = delete;
	Scope & operator=(const Scope &) = delete;

};

//! Times its own lifetime into a phase of the frame (main thread)
class PhaseScope {

	Phase m_phase;
	std::int64_t m_start;

public:

	explicit PhaseScope(Phase phase) noexcept : m_phase(phase), m_start(nowUs()) { }
	~PhaseScope() { g_phaseUs[m_phase] += nowUs() - m_start; }

	PhaseScope(const PhaseScope &) = delete;
	PhaseScope & operator=(const PhaseScope &) = delete;

};

//! What the frame did besides its usual work (a level change, a save): named in its line
inline void note(const char * what) noexcept {
	g_note = what;
}

/*!
 * Lives for the whole ArxGame::doFrame(): reports the frame when it was a spike, and starts
 * the counting for the next one
 *
 * A spike is a frame of 50 ms or more and at least 2.5 times the running average, so a device
 * that just runs slowly is not reported every frame; two lines a second at most.
 */
class Frame {

	static inline std::int64_t s_lastEnd = 0;
	static inline std::int64_t s_averageUs = 16667;
	static inline std::int64_t s_lastReport = 0;
	static inline unsigned s_suppressed = 0;

	std::int64_t m_start;

	static double ms(std::int64_t us) { return double(us) / 1000.0; }

	static void report(std::int64_t frameUs, std::int64_t restUs, const std::int64_t (&calls)[CounterCount],
	                   const std::int64_t (&us)[CounterCount], const std::int64_t (&slowest)[CounterCount]) {

		static const char * const phaseNames[PhaseCount] = {
			"outside", "sleep", "poll", "input", "update", "scene", "swap", "flush"
		};
		static const char * const counterNames[CounterCount] = {
			"draws", "programs", "texture loads", "texture uploads", "resource reads",
			"sound decode", "audio lock wait", "audio thread update"
		};

		std::ostringstream line;
		line << std::fixed << std::setprecision(1) << "Frame spike: " << ms(frameUs) << " ms |";
		for(int phase = 0; phase < PhaseCount; phase++) {
			line << (phase ? ", " : " ") << phaseNames[phase] << ' ' << ms(g_phaseUs[phase]);
		}
		line << ", rest " << ms(restUs);
		for(int counter = 0; counter < CounterCount; counter++) {
			if(calls[counter] == 0) {
				continue;
			}
			line << " | " << counterNames[counter] << ' ' << calls[counter] << "x " << ms(us[counter])
			     << " ms (slowest " << ms(slowest[counter]) << ')';
		}
		if(g_note) {
			line << " | " << g_note;
		}
		if(s_suppressed) {
			line << " | " << s_suppressed << " more spikes not reported";
			s_suppressed = 0;
		}
		LogWarning << line.str();
	}

public:

	Frame() noexcept : m_start(nowUs()) {
		if(s_lastEnd != 0) {
			g_phaseUs[Outside] += m_start - s_lastEnd;
		}
	}

	~Frame() {

		const std::int64_t end = nowUs();
		const std::int64_t frameUs = (s_lastEnd != 0) ? end - s_lastEnd : end - m_start;
		s_lastEnd = end;

		std::int64_t calls[CounterCount];
		std::int64_t us[CounterCount];
		std::int64_t slowest[CounterCount];
		for(int counter = 0; counter < CounterCount; counter++) {
			calls[counter] = g_counters[counter].calls.exchange(0, std::memory_order_relaxed);
			us[counter] = g_counters[counter].us.exchange(0, std::memory_order_relaxed);
			slowest[counter] = g_counters[counter].slowestUs.exchange(0, std::memory_order_relaxed);
		}

		if(frameUs >= 50000 && frameUs * 2 >= s_averageUs * 5) {
			if(end - s_lastReport >= 500000) {
				std::int64_t restUs = frameUs;
				for(std::int64_t phaseUs : g_phaseUs) {
					restUs -= phaseUs;
				}
				report(frameUs, restUs, calls, us, slowest);
				s_lastReport = end;
			} else {
				s_suppressed++;
			}
		}

		// Each frame moves the average a twentieth of the way: back in place some 60 frames after a spike
		s_averageUs += (frameUs - s_averageUs) / 20;

		for(std::int64_t & phaseUs : g_phaseUs) {
			phaseUs = 0;
		}
		g_note = nullptr;
	}

	Frame(const Frame &) = delete;
	Frame & operator=(const Frame &) = delete;

};

} // namespace frameSpikes

#endif // ARX_CORE_FRAMESPIKES_H
