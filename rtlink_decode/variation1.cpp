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
 * Loads the list of dynamic segments from version 1 executables. For these,
 * we find an occurance of the program's own filename, which is used by the
 * segment list, and work backwards to load in all the segments.
 */
bool loadSegmentListV1V3() {
	byte buffer[LARGE_BUFFER_SIZE];
	int dataIndex = 0;

	exeNameOffset = scanExecutable((const byte *)exeFilename, strlen(exeFilename));
	if (exeNameOffset == -1) {
		printf("Could not find the executable's own filename within the file\n");
		return false;
	}

	int bufferStart = exeNameOffset - LARGE_BUFFER_SIZE;
	fExe.seek(bufferStart);
	fExe.read(buffer, LARGE_BUFFER_SIZE);

	// The segment list is a set of entries 18 bytes, bytes 14 & 15 of which are the segment number,
	// which should be in incrementing values. As such, we need to scan backwards through the loaded
	// buffer until we find decrementing values two bytes wide at intervals of 18 bytes aparat
	int offset;
	bool exeFilenameIsFirst = true;
	for (offset = LARGE_BUFFER_SIZE - 4; offset >= 18 * 5; --offset) {
		uint num5 = READ_LE_UINT16(buffer + offset);
		uint num4 = READ_LE_UINT16(buffer + offset - 18);
		uint num3 = READ_LE_UINT16(buffer + offset - 18 * 2);
		uint num2 = READ_LE_UINT16(buffer + offset - 18 * 3);
		uint num1 = READ_LE_UINT16(buffer + offset - 18 * 4);
		if (num5 == (num4 + 1) && num4 == (num3 + 1) && num3 == (num2 + 1) && num2 == (num1 + 1)) {
			// Bonza! We've found the the last entry of the list
			break;
		}

		// Check to see if the OVL version of the filename appears between the end of the segment
		// list and the EXE filename. This is needed to figure out which file each segment is using
		if (!strncmp((const char *)buffer + offset, ovlFilename, strlen(ovlFilename)))
			exeFilenameIsFirst = false;
	}
	if (offset < (18 * 5)) {
		printf("Could not find RTLink segment list\n");
		return false;
	}

	offset -= 14;
	uint segmentsEnd = bufferStart + offset + 18;

	// Move backwards through the segment list, loading the entries
	uint lowestFilenameOffset = 0xffff;
	uint firstSegmentOffset = 0;
	for (int segmentNum = READ_LE_UINT16(buffer + offset + 14);
	READ_LE_UINT16(buffer + offset + 14) == segmentNum; --segmentNum, offset -= 18) {
		segmentList.insert_at(0, SegmentEntry());
		SegmentEntry &seg = segmentList[0];
		byte *p = buffer + offset;

		// If set, it does some extra indexing that I haven't looked into
		assert(!(p[7] & 8));

		if (READ_LE_UINT16(buffer + offset + 14) != segmentNum)
			break;

		// Get data for the entry
		seg.segmentIndex = segmentNum;
		seg.offset = bufferStart + offset;
		seg.loadSegment = READ_LE_UINT16(p);
		seg.filenameOffset = READ_LE_UINT16(p + 2);
		seg.headerOffset = (READ_LE_UINT32(p + 4) & 0xffffff) << 4;
		seg.numRelocations = READ_LE_UINT16(p + 10);
		seg.codeOffset = seg.headerOffset + (((seg.numRelocations + 3) >> 2) << 4);
		seg.codeSize = READ_LE_UINT16(p + 16) << 4;
		assert((seg.codeSize % 16) == 0);

		firstSegmentOffset = seg.offset;

		// Keep track of the highest filename offset. This will be needed to figure 
		// out which filename to use
		if (seg.filenameOffset < lowestFilenameOffset)
			lowestFilenameOffset = seg.filenameOffset;
	}

	// Set the offset and size for the segment list
	segmentsOffset = bufferStart + offset + 18;
	segmentsSize = segmentsEnd - segmentsOffset;

	// Iterate through the list to set whether each segment is using the executable or OVL,
	// and to load the relocation entries from the start of that segment's data
	extraRelocations = 0;
	for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
		SegmentEntry &seg = segmentList[segmentNum];

		// Set executable flag
		seg.isExecutable = exeFilenameIsFirst == (seg.filenameOffset == lowestFilenameOffset);

		// Get a reference to the correct file, and move to the start of the segment
		File &file = seg.isExecutable ? fExe : fOvl;
		file.seek(seg.headerOffset);

		// Get the list of relocations
		for (int relCtr = 0; relCtr < seg.numRelocations; ++relCtr) {
			uint offsetVal = file.readWord();
			uint segmentVal = file.readWord();
			if (segmentVal == 0xffff && offsetVal == 0)
				continue;

			assert((offsetVal != 0) || (segmentVal != 0));
			assert(segmentVal >= seg.loadSegment);

			++extraRelocations;
			RelocationEntry relEntry(segmentVal - seg.loadSegment, offsetVal);
			relEntry._segmentIndex = segmentNum;
			seg.relocations.push_back(relEntry);
		}

		// Sort the list of relocations into relative order
		seg.relocations.sort();
	}

	// Sort the list so that any segments in the Ovl come first. This helps ensure the data
	// segment segment in the executable will come list
	segmentList.sort();

	// Scan through all the list of relocations to find the one with the file offset closest
	// to the start of the segments list. This will give us the program segment the segment
	// list is located in
	uint highestIndex = 0, highestOffset = 0;
	for (uint idx = 0; idx < relocations.size(); ++idx) {
		uint fileOffset = relocations[idx].fileOffset();
		if (fileOffset < segmentsOffset && fileOffset > highestOffset) {
			highestIndex = idx;
			highestOffset = fileOffset;
		}
	}

	rtlinkSegment = relocations[highestIndex].getSegment();
	rtlinkSegmentStart = codeOffset + rtlinkSegment * 16;

	return true;
}
