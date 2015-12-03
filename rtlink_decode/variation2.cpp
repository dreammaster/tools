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
 * Detects a version 2 executable. These are identifiable by the RTLink segment
 * having a relocation on the first word of the segment, so we scan for any
 * relocation with a 0 offset, and then look forward to see if recognisable
 * segment numbers follow it at the correct offset
 */
bool validateExecutableV2() {
	byte buffer[LARGE_BUFFER_SIZE];
	int dataIndex = 0;
	segmentsOffset = 0;

	// Iterate through the relocations looking for one with a 0 offset
	for (uint relIndex = 0; relIndex < relocations.size(); ++relIndex) {
		if (relocations[relIndex].getOffset() != 0)
			continue;

		// Read in data from the segment
		uint fileOffset = relocations[relIndex].fileOffset() + 48;
		fExe.seek(fileOffset);
		fExe.read(buffer, LARGE_BUFFER_SIZE);

		// Check to see if the segment number values we'd expect for the first two
		// segment entries are 2 and 3
		uint num1 = READ_LE_UINT16(buffer + 14);
		uint num2 = READ_LE_UINT16(buffer + 32 + 14);
		if (num1 == 2 && num2 == 3)
			return true;
	}

	return false;
}

/**
 * Loads the list of dynamic segments from version 2 executables. In version 2
 * executables, there'll be a relocation entry at the very start of the segment,
 * following by the segment list.
 */
bool loadSegmentListV2() {
	byte buffer[LARGE_BUFFER_SIZE];
	int dataIndex = 0;
	segmentsOffset = 0;

	// Iterate through the relocations looking for one with a 0 offset
	for (uint relIndex = 0; relIndex < relocations.size(); ++relIndex) {
		if (relocations[relIndex].getOffset() != 0)
			continue;

		// Read in data from the segment
		uint fileOffset = relocations[relIndex].fileOffset() + 48;
		fExe.seek(fileOffset);
		fExe.read(buffer, LARGE_BUFFER_SIZE);

		// Check to see if the segment number values we'd expect for the first two
		// segment entries are 2 and 3
		uint num1 = READ_LE_UINT16(buffer + 14);
		uint num2 = READ_LE_UINT16(buffer + 32 + 14);
		if (num1 == 2 && num2 == 3) {
			segmentsOffset = fileOffset;
			rtlinkSegment = relocations[relIndex].getSegment();
			rtlinkSegmentStart = fileOffset - 48;
			break;
		}
	}

	if (segmentsOffset == 0) {
		printf("Could not find the executable's segment list\n");
		return false;
	}

	// Iterate through the segments
	for (int offset = 0, segmentNum = 2; READ_LE_UINT16(buffer + offset + 14) == segmentNum;
	++segmentNum, offset += 32) {
		segmentsSize = (offset + 32) - segmentsOffset;
		segmentList.push_back(SegmentEntry());
		SegmentEntry &seg = segmentList[segmentList.size() - 1];
		byte *p = buffer + offset;

		// Get data for the entry
		seg.segmentIndex = segmentNum;
		seg.offset = segmentsOffset + offset;
		seg.loadSegment = READ_LE_UINT16(p);
		seg.filenameOffset = 0;
		seg.isExecutable = true;
		seg.headerOffset = READ_LE_UINT32(p + 8);
		seg.numRelocations = 0;
		seg.codeOffset = 0;
		seg.codeSize = 0;		// todo
	}

	// Iterate through the list to load the relocation entries from the start of that segment's data
	extraRelocations = 0;
	for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
		SegmentEntry &seg = segmentList[segmentNum];

		// Move to the start of the segment and read in it's details
		fExe.seek(seg.headerOffset);
		uint segmentParagraphs = fExe.readWord();
		uint headerParagraphs = fExe.readWord();
		fExe.readWord();
		uint16 relocationStart = fExe.readWord();
		seg.numRelocations = fExe.readWord();

		// Set the code file offset and size for the segment
		seg.codeOffset = seg.headerOffset + headerParagraphs * 16;
		seg.codeSize = (segmentParagraphs - headerParagraphs) * 16;
		assert((seg.codeSize % 16) == 0);

		// Get the list of relocations
		assert(relocationStart == 0);
		fExe.seek(6 + relocationStart * 4, SEEK_CUR);

		for (int relCtr = 0; relCtr < seg.numRelocations; ++relCtr) {
			uint offsetVal = fExe.readWord();
			uint segmentVal = fExe.readWord();
			assert((segmentVal * 16 + offsetVal) < seg.codeSize);

			++extraRelocations;
			RelocationEntry relEntry(segmentVal, offsetVal);
			relEntry._segmentIndex = segmentNum;
			seg.relocations.push_back(relEntry);
		}

		// Sort the list of relocations into relative order
		seg.relocations.sort();
	}

	// Sort the list so that any segments in the Ovl come first. This helps ensure the data
	// segment segment in the executable will come list
	segmentList.sort();

	return true;
}
