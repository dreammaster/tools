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

/**
 * Okay, here's how the version of RTLink that this version targets works:
 * One of the segments early in the executable will be a "RTLink segment".
 * It contains three areas that are of interest to us:
 * 1) A list of dynamic segments that the program includes.
 * 2) An intermediate area containing the filenames of the files dynamic segments
 * come from. I'm currently only familiar with there being the name of the EXE itself,
 * as well as an optional OVL filename.
 * 3) The list of "thunks" stub methods which are used to call methods in dynamic segments.
 *
 * Segment List
 * ============
 * The segment list is a set of segment records, each 18 bytes long. Each consist of:
 * memorySegment	word	The segment in memory to load the segment to
 * filename			word	The offset within the same segment for the filename
 *							specifying the file containing the segment (EXE or OVL)
 * fileOffset		3 bytes	The offset within the file in paragraphs (ie multiply by 16)
 * flags			1 byte
 * unknown1			word
 * numRelocations	word	Specifies the number of relocation entries at the start of
 *		the segment in the file; these specify the offsets within the segment where
 *		segment values that need to be adjusted to the loaded position in memory
 * unknown2			word
 * segmentNum		word	An incrementing segment number
 * numParagraphs	word	The number of paragraphs (size * 16) of the code block
 *		following the relocation list for the segment in the file
 *
 * Thunk List
 * ==========
 * The thunk list is a set of stub methods. Each of them consist of the following:
 * A call instruction to a method that handles loading the correct dynamic segment.
 *		This version expects the call to be to a near method. The other version
 *		of the tool, rtlink_decode_mads, handles a version that uses far calls
 * A far jump to the correct offset within the loaded dynamic segment. The segment
 *		specified may not necessarily be the same as the loaded dynamic segment,
 *		since the loaded rtlink segment may contain several sub-segments
 * A word containing the rtlink segment to load. This is used by the rtlink segment
 *		loader, to know which segment it is meant to load
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
uint32 rtlinkSegmentStart;
uint16 rtlinkSegment;

uint16 relocOffset, extraRelocations;
RelocationArray relocations; 
Common::Array<uint32> relocationOffsets; 

uint32 dataStart, dataSize;
uint16 dataSegAdjust;

uint32 jumpOffset, segmentsOffset, segmentsSize;
typedef Common::List<JumpEntry> JumpEntryList;
JumpEntryList jumpList;
SegmentArray segmentList;

bool infoFlag = false, processFlag = false;

/*--------------------------------------------------------------------------
* Support classes
*
*--------------------------------------------------------------------------*/

/**
 * Translates the relocation entry to a file offset
 */
uint RelocationEntry::fileOffset() const {
	return codeOffset + ((_value >> 16) << 4) + (_value & 0xffff);
}

uint RelocationEntry::adjust() const {
	uint16 segVal = getSegment();
	uint16 ofsVal = getOffset();
	uint32 offset = fileOffset();

	uint16 segmentVal = segVal;
	if (offset > dataStart + dataSize)
		// Relocation entry within dynamic code, adjust it backwards
		segmentVal = segVal - (dataSize >> 4);
	else if (offset > dataStart)
		// Adjustment of data segment relocation entry to end of file
		segmentVal = segVal + dataSegAdjust;

	return ((uint32)segmentVal << 16) | ofsVal;
}

/**
 * Return the index of a relocation entry matching the given file offset
 */
int RelocationArray::indexOf(uint fileOffset) const {
	for (uint i = 0; i < relocations.size(); ++i) {
		if ((*this)[i].fileOffset() == fileOffset)
			return i;
	}

	return -1;
}

/**
 * Returns true if the relocation array contains an entry for the given file offset
 */
bool RelocationArray::contains(uint fileOffset) const {
	return indexOf(fileOffset) != -1;
}

/**
 * Return the segment with the specified index
 */
SegmentEntry *SegmentArray::getSegment(int segmentIndex) {
	for (uint idx = 0; idx < segmentList.size(); ++idx) {
		SegmentEntry &se = segmentList[idx];
		if (se.segmentIndex == segmentIndex)
			return &se;
	}

	return nullptr;
}

/**
 * Returns a reference to the first rtlink segment entry that occurs
 * after the data segment in the executable, if there any any
 */
SegmentEntry *SegmentArray::firstEndingSegment() const {
	// First find the earliest segment value
	uint offset = (uint)-1;
	SegmentEntry *segmentEntry = nullptr;
	for (uint idx = 0; idx < size(); ++idx) {
		SegmentEntry &se = segmentList[idx];
		if (se.filenameOffset && se.isExecutable && se.headerOffset > dataStart && se.headerOffset < offset) {
			offset = se.headerOffset;
			segmentEntry = &se;
		}
	}

	return segmentEntry;
}

static bool relocationSortHelper(const RelocationEntry &v1, const RelocationEntry &v2) {
	return v1.fileOffset() < v2.fileOffset();
}

void RelocationArray::sort() {
	Common::sort(begin(), end(), relocationSortHelper);
}

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
	} else {
		strcat(exeFilename, ".EXE");
		strcat(ovlFilename, ".OVL");
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
	uint firstSegmentOffset = 0;
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

		firstSegmentOffset = seg.offset;

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

			seg.relocations.push_back(RelocationEntry(segmentVal, offsetVal));
		}

		// Sort the list of relocations into relative order
		seg.relocations.sort();
	}

	// Finally, move backwards from the start of the segments list to find the first matching
	// entry in the executable's relocations list. This will tell us the RTLink segment,
	// which we'll need later to create new relocation offsets for all the thunks
	int relocIndex;
	rtlinkSegmentStart = firstSegmentOffset;
	while ((relocIndex = relocations.indexOf(rtlinkSegmentStart)) == -1)
		--rtlinkSegmentStart;
	rtlinkSegment = relocations[relocIndex].getSegment();

	return true;
}

/**
 * Load in the list of jump thunks that act as stubs for calling methods in dynamic segments
 */
bool loadJumpList() {
	byte byteVal;

	// After the filename (which is used by an earlier segment list), there may be 
	// another ASCII filename for an overlay file, and then after that the thunk list.
	// So if we get any kind of low value, then something's screwed up
	fExe.seek(exeNameOffset);
	while ((byteVal = fExe.readByte()) != 0xE8) {
		if (byteVal > 0 && byteVal < 32) {
			printf("Couldn't resolve jump list\n");
			return false;
		} else if (byteVal == 0xea) {
			printf("RTLINK thunks in this game are using FAR calls. Try using rtlink_decode_mads\n");
			return false;
		}
	}
	jumpOffset = fExe.pos() - 1;
	
	// Iterate through the list of method thunks
	while (!fExe.eof() && (byteVal == 0xE8)) {
		uint32 fileOffset = fExe.pos() - 1;

		// Skip over the offset for the thunk call that loads the dynamic segment
		fExe.readWord();

		byte jmpByte = fExe.readByte();
		if (jmpByte != 0xea)
			// It's not a jmp statement, so reached end of list
			break;

		// Skip over offset within dynamic segments to jump to, and get the segment value
		fExe.readWord();
		uint16 segment = fExe.readWord();
		int segmentIndex = fExe.readWord();

		SegmentEntry &segEntry = *segmentList.getSegment(segmentIndex);

		JumpEntry rec;
		rec.fileOffset = fileOffset;
		rec.segmentIndex = segmentIndex;
		rec.segmentOffset = segment - segEntry.loadSegment;
		jumpList.push_back(rec);

		byteVal = fExe.readByte();
	}

	return true;
}

/**
* Loads the start and size of the data segment
*/
const char *DataSegmentStr = "MS Run-Time Library";

bool loadDataDetails() {
	int fileOffset = scanExecutable((const byte *)DataSegmentStr, strlen(DataSegmentStr));
	if (fileOffset == -1) {
		printf("Failure occurred - could not locate data segment\n");
		return false;
	}

	assert((fileOffset % 8) == 0);
	dataStart = fileOffset - 8;

	// Find the earliest dynamic segment, if any, that follows the data segment. 
	// This will be used as the end of the data segment
	SegmentEntry *firstEntry = segmentList.firstEndingSegment();
	if (firstEntry) {
		dataSize = firstEntry->headerOffset - dataStart;
	} else {
		dataSize = fExe.size() - dataStart;
	}

	// Figure out how much data segment relocations must be adjusted up by,
	// since we need to move it to the end of the file after the dynamic segments
	dataSegAdjust = (fExe.size() + fOvl.size() - (dataStart + dataSize)) >> 4;

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
		printf("%8xh %16d %14xh\n", je.fileOffset, je.segmentIndex, je.segmentOffset);
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

	// TODO: Figure out relocations for the thunks
	exit(0);//***DEBUG***
	/*
	// Create a new relocation entry for the replacement FAR JMP instruction we'll be creating.
	// Since it's encoded as E8 |offset word| |segment word|, set the 
	index = relocations.indexOf(fileOffset + 3);
	if (index == -1) {
		uint rtlinkSegOffset = fileOffset + 3 - rtlinkSegment;
		//			relocations.push_back()				
		//				(aliasSegment << 16) + (fileOffset + 3 - (aliasSegment << 4)));
		printf("Added entry for file offset %xh", fileOffset + 3,
			relocations[relocations.size() - 1]);
	}
	*/

	// Copy over the existing relocation entries
	for (uint i = 0; i < relocations.size(); ++i) {
		uint32 v = relocations[i].adjust();
		fOut.writeLong(v);
		relocationOffsets.push_back(((v >> 16) << 4) + (v & 0xffff) + newcodeOffset);
	}

	// Loop through adding new relocation entries for each segment
	for (uint segmentCtr = 0; segmentCtr < segmentList.size(); ++segmentCtr) {
		SegmentEntry *segEntry = &segmentList[segmentCtr];

		for (uint i = 0; i < segEntry->relocations.size(); ++i) {
			uint32 v = segEntry->relocations[i].adjust();
			fOut.writeLong(v);
			relocationOffsets.push_back(((v >> 16) << 4) + (v & 0xffff) + newcodeOffset);
		}
	}

	// Write out 0 bytes until the code start position
	int numBytes = newcodeOffset - fOut.pos();
	for (int i = 0; i < numBytes; ++i) fOut.writeByte(0);

	// Copy bytes until the start of the jump alias table
	fExe.seek(codeOffset);
	JumpEntry &firstEntry = *jumpList.begin();
	copyBytes(firstEntry.fileOffset - fExe.pos());

	// Loop through handling the jump aliases - we'll be moving the FAR JMP over the FAR CALL, and
	// replacing the original FAR JMP with a set of NOPs

	JumpEntryList::iterator ij;
	for (ij = jumpList.begin(); ij != jumpList.end(); ++ij) {
		JumpEntry &je = *ij;

		// Write out null bytes between FAR JMPs
		int numBytes = je.fileOffset - fExe.pos();
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
			SegmentEntry *se = segmentList.getSegment(je.segmentIndex);

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

	// The data segment is being moved, so don't copy it over
	fExe.skip(dataSize);

	// Write out the rest of the file
	copyBytes(fExe.size() - fExe.pos());

	// Write out the OVL file, if present


	// Write out the data segment
	fExe.seek(dataStart);
	copyBytes(dataSize);

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
	fOvl.open(ovlFilename);

	if (!validateExecutable())
		close();

	if (!loadSegmentList())
		close();

	if (!loadJumpList())
		close();

	if (!loadDataDetails())
		close();

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
