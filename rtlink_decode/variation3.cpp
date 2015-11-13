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
uint v3StartCS = 0, v3StartIP = 0;
uint extraHeaderSize = 0;

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

#define RTLINK_VERSION 502
struct RTLinkReaderParams {
	uint value1;
	uint value2;
	uint value20;
	uint adjustMask, adjustShift;
	uint mode1Mask, mode2Mask;
	uint mode1Shift, mode2Shift;
	uint hasRelocationsMask;
	uint newBitData;
	uint v271;

	uint getShift(uint v) {
		uint shift = 0;
		while (!(v & 1)) {
			++shift;
			v >>= 1;
		}

		return shift;
	}

	void init(int version) {
		value1 = 1;
		value2 = 2;
		value20 = 0x20;
		adjustMask = 0x1C;
		newBitData = 0x2000;

		if (version > 410) {
			mode1Mask = 0xE00;
			mode2Mask = 0x1C0;
			hasRelocationsMask = 0x1000;
			v271 = 0x4000;	
		} else if (version > 310) {
			mode1Mask = 0xE00;
			mode2Mask = 0x1C0;
			hasRelocationsMask = 0x1000;
			v271 = 0;
		} else {
			mode1Mask = 0x300;
			mode2Mask = 0xC0;
			hasRelocationsMask = 0x400;
		}

		adjustShift = getShift(adjustMask);
		mode1Shift = getShift(mode1Mask);
		mode2Shift = getShift(mode2Mask);
	}
};
RTLinkReaderParams params;

struct RTLFileHeader {
	uint headerId;
	uint rtlType;
	uint rtlId;
	uint field4;
	uint numSegments;
	uint releaseNum;
	uint offsetParagraph;

	void load() {
		headerId = fExe.readWord();
		rtlType = fExe.readByte();
		rtlId = fExe.readByte();
		field4 = fExe.readWord();
		numSegments = fExe.readWord();
		fExe.skip(6);
		offsetParagraph = fExe.readWord();
	}
};
RTLFileHeader rtlHeader;

/*---------------------------------------------------------------*/

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

uint segOfsToLongOffset(uint v) {
	return (v >> 16) * 16 + (v & 0xffff);
}

bool getArrayValue(int mode, byte buffer[], uint *value, uint *idx) {
	bool result = false;

	switch (mode) {
	case 0:
		*value = READ_LE_UINT32(buffer + READ_LE_UINT16(&buffer[8]) + (*idx - 1) * 4);
		*idx = 0;
		break;

	case 1:
		*value = READ_LE_UINT32(buffer + READ_LE_UINT16(&buffer[12]) + (*idx - 1) * 4);
		*idx = 0;
		break;

	case 3:
		*value = *idx << 16;
		*idx = 0;
		break;

	case 2:
	case 4: {
		uint tableOffset = (mode == 2) ? 0x10 : 0x4A;
		byte *pSrc = &buffer[READ_LE_UINT16(&buffer[tableOffset]) + (*idx - 1) * 8];

		if (*pSrc != 0x52) {
			*value = READ_LE_UINT16(pSrc + 1) << 16;
			*idx = READ_LE_UINT16(pSrc + 3);
			result = true;
		} else if (READ_LE_UINT16(pSrc + 1) == 0) {
			byte *pSrc2 = buffer + READ_LE_UINT16(buffer + 8) +
				((READ_LE_UINT16(pSrc + 3) - 1) * 4);
			*value = READ_LE_UINT32(pSrc2);
			*idx = READ_LE_UINT16(pSrc + 5);
		} else {
			uint vSrc1 = READ_LE_UINT16(pSrc + 1);
			uint vSrc3 = READ_LE_UINT16(pSrc + 3);
			uint vSrc5 = READ_LE_UINT16(pSrc + 5);

			uint segOfs1 = READ_LE_UINT32(buffer + READ_LE_UINT16(buffer + 12) + (vSrc1 - 1) * 4);
			uint value1 = segOfsToLongOffset(segOfs1);
			uint segOfs2 = READ_LE_UINT32(buffer + READ_LE_UINT16(buffer + 8) + (vSrc3 - 1) * 4);
			uint value2 = segOfsToLongOffset(segOfs2);
			value2 += vSrc5;

			*value = segOfs1;
			*idx = value2 - value1;
		}
		break;
	}

	default:
		assert(0);
	}

	// Convert the segment/offset pair to just an offset
	*value = (*value >> 16) * 16 + (*value & 0xffff);

	// Return status flag
	return result;
}

void processSegmentRelocations(uint selector, uint dataOffset, byte buffer[]) {
	uint numLoops = decodeValue();
	uint arrayIndex1 = 0, arrayIndex2 = 0;
	uint adjustMode = 0;
	uint arrayValue1 = 0, arrayValue2 = 0;
	uint arrChange = 0;

	for (uint loopCtr = 0; loopCtr < numLoops; ++loopCtr) {
		uint bitData = fExe.readWord();
		
		if (bitData & params.value1) {
			arrayIndex1 = decodeValue();
			arrayIndex2 = decodeValue();
			adjustMode = (bitData & params.adjustMask) >> params.adjustShift;
			
			getArrayValue((bitData & params.mode1Mask) >> params.mode1Shift,
				buffer, &arrayValue1, &arrayIndex1);
			bool flag = getArrayValue((bitData & params.mode2Mask) >> params.mode2Shift,
				buffer, &arrayValue2, &arrayIndex2);

			if (flag) {
				arrayValue1 = arrayValue2;
				bitData = params.newBitData;
			}
			arrayValue2 += arrayIndex2;

			if (bitData & params.value2) {
				uint paragraphBase = arrayValue1 & 0xFFFFFFF0;
				arrayValue2 -= paragraphBase;
				arrayValue1 = paragraphBase / 16;
			}
		}

		arrChange = 0;
		uint innerLoopCount = ((bitData & params.value20) &&
			(bitData & params.hasRelocationsMask)) ? decodeValue() : 1;
		
		for (uint loopCtr2 = 0; loopCtr2 < innerLoopCount; ++loopCtr2) {
			uint hasRelocations;
			if (bitData & params.value20) {
				uint v = decodeValue();
				arrChange = v >> 1;
				hasRelocations = v & 1;
			} else {
				hasRelocations = bitData & params.hasRelocationsMask;
			}
			uint numRelocations = hasRelocations ? decodeValue() : 1;

			// Loop to record the offsets of relocation entries
			for (uint relocCtr = 0; relocCtr < numRelocations; ++relocCtr) {
				uint offset = decodeValue() - extraHeaderSize;

				if (!(bitData & params.value1)) {
					// It's a standard relocation entry
					relocationOffsets.push_back(dataOffset + offset);
				} else {
					// It's not. Here's where shit gets serious
					uint arrOffset = arrayValue2 + arrChange;
					byte *pDest = &v3Data[dataOffset + offset];

					if (bitData & params.value2) {
						switch (adjustMode) {
						case 0:
							*pDest += arrOffset & 0xff;
							break;
						case 1:
							WRITE_LE_UINT16(pDest, READ_LE_UINT16(pDest) + (arrOffset & 0xffff));
							break;
						case 4:
							*pDest += (arrOffset >> 8) & 0xff;
							break;
						case 3:
							WRITE_LE_UINT16(pDest, READ_LE_UINT16(pDest) + (arrOffset & 0xffff));
							pDest += 2;
							offset += 2;
							// Deliberate fall-through
						case 2: {
							if (bitData & params.v271) {
								WRITE_LE_UINT16(pDest, READ_LE_UINT16(pDest) << 12);
							}

							WRITE_LE_UINT16(pDest, READ_LE_UINT16(pDest) + (arrayValue1 & 0xffff));
							uint mode1 = (bitData & params.mode1Mask) >> params.mode1Shift;
							if (mode1 != 3 && !(bitData & params.newBitData)) {
								relocationOffsets.push_back(dataOffset + offset);
							}
							break;
						}

						default:
							assert(0);
						}
					} else {
						assert(0);	// TODO
						switch (adjustMode) {
						case 0:
							// TODO
							break;
						case 1:
							// TODO
							break;
						default:
							assert(0);
							break;
						}
					}
				}
			}
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
		uint exeSegmentOffset = decodeValue();
		
		// Figure out extra needed header data
		extraHeaderSize = 0;
		uint arrOffset = READ_LE_UINT16(&buffer[0x38]);
		for (uint idx = 0; idx < READ_LE_UINT16(&buffer[0x3A]); ++idx, arrOffset += 11) {
			if (arrOffset == READ_LE_UINT16(&buffer[arrOffset])) {
				arrOffset += 2;
				if (rtlHeader.rtlType != 69) {
					arrOffset += 3;
					if (rtlHeader.rtlType != 85) {
						arrOffset += 3;
						assert(rtlHeader.rtlType == 83);
					}
				}

				exeSegmentOffset = READ_LE_UINT16(&buffer[arrOffset]);
				extraHeaderSize = READ_LE_UINT16(&buffer[arrOffset + 2]);
				segmentSize -= extraHeaderSize;
				break;
			}
		}

		bool isPresent = decodeValue() != 0;

		// Skip over any extra header
		fExe.skip(extraHeaderSize);

		// The data for the segment
		uint startingOffset = (segmentOffset >> 16) * 16 + (segmentOffset & 0xf);
		startingOffset += exeSegmentOffset;
		
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
		setSelector(startingOffset / 16, segmentSize);

		// Process the segment to handle any relocation entries
		processSegmentRelocations(segmentOffset >> 16, startingOffset, buffer);
	}
}

void handleExternalSegment(byte buffer[], int extraIndex, uint rtlSegmentId, const char *filename) {
	// Switch to the specified file
	fExe.close();
	fExe.open(filename);
	rtlHeader.load();

	assert(rtlHeader.headerId == 0x37BA);
	assert((extraIndex == 0 && rtlHeader.rtlType == 83) ||
		(extraIndex == 1 && rtlHeader.rtlType == 85));
	assert(rtlHeader.rtlId == rtlSegmentId);

	uint fileOffset = (rtlHeader.releaseNum < 300) ? 32 :
		rtlHeader.offsetParagraph * 16;

	fExe.seek(fileOffset);
	loadSegments(buffer, rtlHeader.numSegments);
}

bool validateExecutableV3() {
	int fileOffset = scanExecutable((const byte *)"RTL", 3);
	if (fileOffset > 0x100) {
		printf("Possible version 3, but file missing data\n");
		return false;
	}

	// Initialize params for RTLink reader
	params.init(RTLINK_VERSION);

	// Initialize selectors list
	selectors.resize(0xffff);

	// Set up the v3Data with the contents of the starting executable.
	// This, along with following code, may not be needed for the final
	// decoded executable, I'm not sure. But it makes comparing against
	// a raw dump taken in DosBox easier
	fExe.seek(8);
	uint headerParagraphs = fExe.readWord();
	fExe.seek(headerParagraphs * 16);
	uint codeSize = fExe.size() - fExe.pos();
	v3Data.resize(codeSize);
	fExe.read(&v3Data[0], codeSize);

	// Read in header data
	byte buffer[8192];
	fExe.seek(0x20);
	fExe.read(buffer, 8192);

	v3StartIP = READ_LE_UINT16(&buffer[0]);
	v3StartCS = READ_LE_UINT16(&buffer[2]);
	fileOffset = READ_LE_UINT16(&buffer[0x14]) + 32;
	uint movedCodeSize = READ_LE_UINT32(&buffer[0x18]);
	int numSegments = READ_LE_UINT16(&buffer[0x16]);

	// Simulate the code that moves the bulk of the EXE higher in memory
	fExe.seek(14);
	uint ssSeg = fExe.readWord();
	uint movedCodeOffset = ssSeg * 16 - movedCodeSize;
	
	v3Data.resize(MAX(v3Data.size(), movedCodeOffset + movedCodeSize));
	fExe.seek(fileOffset);
	fExe.read(&v3Data[movedCodeOffset], movedCodeSize);

	// Piece together the initial segments that form up the low portion
	// of memory for the executable
	fExe.seek(fileOffset);
	loadSegments(buffer, numSegments);

	// Handle any extra segments in secondary RTL files
	for (uint idx = 0; idx < 2; ++idx) {
		if (!buffer[28 + idx * 12])
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
