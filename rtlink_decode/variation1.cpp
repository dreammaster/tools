/* ScummVM - Graphic Adventure Engine
*
* ScummVM is the legal property of its developers, whose names
* are too numerous to list here. Please refer to the COPYRIGHT
* file distributed with this source distribution.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.

* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.

* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/

// HACK to allow building with the SDL backend on MinGW
// see bug #1800764 "TOOLS: MinGW tools building broken"
#ifdef main
#undef main
#endif // main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtlink_decode.h"
#include "common/algorithm.h"
#include "common/list.h"
#include "common/ptr.h"
#include "common/util.h"

#undef printf
#undef exit

/**
 * Loads the list of dynamic segments from version 1 executables
 */
bool loadSegmentListV1() {
	// First call the common method to load v1 & v3 segments
	if (!loadSegmentListV1V3())
		return false;

	// Further checks are only done when there's only one EXE rtlink segment and
	// some further segments in an OVL
	if (segmentList.size() < 2 || segmentList[segmentList.size() - 2].isExecutable)
		return true;

	// Okay. Here's where we deal with some weird shit. For at least Companions of Xanth,
	// there's an area at the beginning of the single Exe segment (that contains the 
	// various data segments) that's blank. And it's utilised twice in the running game..
	// first as the start of the dynamic area loaded into memory, but also as secondary
	// static segments in memory, since they fall before the point in memory where any of
	// the rtlink segments gets loaded.
	// To handle this properly, I get the difference between the effective starting segment
	// of the rtlink exe segment when the EXE is initially loaded, and get the difference
	// between that and the earliest load segment specified for any rtlink segment.
	// If the result is positive, then a new extra dummy rtlink segment is created that will
	// encompass the static part of the other rtlink segment. The remainder of the program
	// will then automatically handle references into this segment as if it were separate 

	// Find the earliest load segment
	int loadSegment = 0xffff;
	for (uint idx = 0; idx < segmentList.size(); ++idx)
		loadSegment = MIN(loadSegment, (int)segmentList[idx].loadSegment);

	int exeLoadSegment = segmentList[segmentList.size() - 1].loadSegment;
	int segmentDiff = exeLoadSegment - loadSegment;
	if (segmentDiff <= 0)
		// No static area, so we can exit the method now
		return true;

	// Create a new dummy EXE segment for the extra data
	segmentList.insert_at(segmentList.size() - 1, SegmentEntry());
	SegmentEntry &newSeg = segmentList[segmentList.size() - 2];
	SegmentEntry &exeSeg = segmentList[segmentList.size() - 1];

	newSeg.headerOffset = newSeg.codeOffset = exeSeg.headerOffset;
	newSeg.codeSize = segmentDiff * 16;
	newSeg.loadSegment = (newSeg.codeOffset - codeOffset) / 16;
	newSeg.isExecutable = true;
	newSeg.isDataSegment = false;

	// Fix the segment index in the data segment relocations, since it's index
	// in the segment list has been increased
	for (uint idx = 0; idx < exeSeg.relocations.size(); ++idx)
		exeSeg.relocations[idx]._segmentIndex = segmentList.size() - 1;

	return true;
}
