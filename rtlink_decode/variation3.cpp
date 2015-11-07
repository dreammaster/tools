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

/**
 * Read in an encode value from the header
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

void process(uint selector, uint dataOffset) {
	int numLoops = decodeValue();
	const uint bitMask = 0x1000;

	for (int loopCtr = 0; loopCtr < numLoops; ++loopCtr) {		
		// Lots of unknown code it certain bits are set. Part of it looks
		// like code to handle code blocks bigger than 64Kb
		uint v1 = fExe.readWord();
		assert((v1 & 0xfff) == 0);

		bool hasRelocations = (v1 & bitMask) != 0;
		int numRelocations = hasRelocations ? decodeValue() : 1;

		// Relocation offset loop
		for (int relocCtr = 0; relocCtr < numRelocations; ++relocCtr) {
			uint offset = decodeValue();

			// Add an entry to the main relocation list
			relocations.push_back(RelocationEntry(selector, offset));
		}
	}
}

bool validateExecutableV3() {
	int fileOffset = scanExecutable((const byte *)"RTL", 3);
	if (fileOffset > 0x100) {
		printf("Possible version 3, but file missing data\n");
		return false;
	}

	// Read in header data
	byte buffer[8192];
	fExe.seek(0x20);
	fExe.read(buffer, 8192);

	v3StartIP = READ_LE_UINT16(&buffer[0]);
	v3StartCS = READ_LE_UINT16(&buffer[2]);
	fileOffset = READ_LE_UINT16(&buffer[0x14]) + 32;
	uint tableOffset = READ_LE_UINT16(&buffer[8]);
	uint maxV1 = READ_LE_UINT16(&buffer[10]);
	int numSegments = READ_LE_UINT16(&buffer[0x16]);
	fExe.seek(fileOffset);

	for (int segmentNum = 0; segmentNum < numSegments; ++segmentNum) {
		uint relV1 = decodeValue();
		assert((relV1 - 1) < maxV1);

		uint entryOffset = tableOffset + (relV1 - 1) * 4;
		assert(entryOffset < 8192);
		uint offset = READ_LE_UINT32(&buffer[entryOffset]);

		uint segmentSize = decodeValue();
		uint exeOffset = decodeValue();
		// rtlinkst had loop here to figure out header size
		const int config_headerSize = 0;
		bool isPresent = decodeValue() != 0;
		fExe.skip(config_headerSize);

		// The data for the segment
		uint oldSize = v3Data.size();
		if (segmentSize) {
			v3Data.resize(oldSize + segmentSize);

			if (isPresent)
				// Read in data from the stream
				fExe.read(&v3Data[oldSize], segmentSize);
			else
				Common::fill(&v3Data[oldSize], &v3Data[oldSize] + segmentSize, 0);

		}

		// Process the segment to handle any relocation entries
		process(oldSize / 16, oldSize);
	}

	printf("Version 3 - rtlinkst.com usage detected.\n");
	return true;
}