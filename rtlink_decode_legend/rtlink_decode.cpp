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

void error(char const *s, ...) {
	printf(s);
	exit(1);
}

File fExe, fOvl, fOut;
char exeFilename[MAX_FILENAME_SIZE];
char ovlFilename[MAX_FILENAME_SIZE];
char outputFilename[MAX_FILENAME_SIZE];
uint32 codeOffset, exeNameOffset;

uint16 relocOffset, extraRelocations;
Common::Array<uint32> relocations; 
Common::Array<uint32> relocationOffsets; 


uint32 dataStart, dataSize;
uint16 dataSegAdjust;

uint32 jumpOffset, segmentsOffset, segmentsSize;
typedef Common::List<JumpEntry> JumpEntryList;
JumpEntryList jumpList;
typedef Common::Array<SegmentEntry> SegmentEntryArray;
SegmentEntryArray segmentList;

bool infoFlag = false, processFlag = false, dataFlag = false;

#define SEG_OFS_TO_OFFSET(seg,ofs) (codeOffset + (seg << 4) + ofs)
#define SEGOFS_TO_OFFSET(segofs) (codeOffset + ((segofs >> 16) << 4) + (segofs & 0xffff))

/*--------------------------------------------------------------------------
 * Support functions
 *
 *--------------------------------------------------------------------------*/

int memScan(byte *data, int dataLen, const byte *s, int sLen) {
	for (int index = 0; index < dataLen - sLen - 1; ++index) {
		if (!strncmp((const char *)&data[index], (const char *)s, sLen))
			return index;
	}

	return -1;
}

int scanExecutable(const byte *data, int count) {
	byte buffer[BUFFER_SIZE];
	assert(count < BUFFER_SIZE);

	fExe.seek(0);
	do {
		int fileOffset = fExe.pos();
		fExe.read(buffer, BUFFER_SIZE);

		int dataIndex = memScan(buffer, BUFFER_SIZE - count, data, count);
		if (dataIndex >= 0) {
			int offset = fileOffset + dataIndex;
			fExe.seek(offset);
			return offset;
		}

		// Move slightly backwards in the file before the next iteration and read,
		// just in case the sequence we're looking for falls across the buffer boundary
		fExe.seek(-(BUFFER_SIZE - 1), SEEK_CUR);
	} while (!fExe.eof());

	return -1;
}

int relocIndexOf(uint32 offset) {
	for (uint i = 0; i < relocations.size(); ++i) {
		if (SEGOFS_TO_OFFSET(relocations[i]) == offset)
			return i;
	}

	return -1;
}

SegmentEntry *getSegment(int segmentIndex) {
	for (uint idx = 0; idx < segmentList.size(); ++idx) {
		SegmentEntry &se = segmentList[idx];
		if (se.segmentIndex == segmentIndex)
			return &se;
	}

	return nullptr;
}

void copyBytes(int numBytes) {
	byte buffer[BUFFER_SIZE];

	while (numBytes > BUFFER_SIZE) {
		fExe.read(buffer, BUFFER_SIZE);
		fOut.write(buffer, BUFFER_SIZE);
		numBytes -= BUFFER_SIZE;
	}

	if (numBytes > 0) {
		fExe.read(buffer, numBytes);
		fOut.write(buffer, numBytes);
	}
}

uint32 relocationAdjust(uint32 v) {
	uint16 segVal = v >> 16;
	uint16 ofsVal = v & 0xffff;
	uint32 offset = ((uint32)segVal << 4) + ofsVal + codeOffset;

	uint16 segmentVal = segVal;
	if (offset > dataStart + dataSize)
		// Relocation entry within dynamic code, adjust it backwards
		segmentVal = segVal - (dataSize >> 4);
	else if (offset > dataStart)
		// Adjustment of data segment relocation entry to end of file
		segmentVal = segVal + dataSegAdjust;

	return ((uint32)segmentVal << 16) | ofsVal;
}

uint32 segmentAdjust(uint16 v) {
	if (v < ((dataStart - codeOffset) >> 4))
		// Segment points to base code areas
		return v;
	else if (v < ((dataStart + dataSize - codeOffset) >> 4))
		// Segment points to data segment, adjust it upwards
		return v + dataSegAdjust;

	// Segment points to a dynamic code segment, so adjust it down
	return v - (dataSize >> 4);
}

/*--------------------------------------------------------------------------
 * Main functions
 *
 *--------------------------------------------------------------------------*/

// checkCommandLine
// Scans the command line for options and switches

void checkCommandLine(int argc, char *argv[]) {
	if (argc == 1) {
		printf("RTLink(R)/Plus Legend Entertainment executable decoder -- Version 1.0\n\n\
			   Usage: rtlink_decode Input.exe [Output.exe]\n");
		exit(0);
	}

	// Set up the executable filename
	strcpy(exeFilename, argv[1]);
	_strupr(exeFilename);

	// Set up an optional OVL filename with a .OVL extension
	strcpy(ovlFilename, exeFilename);
	char *p = strchr(ovlFilename, '.');
	if (p) {
		strcpy(p, ".OVL");
	}

	// Handle an output filename, if any
	if (argc == 2) {
		infoFlag = true;
	} else {
		strcpy(outputFilename, argv[2]);
	}
}

/**
 * Validates that the specified file is an RTLink-encoded executable
 */
const char *RTLinkStr = ".RTLink(R)/Plus";
#define RTLINK_STR_SIZE 15

bool validateExecutable() {
	char mzBuffer[2];
	fExe.seek(0);
	fExe.read(mzBuffer, 2);
	
	if (strncmp(mzBuffer, "MZ", 2) != 0) {
		printf("The specified file is not a valid executable");
		return false;
	}

	// Go and grab needed information from the EXE header
	fExe.seek(6);
	int numRelocations = fExe.readWord();
	codeOffset = fExe.readWord() << 4;
	fExe.seek(24);
	relocOffset = fExe.readWord();

	// Get the relocation list
	fExe.seek(relocOffset);
	for (int i = 0; i < numRelocations; ++i) 
		relocations.push_back(fExe.readLong());

	// Check for the RTLink string
	if (scanExecutable((const byte *)RTLinkStr, RTLINK_STR_SIZE) == -1) {
		printf("RTLink(R)/Plus identifier not found in specified executable\n");
		return false;
	} else {
		printf("Found RTLink(R)/Plus identifier in executable\n");
		return true;
	}
}

/**
 * Loads the list of dynamic segments from the executable
 */
bool loadSegmentList() {
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
			segmentList.resize(num5);
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

	// Move backwards through the segment list, loading the entries
	uint lowestFilenameOffset = 0xffff;
	offset -= 14;
	for (int segmentNum = (int)segmentList.size(); segmentNum > 0; --segmentNum, offset -= 18) {
		SegmentEntry &seg = segmentList[segmentNum - 1];
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
		seg.codeOffset = seg.headerOffset + (((seg.relocations.size() + 3) >> 2) << 4);
		seg.codeSize = READ_LE_UINT16(p + 16) << 4;

		// Keep track of the highest filename offset. This will be needed to figure 
		// out which filename to use
		if (seg.filenameOffset < lowestFilenameOffset)
			lowestFilenameOffset = seg.filenameOffset;
	}

	// Iterate through the list to set whether each segment is using the executable or OVL,
	// and to load the relocation entries from the start of that segment's data
	for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
		SegmentEntry &seg = segmentList[segmentNum];
		if (!seg.filenameOffset)
			continue;

		// Set executable flag
		seg.isExecutable = exeFilenameIsFirst == (seg.filenameOffset == lowestFilenameOffset);

		// Get a reference to the correct file, and move to the start of the segment
		File &file = seg.isExecutable ? fExe : fOvl;
		file.seek(seg.headerOffset);

		// Get the list of relocations
		for (int relCtr = 0; relCtr < seg.numRelocations; ++relCtr, ++extraRelocations) {
			uint offsetVal = file.readWord();
			uint segmentVal = file.readWord();
			if (segmentVal == 0xffff && offsetVal == 0)
				continue;

			assert((offsetVal != 0) || (segmentVal != 0));
			assert(segmentVal >= seg.loadSegment);

			uint32 v = ((segmentVal - seg.loadSegment) * 16) + offsetVal;
			assert(v <= seg.codeSize);

			seg.relocations.push_back(v);
		}

		// Sort the list of relocations into relative order
		Common::sort(seg.relocations.begin(), seg.relocations.end());
	}

	return true;
}


/**
 * Load in the list of jump thunks that act as stubs for calling methods in dynamic segments
 */
bool loadJumpList() {
	byte byteVal;
	while ((byteVal = fExe.readByte()) != 0xE8) {
		if (byteVal > 0 && byteVal < 32) {
			// After the filename (which is used by an earlier segment list), there may be 
			// another ASCII filename for an overlay file, and then after that the thunk list.
			// So if we get any kind of low value, then something's screwed up
			printf("Couldn't resolve jump list\n");
			return false;
		}
	}

	jumpOffset = fExe.pos() - 1;
	uint16 aliasSegment = 0;
	int index;

	while (!fExe.eof() && (byteVal == 0xE8)) {
		uint32 fileOffset = fExe.pos() - 1;

		// Skip over the offset for the thunk call that loads the dynamic segment
		fExe.readWord();

		byte jmpByte = fExe.readByte();
		if (jmpByte != 0xea)
			// It's not a jmp statement, so reached end of list
			break;

		fExe.readWord();
		uint16 segment = fExe.readWord();

		JumpEntry rec;
		rec.offset = fileOffset;
		rec.altFlag = false;
		rec.segmentIndex = fExe.readWord();
		jumpList.push_back(rec);

		byteVal = fExe.readByte();

		// Create a new relocation entry for the replacement FAR JMP instruction we'll be creating
		index = relocIndexOf(fileOffset + 3);
		if (index == -1) {
			if (aliasSegment == 0) {
				printf("Found missing relocation before alias segment could be determined\n");
				return false;
			}

			relocations.push_back((aliasSegment << 16) + (fileOffset + 3 - (aliasSegment << 4)));
			printf("Added entry for file offset %xh", fileOffset + 3,
				relocations[relocations.size() - 1]);
		}
	}

	return true;
}

// loadDataDetails
// Loads the start and size of the data segment

const char *DataSegmentStr = "MS Run-Time Library";

bool loadDataDetails() {
	byte buffer[BUFFER_SIZE + 16];
	int dataIndex = 0;

	// Scan for the data area where needed values can be located
	Common::fill(&buffer[BUFFER_SIZE], &buffer[BUFFER_SIZE + 16], 0);

	fExe.seek(0);
	fExe.read(buffer, BUFFER_SIZE);
	while (!fExe.eof()) {
		dataIndex = memScan(buffer, BUFFER_SIZE - 16, (const byte *)DataSegmentStr, 19);
		if (dataIndex >= 0)
			break;

		// Move the last 16 bytes and fill up the rest of the buffer with new data
		Common::copy(&buffer[BUFFER_SIZE], &buffer[BUFFER_SIZE + 16], &buffer[0]);
		fExe.read(&buffer[16], BUFFER_SIZE);
	}

	if (fExe.eof()) {
		printf("Failure occurred - could not locate data segment\n");
		return false;
	}

	// Found data segmetn start
	fExe.seek(-(BUFFER_SIZE + 16 - dataIndex) - 8, SEEK_CUR);
	dataStart = fExe.pos();

	// Presume the data segment will be the area up until the start of the first dynamic segment
	SegmentEntry &firstEntry = segmentList[0];
	dataSize = firstEntry.codeOffset - dataStart;

	// Figure out how much data segment relocations must be adjusted up by
	dataSegAdjust = (fExe.size() - (dataStart + dataSize)) >> 4;

	return true;
}

void listInfo() {
	printf("\nSegment list at offset %xh, size %xh:\n", segmentsOffset, segmentsSize);
	printf("\n\
Index   Exe Offset   Header offset   Code Offset,Size   # Relocs\n\
=====   ==========   =============   ================   ========\n");

	for (uint segmentCtr = 0; segmentCtr < segmentList.size(); ++segmentCtr) {
		SegmentEntry *se = &segmentList[segmentCtr];
		printf("%4d %11xh %14xh  %9xh, %4xh   %8d\n", se->segmentIndex, se->offset, se->headerOffset, 
			se->codeOffset, se->codeSize, se->relocations.size());
	}

	printf("\nJump table list at offset %xh:\n", jumpOffset);
	printf("\n\
Exe Offset    Segment Index    Segment Offset\n\
==========    =============    ==============\n");

	JumpEntryList::iterator ij;
	for (ij = jumpList.begin(); ij != jumpList.end(); ++ij) {
		JumpEntry &je = *ij;
		printf("%8xh %16d %14xh\n", je.offset, je.segmentIndex, je.segmentOffset);
	}
	printf("\n");
}

void processExecutable() {
	assert(relocOffset % 2 == 0);
	uint16 *header = new uint16[relocOffset / 2];

	// Read in the header data, process it, and write it out
	fExe.seek(0);
	for (int i = 0; i < relocOffset / 2; ++i) header[i] = fExe.readWord();

	// Set the filesize, taking into account extra space needed for extra relocation entries
	uint32 newcodeOffset = ((relocOffset + (relocations.size() + extraRelocations) * 4 + 511) / 512) * 512;
	uint32 newSize = newcodeOffset + (fExe.size() - codeOffset);
	header[1] = newSize % 512;
	header[2] = (newSize + 511) / 512;
	// Set the number of relocation entries
	header[3] = relocations.size() + extraRelocations;
	// Set the page offset for the code start
	header[4] = newcodeOffset / 16;
	// Make sure the file checksum is zero
	header[9] = 0;

	for (int i = 0; i < relocOffset / 2; ++i) fOut.writeWord(header[i]);
	delete header;

	// Copy over the existing relocation entries
	for (uint i = 0; i < relocations.size(); ++i) {
		if (dataFlag) {
			uint32 v = relocationAdjust(relocations[i]);
			fOut.writeLong(v);
			relocationOffsets.push_back(((v >> 16) << 4) + (v & 0xffff) + newcodeOffset);
		}
		else
			fOut.writeLong(relocations[i]);
	}

	// Loop through adding new relocation entries for each segment
	for (uint segmentCtr = 0; segmentCtr < segmentList.size(); ++segmentCtr) {
		SegmentEntry *segEntry = &segmentList[segmentCtr];

		for (uint i = 0; i < segEntry->relocations.size(); ++i) {
			if (dataFlag) {
				uint32 v = relocationAdjust(segEntry->relocations[i]);
				fOut.writeLong(v);
				relocationOffsets.push_back(((v >> 16) << 4) + (v & 0xffff) + newcodeOffset);
			} else
				fOut.writeLong(segEntry->relocations[i]);
		}
	}

	// Write out 0 bytes until the code start position
	int numBytes = newcodeOffset - fOut.pos();
	for (int i = 0; i < numBytes; ++i) fOut.writeByte(0);

	// Copy bytes until the start of the jump alias table
	fExe.seek(codeOffset);
	JumpEntry &firstEntry = *jumpList.begin();
	copyBytes(firstEntry.offset - fExe.pos());

	// Loop through handling the jump aliases - we'll be moving the FAR JMP over the FAR CALL, and
	// replacing the original FAR JMP with a set of NOPs

	JumpEntryList::iterator ij;
	for (ij = jumpList.begin(); ij != jumpList.end(); ++ij) {
		JumpEntry &je = *ij;

		// Write out null bytes between FAR JMPs
		int numBytes = je.offset - fExe.pos();
		assert(numBytes >= 0);
		if (numBytes > 0) {
			fExe.seek(numBytes, SEEK_CUR);
			while (numBytes-- > 0) fOut.writeByte(0);
		}

		// Get the data from the source
		fExe.seek(6, SEEK_CUR);
		uint16 offsetVal = fExe.readWord();
		uint32 segmentVal = fExe.readWord();		// Normally zero

		// Adjust the segment value appropriately
		if (je.segmentIndex != 0xffff) {
			// Get the entry for the specified segment index
			SegmentEntry *se = getSegment(je.segmentIndex);

			if (se != NULL) {
				uint32 segOffset = se->codeOffset - codeOffset;
				assert((segOffset % 16) == 0);

				segmentVal += (segOffset >> 4);
				if (je.segmentOffset != 0xffff)
					segmentVal += je.segmentOffset;
			}
		}

		// Write out the FAR JMP instruction
		fOut.writeByte(0xEA);
		fOut.writeWord(offsetVal);
		fOut.writeWord(segmentVal);

		// Write 5 padding bytes after the last entry
		for (int i = 0; i < 5; ++i) fOut.writeByte(0);
	}

	// Write out the data to the start of the data segment
	copyBytes(dataStart - fExe.pos());

	if (dataFlag) 
		// The data segment is being moved, so don't copy it over
		fExe.skip(dataSize);

	// Write out the rest of the file
	copyBytes(fExe.size() - fExe.pos());

	if (dataFlag) {
		fExe.seek(dataStart);
		copyBytes(dataSize);
	}

	// If data segment has been moved, adjust the segment offsets of all relocations
	for (uint i = 0; i < relocationOffsets.size(); ++i) {
		fOut.seek(relocationOffsets[i]);
		uint16 v = fOut.readWord();
		fOut.seek(-2, SEEK_CUR);
		fOut.writeWord(segmentAdjust(v));
	}

	fOut.seek(0, SEEK_END);
	printf("\nProcessing complete\n");
}

void close() {
	fExe.close();
	fOvl.close();
	fOut.close();
	exit(0);
}

int main(int argc, char *argv[]) {
	checkCommandLine(argc, argv);

	// Try to open the specified executable file
	if (!fExe.open(exeFilename)) {
		printf("The specified file could not be found\n");
		close();
	}
	if (!fOvl.open(ovlFilename)) {
		// Fall back on opening the exe file again, if no OVL file exists
		fOvl.open(exeFilename);
	}

	if (!validateExecutable())
		close();

	if (!loadSegmentList()) {
		close();
	}

	if (!loadJumpList()) {
		close();
	}

	if (dataFlag) {
		if (!loadDataDetails()) {
			close();
		}
	}

	if (infoFlag) 
		// Informational mode - list the RTLink data
		listInfo();

	if (processFlag) {
		// Processing mode - make a copy of the executable and update it
		if (!fOut.open(outputFilename, kFileWriteMode)) {
			printf("The specified output file '%s' could not be created", outputFilename);
			close();
		}

		processExecutable();

		fOut.close();
	}

	fExe.close();
	fOvl.close();
}
