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
uint32 codeStart, rtlinkStrStart, segmentsStart, jumpsStart;

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

#define SEG_OFS_TO_OFFSET(seg,ofs) (codeStart + (seg << 4) + ofs)
#define SEGOFS_TO_OFFSET(segofs) (codeStart + ((segofs >> 16) << 4) + (segofs & 0xffff))

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
	uint32 offset = ((uint32)segVal << 4) + ofsVal + codeStart;

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
	if (v < ((dataStart - codeStart) >> 4))
		// Segment points to base code areas
		return v;
	else if (v < ((dataStart + dataSize - codeStart) >> 4))
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
	int gameId = 0;

	// Loop through parameters
	for (int idx = 1; idx < argc; ++idx) {
		if (!strcmp(argv[idx], "xanth"))
			gameId = 1;
		else if (!strcmp(argv[idx], "/list"))
			infoFlag = true;
		else
			error("Unknown option - %s", argv[idx]);
	}

	if (argc == 1) {
		printf("RTLink(R)/Plus Legend Entertainment executable decoder -- Version 1.0\n\n\
			   Usage: rtlink_decode xanth [/list | /process]\n");
		exit(0);
	} else if (gameId == 1) {
		// Xanth
		strcpy(exeFilename, "xanth.exe");
		strcpy(ovlFilename, "xanth.ovl");
		rtlinkStrStart = 0x28BC4;
		segmentsStart = 0x25F49;
		jumpsStart = 0x266C9;

	} else {
		error("No valid game specified");
	}
}

// validateExecutable
// Validates that the specified file is an RTLink executable

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
	codeStart = fExe.readWord() << 4;
	fExe.seek(24);
	relocOffset = fExe.readWord();

	// Get the relocation list
	fExe.seek(relocOffset);
	for (int i = 0; i < numRelocations; ++i) 
		relocations.push_back(fExe.readLong());

	// Check for the RTLink string
	fExe.seek(rtlinkStrStart);
	char buffer[RTLINK_STR_SIZE];
	fExe.read(buffer, RTLINK_STR_SIZE);

	if (!strncmp(buffer, RTLinkStr, RTLINK_STR_SIZE)) {
		printf("Found RTLink(R)/Plus identifier in executable\n");
		return true;
	}

	printf("RTLink(R)/Plus identifier not found in specified executable\n");
	return false;
}

/**
 * Load in the list of jump thunks that handle ensuring a particular segment is loaded,
 * and then jump to a particular routine in the segment once loaded
 */
bool loadJumpList() {
	fExe.seek(jumpsStart);
	byte byteVal = 0xE8;
	int index;

	jumpOffset = fExe.pos() - 1;
	uint16 aliasSegment = 0;

	while (!fExe.eof() && (byteVal == 0xE8)) {
		uint32 fileOffset = fExe.pos() - 1;

		// Skip over the thunk call that loads the dynamic segment
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
		jumpList.push_back(JumpEntryList::value_type(rec));

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

// loadSegmentList
// Loads the list of dynamic segments from the executable

const byte DataFragment[] = {0x02, 0x40, 0x00, 0x00, 0x04, 0x00, 0x04, 0xff, 0xff, 0x00, 0x00};

bool loadSegmentList() {
	SegmentEntry *rec, *prevRec;
	byte buffer[BUFFER_SIZE + 16];
	int dataIndex = 0;

	// Scan for the data area where needed values can be located
	Common::fill(&buffer[BUFFER_SIZE], &buffer[BUFFER_SIZE + 16], 0);

	fExe.seek(0);
	fExe.read(buffer, BUFFER_SIZE);
	while (!fExe.eof()) {
		dataIndex = memScan(buffer, BUFFER_SIZE - 16, DataFragment, 11);
		if (dataIndex >= 0)
			break;

		// Move the last 16 bytes and fill up the rest of the buffer with new data
		Common::copy(&buffer[BUFFER_SIZE], &buffer[BUFFER_SIZE + 16], &buffer[0]);
		fExe.read(&buffer[16], BUFFER_SIZE);
	}

	if (fExe.eof()) {
		printf("Failure occurred - could not locate segment table\n");
		return false;
	}

	fExe.seek(-(BUFFER_SIZE + 16 - dataIndex) + 11 + 24, SEEK_CUR);
	uint32 segmentsSeg = fExe.readWord();

	// Go and get the data

	// Move to the segments position, skipping past the 30h byte header
	segmentsOffset = codeStart + (segmentsSeg << 4);
	fExe.seek(segmentsOffset + 0x30);

	rec = NULL;
	int segmentIndex = 0;
	for (;;) {
		// Check next entry
		uint32 filePos = fExe.pos();
		fExe.seek(8, SEEK_CUR);
		uint32 headerOffset = fExe.readLong();
		fExe.readWord();
		int newIndex = fExe.readWord() - 1;
		fExe.seek(16, SEEK_CUR);
		uint32 nextPos = fExe.pos();

		bool breakFlag = ++segmentIndex != newIndex;
		if (segmentList.size() > 0) {
			// If another sequental segment isn't found, break out of loop
			SegmentEntry &last = segmentList[segmentList.size() - 1];
			if (breakFlag) {
				last.codeSize = fExe.size() - last.codeOffset;
				break;
			}

			// Set the code size for the previous segment
			last.codeSize = headerOffset - last.codeOffset;
		}

		// Create the next entry and add it to the list
		SegmentEntry rec;
		rec.offset = filePos;
		rec.headerOffset = headerOffset;
		rec.segmentIndex = segmentIndex;

		segmentList.push_back(rec);
	}
	
	// Rescan through the list of segments to calculate the code offsets and relocation entries

	extraRelocations = 0;
	prevRec = NULL;
	segmentIndex = 0;
	for (uint segmentCtr = 0; segmentCtr < segmentList.size(); ++segmentCtr) {
		rec = &segmentList[segmentCtr];

		// Set the previous record's code size
		if (prevRec != nullptr)
			prevRec->codeSize = rec->headerOffset - prevRec->codeOffset;

		// Move to the specified offset and read the segment header information
		fExe.seek(rec->headerOffset + 2);
		uint16 codeParagraphOffset = fExe.readWord();
		fExe.readWord();
		uint16 relocationStart = fExe.readWord();
		uint16 numRelocations = fExe.readWord();
		assert(relocationStart == 0);

		// Set the code offset
		rec->codeOffset = rec->headerOffset + (codeParagraphOffset << 4);

		// Get the list of relocations
		fExe.seek(6 + relocationStart * 4, SEEK_CUR);
		for (int relCtr = 0; relCtr < numRelocations; ++relCtr, ++extraRelocations) {
			uint16 offsetVal = fExe.readWord();
			uint16 segmentVal = fExe.readWord();
			assert((offsetVal != 0) || (segmentVal != 0));
			assert((uint32)(segmentVal * 16 + offsetVal) <= rec->codeSize);

			uint32 v = (segmentVal << 16) + offsetVal;
			rec->relocations.push_back(v);
		}

		// Process the relocation list to add the relative segment start of this segment's code
		for (uint i = 0; i < rec->relocations.size(); ++i) {
			rec->relocations[i] += ((rec->codeOffset - codeStart) >> 4) << 16;
		}
		Common::sort(rec->relocations.begin(), rec->relocations.end());
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
	uint32 newCodeStart = ((relocOffset + (relocations.size() + extraRelocations) * 4 + 511) / 512) * 512;
	uint32 newSize = newCodeStart + (fExe.size() - codeStart);
	header[1] = newSize % 512;
	header[2] = (newSize + 511) / 512;
	// Set the number of relocation entries
	header[3] = relocations.size() + extraRelocations;
	// Set the page offset for the code start
	header[4] = newCodeStart / 16;
	// Make sure the file checksum is zero
	header[9] = 0;

	for (int i = 0; i < relocOffset / 2; ++i) fOut.writeWord(header[i]);
	delete header;

	// Copy over the existing relocation entries
	for (uint i = 0; i < relocations.size(); ++i) {
		if (dataFlag) {
			uint32 v = relocationAdjust(relocations[i]);
			fOut.writeLong(v);
			relocationOffsets.push_back(((v >> 16) << 4) + (v & 0xffff) + newCodeStart);
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
				relocationOffsets.push_back(((v >> 16) << 4) + (v & 0xffff) + newCodeStart);
			} else
				fOut.writeLong(segEntry->relocations[i]);
		}
	}

	// Write out 0 bytes until the code start position
	int numBytes = newCodeStart - fOut.pos();
	for (int i = 0; i < numBytes; ++i) fOut.writeByte(0);

	// Copy bytes until the start of the jump alias table
	fExe.seek(codeStart);
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
				uint32 segOffset = se->codeOffset - codeStart;
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

int main(int argc, char *argv[]) {
	checkCommandLine(argc, argv);

	// Try to open the specified executable file
	if (!fExe.open(exeFilename) || !fOvl.open(ovlFilename)) {
		printf("The specified file could not be found\n");
		exit(0);
	}

	if (!validateExecutable()) {
		fExe.close();
		exit(0);
	}

	if (!loadJumpList()) {
		fExe.close();
		exit(0);
	}

	if (!loadSegmentList()) {
		fExe.close();
		exit(0);
	}

	if (dataFlag) {
		if (!loadDataDetails()) {
			fExe.close();
			exit(0);
		}
	}

	if (infoFlag) 
		// Informational mode - list the RTLink data
		listInfo();

	if (processFlag) {
		// Processing mode - make a copy of the executable and update it
		if (!fOut.open(outputFilename, kFileWriteMode)) {
			fExe.close();
			printf("The specified output file '%s' could not be created", outputFilename);
			exit(0);
		}

		processExecutable();

		fOut.close();
	}

	fExe.close();
	fOvl.close();
}
