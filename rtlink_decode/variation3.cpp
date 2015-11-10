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

Common::Array<byte> v3Data;
uint v3StartCS, v3StartIP;

struct SelectorEntry {
	uint _selector;
	uint _referenceCount;
	SelectorEntry() : _selector(0), _referenceCount(0) {}
	SelectorEntry(uint selector) : _selector(selector), _referenceCount(1) {}
};

class SelectorArray : public Common::Array<SelectorEntry> {
public:
	void add(uint selector) {
		for (uint idx = 0; idx < size(); ++idx) {
			if ((*this)[idx]._selector == selector) {
				(*this)[idx]._referenceCount++;
				return;
			}
		}
		push_back(SelectorEntry(selector));
	}
	uint selector() const {
		assert(size() > 0);
		uint maxIdx, maxVal = 0;
		for (uint idx = 0; idx < size(); ++idx) {
			if ((*this)[idx]._referenceCount > maxVal) {
				maxVal = (*this)[idx]._referenceCount;
				maxIdx = idx;
			}
		}

		return (*this)[maxIdx]._selector;
	}
};

/**
 * We have a somewhat annoying job of converting the offsets that
 * RTLink stores into proper segment/offset pairs to store in the
 * produced executable's relocation table. It's important that the
 * selectors are correct, and one overall loaded segment may actually
 * consist of several smaller segments.
 *
 * Part of that is checking for relocation entries being part of far
 * call or far jump instructions, and using the given segment and offset
 * operands to also lock in segment ranges. However, since it's possible
 * that what we think is a far call/jump instruction may not be (it may
 * just be random data), that's why a ranking system is being used,
 * where the base segment used for each paragraph is the one that was
 * used most often during processing.
 */
Common::Array<SelectorArray> selectors;
Common::Array<uint> relocationOffsets;

void setSelector(int startSelector, uint offset) {
	for (uint idx = startSelector; idx <= (startSelector + offset / 16); ++idx) {
		selectors[idx].add(startSelector);
	}
}

/**
 * Creates relocation entries from the offsets
 */
void create_relocation_entries() {
	// Sort the relocation list
	Common::sort(relocationOffsets.begin(), relocationOffsets.end());

	// First scan through the offsets to see if any of the relocation entries
	// are part of far jumps or far calls, and if found, use the offsets in
	// the segments pointed to as a range of memory that must be in that segment
	for (uint idx = 0; idx < relocationOffsets.size(); ++idx) {
		byte opcode = v3Data[relocationOffsets[idx] - 3];
		if (opcode != 0x9A && opcode != 0xEA)
			continue;

		uint offset = READ_LE_UINT16(&v3Data[relocationOffsets[idx] - 2]);
		uint segment = READ_LE_UINT16(&v3Data[relocationOffsets[idx]]);
		setSelector(segment, offset);
	}

	// Write out the relocation list
	for (uint idx = 0; idx < relocationOffsets.size(); ++idx) {
		int offset = relocationOffsets[idx];
		int selector = selectors[offset / 16].selector();
		int offsetInSegment = offset - selector * 16;
		assert(offsetInSegment >= 0 && offsetInSegment <= 0xffff);
		relocations.push_back(RelocationEntry(selector, offsetInSegment));
	}
}

/**
 * Read in an encoded value from the header
 */
uint decodeValue() {
	byte buffer[4], v;
	buffer[0] = buffer[1] = buffer[2] = buffer[3] = 0;
	v = fExe.readByte();

	// If the high bit isn't set, it's a 7-bit value that can be returned
	if (!(v & 0x80))
		return v;

	v &= 0x7f;
	int numBytes = 1;
	byte *pByte = &buffer[2];

	if (v & 0x40) {
		v &= 0x3f;
		--pByte;
		++numBytes;
		
		if (buffer[0] & 0x20) {
			v &= 0x1f;
			--pByte;
			++numBytes;
			assert(v & 0x10);
		}
	}

	*pByte++ = v;
	fExe.read(pByte, numBytes);
	return MKTAG(buffer[0], buffer[1], buffer[2], buffer[3]);
}

bool getArrayValue(int mode, int index, byte buffer[], uint *value, uint *bx) {
	bool result = false;

	switch (mode) {
	case 0:
		*value = READ_LE_UINT32(buffer + READ_LE_UINT16(&buffer[8]) + (index - 1) * 4);
		*bx = 0;
		break;

	case 1:
		*value = READ_LE_UINT32(buffer + READ_LE_UINT16(&buffer[12]) + (index - 1) * 4);
		*bx = 0;
		break;

	case 3:
		*value = *bx << 16;
		*bx = 0;
		break;

	case 2:
	case 4: {
		uint tableOffset = (mode == 2) ? 0x10 : 0x4A;
		byte *pSrc = &buffer[tableOffset + (*bx - 1) * 8];

		if (*pSrc == 0x52) {
			*value = READ_LE_UINT16(pSrc + 1) << 16;
			*bx = READ_LE_UINT16(pSrc + 3);
			result = true;
		} else if (READ_LE_UINT16(pSrc + 1) == 0) {
			byte *pSrc2 = buffer + READ_LE_UINT16(buffer + 8) +
				((READ_LE_UINT16(pSrc + 3) - 1) * 4);
			*value = READ_LE_UINT32(pSrc2);
			*bx = READ_LE_UINT16(pSrc2 + 5);
		} else {
			uint value1 = READ_LE_UINT32(buffer + READ_LE_UINT16(buffer + 12) +
				((READ_LE_UINT16(pSrc + 1) - 1) * 4));
			uint value2 = READ_LE_UINT32(buffer + READ_LE_UINT16(buffer + 8) +
				((READ_LE_UINT16(pSrc + 3) - 1) * 4));
			value2 += pSrc[5];
			*value = value1 - value2;
		}
		break;
	}

	default:
		assert(0);
	}

	return result;
}

void process(uint selector, uint dataOffset) {
	uint numLoops = decodeValue();
	assert(numLoops <= 1);
	const uint bitMask = 0x1000;

	for (uint loopCtr = 0; loopCtr < numLoops; ++loopCtr) {
		// Lots of unknown code if certain bits are set. Part of it looks
		// like code to handle code blocks bigger than 64Kb
		uint v1 = fExe.readWord();
		assert((v1 & 0xfff) == 0);

		bool hasRelocations = (v1 & bitMask) != 0;
		int numRelocations = hasRelocations ? decodeValue() : 1;

		// Loop to record the offsets of relocation entries
		for (int relocCtr = 0; relocCtr < numRelocations; ++relocCtr) {
			uint offset = decodeValue();
			relocationOffsets.push_back(dataOffset + offset);
		}
	}
}

void loadSegments(byte buffer[], int numSegments) {
	uint tableOffset = READ_LE_UINT16(&buffer[8]);
	uint maxSegmentIndex = READ_LE_UINT16(&buffer[10]);

	for (int segmentNum = 0; segmentNum < numSegments; ++segmentNum) {
		uint segmentIndex = decodeValue();
		assert((segmentIndex - 1) < maxSegmentIndex);

		uint entryOffset = tableOffset + (segmentIndex - 1) * 4;
		assert(entryOffset < 8192);
		uint segmentOffset = READ_LE_UINT32(&buffer[entryOffset]);
		assert((segmentOffset & 0xffff) < 0x10);

		uint segmentSize = decodeValue();
		uint exeOffset = decodeValue();

		// rtlinkst had loop here to figure out header size
		const int config_headerSize = 0;
		bool isPresent = decodeValue() != 0;
		fExe.skip(config_headerSize);

		// The data for the segment
		uint startingOffset = (segmentOffset >> 16) * 16 + (segmentOffset & 0xf);
		if (segmentSize) {
			// Ensure the data array is big enough to hold next segment
			v3Data.resize(MAX(startingOffset + segmentSize, v3Data.size()));

			if (isPresent)
				// Read in data from the stream
				fExe.read(&v3Data[startingOffset], segmentSize);
			else
				Common::fill(&v3Data[startingOffset], &v3Data[startingOffset] + segmentSize, 0);
		}

		// Mark the range of selectors for the data block
		setSelector(segmentOffset >> 16, segmentSize);

		// Process the segment to handle any relocation entries
		process(segmentOffset >> 16, startingOffset);
	}
}

void handleExternalSegment(byte buffer[], int extraIndex, uint rtlSegmentId, const char *filename) {
	// Switch to the specified file
	fExe.close();
	fExe.open(filename);

	uint headerId = fExe.readWord();
	assert(headerId == 0x37BA);
	uint rtlType = fExe.readByte();
	assert((extraIndex == 0 && rtlType == 83) || (extraIndex == 1 && rtlType == 85));
	uint rtlId = fExe.readByte();
	assert(rtlId == rtlSegmentId);
	fExe.readWord();
	uint numSegments = fExe.readWord();
	uint v8 = fExe.readWord();

	uint fileOffset = 32;
	if (v8 >= 300) {
		fExe.seek(16);
		fileOffset = fExe.readWord() * 16;
	}

	fExe.seek(fileOffset);
	loadSegments(buffer, numSegments);
}

bool validateExecutableV3() {
	int fileOffset = scanExecutable((const byte *)"RTL", 3);
	if (fileOffset > 0x100) {
		printf("Possible version 3, but file missing data\n");
		return false;
	}

	// Initialize selectors list
	selectors.resize(0xffff);

	// Read in header data
	byte buffer[8192];
	fExe.seek(0x20);
	fExe.read(buffer, 8192);

	v3StartIP = READ_LE_UINT16(&buffer[0]);
	v3StartCS = READ_LE_UINT16(&buffer[2]);
	fileOffset = READ_LE_UINT16(&buffer[0x14]) + 32;
	int numSegments = READ_LE_UINT16(&buffer[0x16]);

	fExe.seek(fileOffset);
	loadSegments(buffer, numSegments);

	// Handle any extra segments
	for (uint idx = 0; idx < 2; ++idx) {
		if (!buffer[28 + idx * 2])
			continue;

		char filename[16];
		strcpy(filename, (const char *)&buffer[29 + idx * 12]);
		strcat(filename, ".RTL");

		handleExternalSegment(buffer, idx, buffer[28 + idx * 2], filename);
	}

	// Handle converting the offset list to proper relocation entries
	create_relocation_entries();

	// Re-open the main executable
	fExe.open(exeFilename);

	printf("Version 3 - rtlinkst.com usage detected.\n");
	return true;
}
