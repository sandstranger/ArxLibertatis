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
/* Based on:
===========================================================================
ARX FATALIS GPL Source Code
Copyright (C) 1999-2010 Arkane Studios SA, a ZeniMax Media company.

This file is part of the Arx Fatalis GPL Source Code ('Arx Fatalis Source Code'). 

Arx Fatalis Source Code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public 
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Arx Fatalis Source Code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Arx Fatalis Source Code.  If not, see 
<http://www.gnu.org/licenses/>.

In addition, the Arx Fatalis Source Code is also subject to certain additional terms. You should have received a copy of these 
additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Arx 
Fatalis Source Code. If not, please request a copy in writing from Arkane Studios at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing Arkane Studios, c/o 
ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
===========================================================================
*/
// Code: Cyril Meynier
//
// Copyright (c) 1999-2001 ARKANE Studios SA. All rights reserved

#include "ai/PathFinderManager.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <utility>
#include <vector>

#include "ai/Anchors.h"
#include "ai/PathFinder.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "graphics/Math.h"
#include "platform/Thread.h"
#include "platform/profiler/Profiler.h"
#include "scene/Light.h"


static constexpr float PATHFINDER_HEURISTIC_MIN = 0.2f;
static constexpr float PATHFINDER_HEURISTIC_MAX = PathFinder::HEURISTIC_MAX;
static constexpr float PATHFINDER_HEURISTIC_RANGE = PATHFINDER_HEURISTIC_MAX - PATHFINDER_HEURISTIC_MIN;
static constexpr float PATHFINDER_DISTANCE_MAX = 5000.0f;

/*
 * The pathfinder thread used to read the requesting NPC while the game changed it, and to write the
 * path into the NPC (pathfind.list, then pathfind.listnb) while the game read, reset and freed them,
 * with nothing ordering the two. On ARM's weak memory model the game could see the count before the
 * list (and index a null or freed list), or free a list the thread had just stored. Now a request
 * carries what the search needs, copied from the NPC when it is queued, the thread never touches
 * the NPC, and the paths come back through a mailbox that the game empties once per frame, on its
 * own thread (EERIE_PATHFINDER_Collect).
 */
struct PathFinderJob {
	
	PATHFINDER_REQUEST request = { };
	EntityHandle handle;
	Behaviour behavior;
	float behaviorParam = 0.f;
	Vec3f pos = Vec3f(0.f);
	Vec3f target = Vec3f(0.f);
	float radius = 0.f;
	float height = 0.f;
	
};

struct PathFinderResult {
	
	Entity * entity = nullptr; // only compared, never dereferenced off the game's thread
	EntityHandle handle;
	std::vector<long> path;
	
};

class PathFinderThread final : public StoppableThread {
	
	std::mutex m_mutex; // the queue and the results
	std::mutex m_searchMutex; // held through a search: clearQueue() waits for it, g_anchors goes after
	std::list<PathFinderJob> m_queue;
	std::vector<PathFinderResult> m_results;
	std::atomic<bool> m_busy;
	
	void run() override;
	void search(const PathFinderJob & job, PathFinder & pathfinder);
	
public:
	
	PathFinderThread() : m_busy(false) { }
	
	void queueRequest(const PATHFINDER_REQUEST & request);
	
	size_t queueSize() {
		
		std::scoped_lock lock(m_mutex);
		
		return m_queue.size();
	}
	
	void clearQueue() {
		
		std::scoped_lock searchLock(m_searchMutex);
		std::scoped_lock lock(m_mutex);
		
		m_queue.clear();
		m_results.clear();
	}
	
	std::vector<PathFinderResult> takeResults() {
		
		std::scoped_lock lock(m_mutex);
		
		return std::exchange(m_results, std::vector<PathFinderResult>());
	}
	
	bool isBusy() const {
		return m_busy.load();
	}
	
};

void PathFinderThread::queueRequest(const PATHFINDER_REQUEST & request) {
	
	arx_assert(request.entity && (request.entity->ioflags & IO_NPC));
	
	// What the search needs from the NPC, read here on the game's thread
	PathFinderJob job;
	job.request = request;
	job.handle = request.entity->index();
	job.behavior = request.entity->_npcdata->behavior;
	job.behaviorParam = request.entity->_npcdata->behavior_param;
	job.pos = request.entity->pos;
	job.target = request.entity->target;
	job.radius = request.entity->physics.cyl.radius;
	job.height = request.entity->physics.cyl.height;
	
	std::scoped_lock lock(m_mutex);
	
	// A path not collected yet answers an older request of this NPC
	m_results.erase(std::remove_if(m_results.begin(), m_results.end(), [&](const PathFinderResult & result) {
		return result.entity == request.entity;
	}), m_results.end());
	
	// If this NPC is already requesting a Pathfinding then either
	// try to Override it or add it to queue if it is currently being
	// processed.
	for(PathFinderJob & oldJob : m_queue) {
		if(oldJob.request.entity == request.entity) {
			oldJob = job;
			return;
		}
	}
	
	if(!m_queue.empty() && (job.behavior & (BEHAVIOUR_MOVE_TO | BEHAVIOUR_FLEE | BEHAVIOUR_LOOK_FOR))) {
		// priority: insert as second element of queue
		m_queue.insert(++m_queue.begin(), job);
	} else {
		m_queue.push_back(job);
	}
	
}

void PathFinderThread::search(const PathFinderJob & job, PathFinder & pathfinder) {
	
	if(job.behavior == BEHAVIOUR_NONE) {
		return;
	}
	
	ARX_PROFILE_FUNC();
	
	pathfinder.setCylinder(job.radius, job.height);
	
	const Behaviour behavior = job.behavior;
	const long from = job.request.from;
	const long to = job.request.to;
	
	float distance;
	if(behavior & (BEHAVIOUR_MOVE_TO | BEHAVIOUR_GO_HOME)) {
		distance = fdist(g_anchors[from].pos, g_anchors[to].pos);
	} else if(behavior & (BEHAVIOUR_WANDER_AROUND | BEHAVIOUR_FLEE | BEHAVIOUR_HIDE)) {
		distance = job.behaviorParam;
	} else if(behavior & BEHAVIOUR_LOOK_FOR) {
		distance = fdist(job.pos, job.target);
	} else {
		return;
	}
	float heuristic = PATHFINDER_HEURISTIC_MAX;
	if(distance < PATHFINDER_DISTANCE_MAX) {
		heuristic = PATHFINDER_HEURISTIC_MIN + PATHFINDER_HEURISTIC_RANGE * (distance / PATHFINDER_DISTANCE_MAX);
	}
	pathfinder.setHeuristic(heuristic);
	
	bool stealth = behavior.hasAll(BEHAVIOUR_SNEAK | BEHAVIOUR_HIDE);
	
	PathFinder::Result result;
	if(behavior & (BEHAVIOUR_MOVE_TO | BEHAVIOUR_GO_HOME)) {
		
		pathfinder.move(from, to, result, stealth);
		
	} else if(behavior & BEHAVIOUR_WANDER_AROUND) {
		
		pathfinder.wanderAround(from, distance, result, stealth);
		
	} else if(behavior & (BEHAVIOUR_FLEE | BEHAVIOUR_HIDE)) {
		
		float safeDistance = distance + fdist(job.target, job.pos);
		pathfinder.flee(from, job.target, safeDistance, result, stealth);
		
	} else if(behavior & BEHAVIOUR_LOOK_FOR) {
		
		float radius = job.behaviorParam;
		pathfinder.lookFor(from, job.target, radius, result, stealth);
		
	}
	
	PathFinderResult found;
	found.entity = job.request.entity;
	found.handle = job.handle;
	found.path.assign(result.begin(), result.end());
	
	std::scoped_lock lock(m_mutex);
	m_results.push_back(std::move(found));
	
}

void PathFinderThread::run() {
	
	PathFinder pathfinder(g_anchors.size(), g_anchors.data(), &g_staticLights);
	
	for(; !isStopRequested(); sleep(10ms)) {
		
		// Taken before the job: once clearQueue() has it, no job taken before the clear is searched
		std::scoped_lock searchLock(m_searchMutex);
		
		PathFinderJob job;
		{
			std::scoped_lock lock(m_mutex);
			if(m_queue.empty()) {
				continue;
			}
			job = std::move(m_queue.front());
			m_queue.pop_front();
		}
		
		m_busy = true;
		search(job, pathfinder);
		m_busy = false;
		
	}
	
}

static PathFinderThread * g_pathFinderThread = nullptr;

// Adds a Pathfinder Search Element to the pathfinder queue.
bool EERIE_PATHFINDER_Add_To_Queue(const PATHFINDER_REQUEST & request) {
	
	if(!g_pathFinderThread) {
		return false;
	}
	
	g_pathFinderThread->queueRequest(request);
	
	return true;
}

void EERIE_PATHFINDER_Collect() {
	
	if(!g_pathFinderThread) {
		return;
	}
	
	for(PathFinderResult & result : g_pathFinderThread->takeResults()) {
		
		Entity * io = entities.get(result.handle);
		if(!io || io != result.entity || !(io->ioflags & IO_NPC) || !io->_npcdata->pathfind.pathwait) {
			continue; // gone, or not waiting for a path any more
		}
		
		IO_PATHFIND & pathfind = io->_npcdata->pathfind;
		delete[] pathfind.list;
		pathfind.list = nullptr;
		if(!result.path.empty()) {
			pathfind.list = new long[result.path.size()];
			std::copy(result.path.begin(), result.path.end(), pathfind.list);
		}
		pathfind.listnb = long(result.path.size());
		
	}
	
}

long EERIE_PATHFINDER_Get_Queued_Number() {
	
	if(!g_pathFinderThread) {
		return 0;
	}
	
	return g_pathFinderThread->queueSize();
}

bool EERIE_PATHFINDER_Is_Busy() {
	
	if(!g_pathFinderThread) {
		return false;
	}
	
	return g_pathFinderThread->isBusy();
}

void EERIE_PATHFINDER_Clear() {
	
	if(!g_pathFinderThread) {
		return;
	}
	
	g_pathFinderThread->clearQueue();
}

void EERIE_PATHFINDER_Release() {
	
	if(!g_pathFinderThread) {
		return;
	}
	
	g_pathFinderThread->clearQueue();
	g_pathFinderThread->stop();
	
	delete g_pathFinderThread, g_pathFinderThread = nullptr;
}

void EERIE_PATHFINDER_Create() {
	
	if(g_pathFinderThread) {
		EERIE_PATHFINDER_Release();
	}
	
	g_pathFinderThread = new PathFinderThread();
	g_pathFinderThread->setThreadName("Pathfinder");
	g_pathFinderThread->start();
}
