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

RTLinkVersion rtlinkVersion;
File fExe, fOvl, fOut;
char exeFilename[MAX_FILENAME_SIZE];
char ovlFilename[MAX_FILENAME_SIZE];
char outputFilename[MAX_FILENAME_SIZE];
uint32 codeOffset, exeNameOffset;
uint32 rtlinkSegmentStart;
uint16 rtlinkSegment;

uint16 relocOffset, extraRelocations;
RelocationArray relocations; 
uint originalRelocationCount;

uint jumpOffset, segmentsOffset;
uint jumpSize, segmentsSize;
typedef Common::Array<JumpEntry> JumpEntryList;
JumpEntryList jumpList;
SegmentArray segmentList;

bool infoFlag = false;

uint outputCodeOffset, outputDataOffset;

/*--------------------------------------------------------------------------
* Support classes
*
*--------------------------------------------------------------------------*/

/**
 * Translates the relocation entry to a relative file offset
 */
uint RelocationEntry::relativeOffset() const {
	return ((_value >> 16) << 4) + (_value & 0xffff);
}

/**
 * Translates the relocation entry to a file offset
 */
uint RelocationEntry::fileOffset() const {
	return codeOffset + relativeOffset();
}

/**
 * Add an amount onto the segment portion of a relocation entry
 */
void RelocationEntry::addSegment(uint16 seg) {
	assert(getSegment() + seg <= 0xffff);
	_value += (uint)seg << 16;
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

static bool relocationSortHelper(const RelocationEntry &v1, const RelocationEntry &v2) {
	return v1.fileOffset() < v2.fileOffset();
}

void RelocationArray::sort() {
	Common::sort(begin(), end(), relocationSortHelper);
}

void RelocationArray::sortNew() {
	Common::sort(&(*this)[originalRelocationCount], end(), relocationSortHelper);
}

static bool segmentSortHelper(const SegmentEntry &v1, const SegmentEntry &v2) {
	return (!v1.isExecutable && v2.isExecutable) ||
		(v1.isExecutable == v2.isExecutable && v1.codeOffset < v2.codeOffset);
}

void SegmentArray::sort() {
	Common::sort(begin(), end(), segmentSortHelper);
}

SegmentEntry &SegmentArray::firstExeSegment() {
	uint lowestIndex = 0, lowestOffset = 0xfffffff;
	for (uint idx = 0; idx < size(); ++idx) {
		if ((*this)[idx].isExecutable && (*this)[idx].codeOffset < lowestOffset) {
			lowestIndex = idx;
			lowestOffset = (*this)[idx].codeOffset;
		}
	}
	
	return (*this)[lowestIndex];
}

SegmentEntry &SegmentArray::dataSegment() {
	SegmentEntry &lastSeg = (*this)[size() - 1];
	if (lastSeg.isDataSegment)
		return lastSeg;

	printf("Couldn't find data segment\n");
	exit(0);
}

/*--------------------------------------------------------------------------
 * Support functions
 *
 *--------------------------------------------------------------------------*/

/**
 * Scan a memory block for a given byte sequence of a given length
 */
int memScan(byte *data, int dataLen, const byte *s, int sLen) {
	for (int index = 0; index < dataLen - sLen - 1; ++index) {
		if (!strncmp((const char *)&data[index], (const char *)s, sLen))
			return index;
	}

	return -1;
}

/**
 * Scans the entire executable file for a given byte sequence and, if found,
 * returns the offset within the file
 */
int scanExecutable(const byte *data, int count) {
	byte buffer[BUFFER_SIZE];
	assert(count < BUFFER_SIZE);

	fExe.seek(0);
	for (;;) {
		int fileOffset = fExe.pos();
		fExe.read(buffer, BUFFER_SIZE);

		int dataIndex = memScan(buffer, BUFFER_SIZE - count, data, count);
		if (dataIndex >= 0) {
			int offset = fileOffset + dataIndex;
			fExe.seek(offset);
			return offset;
		}

		if (fExe.eof())
			break;

		// Move slightly backwards in the file before the next iteration and read,
		// just in case the sequence we're looking for falls across the buffer boundary
		fExe.seek(-count, SEEK_CUR);
	}

	return -1;
}

/**
 * Copies a specified number of bytes from the executable to the new 
 * file being created
 */
void copyBytes(int numBytes, File *f = &fExe) {
	byte buffer[BUFFER_SIZE];
	assert(numBytes >= 0);

	while (numBytes > BUFFER_SIZE) {
		f->read(buffer, BUFFER_SIZE);
		fOut.write(buffer, BUFFER_SIZE);
		numBytes -= BUFFER_SIZE;
	}

	if (numBytes > 0) {
		f->read(buffer, numBytes);
		fOut.write(buffer, numBytes);
	}
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
	int stringOffset = scanExecutable((const byte *)RTLinkStr, RTLINK_STR_SIZE);
	if (stringOffset == -1) {
		printf("RTLink(R)/Plus identifier not found in specified executable\n");
		return false;
	}
	printf("Found RTLink(R)/Plus identifier in executable\n");

	// Detect the version of RTLink in use
	
	// Version 3 is easily identifiable by the overall executable having no
	// relocation entries
	if (numRelocations == 0) {
		rtlinkVersion = VERSION3;
		printf("Version 3 - rtlinkst.com usage detected. This format not supported\n");
		return false;
	}

	// Version 2 has a longer string version of RTLink/Plus
	const char *V2_STRING = "RTLink(R)/Plus run-time code.\r\n";
	if (scanExecutable((const byte *)V2_STRING, strlen(V2_STRING)) != -1) {
		rtlinkVersion = VERSION2;
		printf("Version 2 of RTLink detected\n");
		return true;
	}

	rtlinkVersion = VERSION1;
	printf("Version 1 of RTLink presumed\n");
	return true;
}

/**
 * Loads the list of dynamic segments from version 1 executables. For these,
 * we find an occurance of the program's own filename, which is used by the
 * segment list, and work backwards to load in all the segments.
 */
bool loadSegmentListV1() {
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
		if (relocations[idx].fileOffset() > highestOffset) {
			highestIndex = idx;
			highestOffset = relocations[idx].fileOffset();
		}
	}

	rtlinkSegment = relocations[highestIndex].getSegment();
	rtlinkSegmentStart = codeOffset + rtlinkSegment * 16;

	return true;
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

/**
 * Load in the list of jump thunks that act as stubs for calling methods in dynamic segments
 */
bool loadJumpList() {
	byte byteVal;
	byte callByte;

	if (rtlinkVersion == VERSION1) {
		// After the filename (which is used by an earlier segment list), there may be 
		// another ASCII filename for an overlay file, and then after that the thunk list.
		// So if we get any kind of low value, then something's screwed up
		fExe.seek(exeNameOffset);
		callByte = 0xE8;

		// Scan forward to following start of thunk methods
		while ((byteVal = fExe.readByte()) != callByte) {
			if (byteVal > 0 && byteVal < 32) {
				printf("Couldn't resolve jump list\n");
				return false;
			}
		}
	} else {
		const char *EnterDirStr = "Enter directory for $";
		int fileOffset = scanExecutable((const byte *)EnterDirStr, strlen(EnterDirStr));
		if (fileOffset == -1) {
			printf("Couldn't resolve jump list\n");
			return false;
		}
		callByte = 0x9a;

		// Scan forward to following start of thunk methods
		while (!fExe.eof() && ((byteVal = fExe.readByte()) != 0x9A));
	}

	jumpOffset = fExe.pos() - 1;
	
	// Iterate through the list of method thunks
	while (!fExe.eof() && (byteVal == callByte)) {
		uint32 fileOffset = fExe.pos() - 1;

		// Skip over the ther operands for the thunk call that loads the dynamic segment
		fExe.skip((rtlinkVersion == VERSION1) ? 2 : 4);

		byte jmpByte = fExe.readByte();
		if (jmpByte != 0xea)
			// It's not a jmp statement, so reached end of list
			break;

		// Skip over offset within dynamic segments to jump to, and get the segment value
		uint16 offsetInSegment = fExe.readWord();
		uint16 segment = fExe.readWord();
		int segmentIndex;

		if (rtlinkVersion == VERSION2) {
			// For Rex Nebular at least, there can be some variation in the number
			// of padding bytes betwen method. And also there are some stub methods 
			// that don't seem to be called, and don't have a following segment index.
			// These may be methods that were optimized out of the final executable
			byteVal = fExe.readByte();

			if ((segment != 0) || (byteVal == 0x9a)) {
				segmentIndex = -1;
			} else {
				// The following bytes are an alias for segment translation
				segmentIndex = byteVal | (fExe.readByte() << 8);
				--segmentIndex;

				byteVal = fExe.readByte();
				byte byteVal2 = fExe.readByte();
				byte byteVal3 = fExe.readByte();

				if ((byteVal == 0x9a) && (byteVal3 != 0x9a)) {
					// In some rare cases, another jump follows after two bytes - dunno why
					segmentIndex = -1;
					fExe.seek(-2, SEEK_CUR);
				} else {
					// Get the offset and byte from following instruction
					segment = byteVal | (byteVal2 << 8);

					byteVal = byteVal3;
				}
			}
		} else {
			segmentIndex = fExe.readWord();
		}

		if (segmentIndex != -1) {
			assert((uint)segmentIndex < segmentList.size());
			SegmentEntry &segEntry = segmentList[segmentIndex];
			JumpEntry rec;

			rec.fileOffset = fileOffset;
			rec.segmentIndex = segmentIndex;
			rec.segmentOffset = (rtlinkVersion == VERSION2) ? segment : segment - segEntry.loadSegment;
			rec.offsetInSegment = offsetInSegment;

			jumpList.push_back(rec);
		}

		// If the next byte is 0, scan forward to see if the list resumes
		if (rtlinkVersion == VERSION1)
			byteVal = fExe.readByte();
		while (byteVal == 0)
			byteVal = fExe.readByte();
	}

	jumpSize = fExe.pos() - jumpOffset - 1;
	return true;
}

/**
 * Checks the details for the data segment. This is presumed to be the last
 * dynamic segment specified for the executable
 */
bool loadDataDetails() {
	int segmentExeCount = 0, segmentOvlCount = 0;

	// Iterate through the segments to find the last executable segment
	for (uint idx = 0; idx < segmentList.size(); ++idx) {
		SegmentEntry &se = segmentList[idx];

		// Keep track of how many are Exe vs Ovl segments
		if (se.isExecutable) {
			++segmentExeCount;
		} else {
			++segmentOvlCount;
		}
	}

	if (rtlinkVersion == VERSION1) {
		// Last RTLink segment is presumed to contain the data segment
		SegmentEntry &lastSeg = segmentList[segmentList.size() - 1];
		if (segmentOvlCount > 0 && segmentExeCount != 1) {
			printf("Warning.. multiple segments found in Exe. Presuming last is data segment(s)\n");
		}

		assert(lastSeg.isExecutable);
		lastSeg.isDataSegment = true;
	} else {
		// Version 2 RTLINK executables have the data segment at end of the
		// static part of the executable, before all the RTLink segments
		const char *MS_RUNTIME = "MS Run-Time";
		int fileOffset = scanExecutable((const byte *)MS_RUNTIME, strlen(MS_RUNTIME));
		if (fileOffset == -1) {
			printf("Could not locate data segment. Maybe not using MS Run-Time?\n");
			return false;
		}
		fileOffset -= 8;

		// Set up a new dummy segment in the segment list for the data segment
		SegmentEntry seg;
		seg.isExecutable = seg.isDataSegment = true;
		seg.headerOffset = seg.codeOffset = fileOffset;
		seg.codeSize = segmentList[0].headerOffset - fileOffset;
		seg.loadSegment = (fileOffset - codeOffset) / 16;

		segmentList.push_back(seg);
	}

	return true;
}

/**
 * In info mode, lists out some basic data for the segments and jump table
 */
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

/**
 * For processing mode, handles updating the existing entries as needed,as well
 * as adding in all the needed new entries
 */
void updateRelocationEntries() {
	// Firstly do an iteration of the original relocations, and delete any of them that
	// fall within the segment list. This is because some of them may point to different
	// points within the same area of memory allocated for loading them, so if we left
	// them in place, we could end up with confusion about how big each segment is
	for (int idx = (int)relocations.size() - 1; idx >= 0; --idx) {
		RelocationEntry &re = relocations[idx];
		if (re.fileOffset() >= segmentsOffset && re.fileOffset() < (segmentsOffset + segmentsSize))
			relocations.remove_at(idx);
	}

	originalRelocationCount = relocations.size();

	// For the data segment, we need to do a bit of pre-processing on the relocation 
	// entries.. some of the selectors pointed to are for segments outside the data 
	// segment, and into the area of memory rtlink segments are loaded into. 
	// As such, they can't really be mapped to a single specific segment in the decoded 
	// data, and what's worse, can screw up segment sizes when the decoded exe is 
	// disassembled. So scan for such entries and delete them now
	SegmentEntry &dataSeg = segmentList.dataSegment();
	for (int idx = (int)dataSeg.relocations.size() - 1; idx >= 0; --idx) {
		RelocationEntry &re = dataSeg.relocations[idx];
		
		// Figure out the file position the relocation entry points to within the
		// data segment, and read in the segment selector
		uint fileOffset = dataSeg.codeOffset + re.relativeOffset();
		fExe.seek(fileOffset);
		uint selector = fExe.readWord();

		if (selector < dataSeg.loadSegment && selector >= segmentList[0].loadSegment) {
			dataSeg.relocations.remove_at(idx);
			--extraRelocations;
		}
	}

	// Figure out the code start in the new executable, allowing enough room to put
	// in all the relocations that be copied out from the rtlink segments
	int totalRelocations = originalRelocationCount + extraRelocations;
	outputCodeOffset = (((relocOffset + totalRelocations * 4) + 511) / 512) * 512;
	if (outputCodeOffset < codeOffset)
		outputCodeOffset = codeOffset;

	// Get the entry for where the first RTLink segment in the executable started
	SegmentEntry &firstExeSeg = segmentList.firstExeSegment();

	// Start figuring out where each segment will be written to
	uint32 outputOffset = (outputCodeOffset - codeOffset) + firstExeSeg.headerOffset;

	// Iterate through each of the rtlink segments
	for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
		SegmentEntry &se = segmentList[segmentNum];

		// Set where each will go in the new EXE
		se.outputCodeOffset = outputOffset;
		outputOffset += se.codeSize;
		
		// Iterate through the dynamic relocation entries for the segment and add them in
		uint baseSegment = (se.outputCodeOffset - outputCodeOffset) >> 4;
		for (uint idx = 0; idx < se.relocations.size(); ++idx) {
			relocations.push_back(se.relocations[idx]);
			relocations[relocations.size() - 1].addSegment(baseSegment);
		}
	}

	// Process the original set of relocation entries. Any relocation entries
	// that were pointing into rtlink segments in the executable (i.e. data
	// segment references) will need to be adjusted by the change in position 
	// of the segments in the output file
	for (uint idx = 0; idx < originalRelocationCount; ++idx) {
		RelocationEntry &re = relocations[idx];
		
		for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
			SegmentEntry &se = segmentList[segmentNum];

			if (se.isExecutable && re.fileOffset() >= se.codeOffset
					&& re.fileOffset() < (se.codeOffset + se.codeSize)) {
				int oldSelectorDiff = re.getSegment() - se.loadSegment;
				assert(oldSelectorDiff > 0);
				int newSelector = (se.outputCodeOffset - outputCodeOffset) / 16 + oldSelectorDiff;

				re = RelocationEntry(newSelector, re.getOffset());
				break;
			}
		}
	}

	relocations.sortNew();
	assert(relocations.size() == totalRelocations);
}

void processExecutable() {
	uint segs[0xffff];
	for (uint i = 0; i < 0xffff; ++i) segs[i] = 0;
	for (uint idx = 0; idx < relocations.size(); ++idx) {
		RelocationEntry &re = relocations[idx];
		uint startSeg = re.getSegment();
		uint endSeg = re.getSegment() + (re.getOffset() / 16);

		for (uint s = startSeg; s < endSeg; ++s) {
			if (segs[s] == 0 || segs[s] == startSeg)
				segs[s] = startSeg;
			else {
				//printf("%x", segs[s]);
			}
		}
	}

	assert(relocOffset % 2 == 0);
	uint16 *header = new uint16[relocOffset / 2];

	// Read in the header data, process it, and write it out
	fExe.seek(0);
	for (int i = 0; i < relocOffset / 2; ++i) header[i] = fExe.readWord();

	// Set the filesize, taking into account extra space needed for extra relocation entries
	SegmentEntry &lastSeg = segmentList[segmentList.size() - 1];
	uint32 newSize = lastSeg.outputCodeOffset + lastSeg.codeSize;
	header[1] = newSize % 512;
	header[2] = (newSize + 511) / 512;
	// Set the number of relocation entries
	header[3] = relocations.size();
	// Set the page offset for the code start
	header[4] = outputCodeOffset / 16;
	// Make sure the file checksum is zero
	header[9] = 0;

	for (int i = 0; i < relocOffset / 2; ++i) fOut.writeWord(header[i]);
	delete header;

	// Copy over the relocation list
	for (uint i = 0; i < relocations.size(); ++i) {
		fOut.writeLong(relocations[i]);
	}

	// Write out 0 bytes until the code start position
	int numBytes = outputCodeOffset - fOut.pos();
	fOut.writeByte(0, numBytes);

	// Copy bytes until the start of the jump alias table
	fExe.seek(codeOffset);
	JumpEntry &firstEntry = jumpList[0];
	copyBytes(firstEntry.fileOffset - codeOffset);

	// Loop through handling the jump methods
	for (uint idx = 0; idx < jumpList.size(); ++idx) {
		JumpEntry &je = jumpList[idx];
		SegmentEntry &segEntry = segmentList[je.segmentIndex];
		uint newSelector = je.segmentOffset + (segEntry.outputCodeOffset - outputCodeOffset) / 16;

		// Copy over the call to the rtlink load method, and the opcode
		// byte for the following FAR JMP instruction
		copyBytes(4);

		// Write out the new JMP
		fOut.writeWord(je.offsetInSegment);
		fOut.writeWord(newSelector);
		fExe.skip(4);

		// Nop out the segment number at the end of the thunk method
		fOut.writeByte(0x90, 2);
		fExe.skip(2);
	}

	// Write out the data between the end of the thunk methods and the start of
	// the data for the first rtlink segment following it
	SegmentEntry &firstExeSeg = segmentList.firstExeSegment();
	SegmentEntry &dataSeg = segmentList.dataSegment();
	copyBytes(firstExeSeg.headerOffset - fExe.pos());

	// Iterate through writing the code for each rtlink segment in turn
	for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
		SegmentEntry &se = segmentList[segmentNum];

		// Write out the segment's data
		assert(fOut.pos() == se.outputCodeOffset);
		File *file = se.isExecutable ? &fExe : &fOvl;
		file->seek(se.codeOffset);
		copyBytes(se.codeSize, file);

		// iterate through the relocations for the segment and adjust the segment
		// values that the entries point to
		for (uint idx = 0; idx < relocations.size(); ++idx) {
			RelocationEntry &re = relocations[idx];
			if (re._segmentIndex != segmentNum)
				continue;

			uint fileOffset = (outputCodeOffset - codeOffset) + re.fileOffset();
			fOut.seek(fileOffset);
			uint selector = fOut.readWord();

			if (selector >= se.loadSegment && selector < (se.loadSegment + se.codeSize / 16)) {
				int selectorDiff = selector - se.loadSegment;
				int newSelector = (se.outputCodeOffset - outputCodeOffset) / 16 + selectorDiff;

				fOut.seek(-2, SEEK_CUR);
				fOut.writeWord(newSelector);
			} else if (selector >= dataSeg.loadSegment) {
				int selectorDiff = selector - dataSeg.loadSegment;
				int newSelector = (se.outputCodeOffset - outputCodeOffset) / 16 + selectorDiff;

				fOut.seek(-2, SEEK_CUR);
				fOut.writeWord(newSelector);
			}
		}

		// Move to the end of the output file, ready to write the next segment
		fOut.seek(0, SEEK_END);
	}

	// Do a final iteration across all the relocation entries from the original
	// executable. If any of them point directly into one of the original Exe
	// rtlink segments, then adjust them so they they're correctly relative to 
	// segment's new starting selector in the output file. This may sound weird,
	// but for Xanth at least, the data segment is part of a single RTlink segment
	// for the executable, and the selector the relocation entry points to is
	// already hardcoded for where the data is loaded. Since the segment is in a 
	// different place in the new Exe, we need to adjust for it
	for (uint idx = 0; idx < originalRelocationCount; ++idx) {
		RelocationEntry &re = relocations[idx];
		
		// If the relocation entry is for an entry in the method thunks, then we can
		// skip it, since it's already properly mapped to the correct dest segment
		if (re.fileOffset() >= jumpOffset && re.fileOffset() < (jumpOffset + jumpSize))
			continue;
		
		fOut.seek((outputCodeOffset - codeOffset) + re.fileOffset());
		uint selector = fOut.readWord();

		for (int segmentNum = segmentList.size() - 1; segmentNum >= 0; --segmentNum) {
			SegmentEntry &se = segmentList[segmentNum];

			// Check for mapping into segment. Note that, rarely, original segment references
			// may also point to unallocated data beyond the end of the file, so if the segment
			// is flagged as the data segment, don't bother checking against segment ending
			if (se.isExecutable && selector >= se.loadSegment && (se.isDataSegment ||
					selector < (se.loadSegment + se.codeSize / 16))) {
				// Adjust the selector
				int selectorDiff = selector - se.loadSegment;
				assert(selectorDiff >= 0);
				int newSelector = (se.outputCodeOffset - outputCodeOffset) / 16 + selectorDiff;
				fOut.seek(-2, SEEK_CUR);
				fOut.writeWord(newSelector);
				break;
			}
		}
	}

	//***DEBUG****
	for (uint idx = 0; idx < relocations.size(); ++idx) {
		RelocationEntry &re = relocations[idx];
		uint fileOffset = (outputCodeOffset - codeOffset) + re.fileOffset();

		fOut.seek(fileOffset);
		uint selector = fOut.readWord();

		if (segs[selector] != selector) {
			if (segs[selector] == 0) {
				segs[selector] = selector;
			}
			else {
//				printf("%x", segs[selector]); //**DEBUG**
			}
		}
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

	if (rtlinkVersion == VERSION1) {
		if (!loadSegmentListV1())
			close();
	} else {
		if (!loadSegmentListV2())
			close();
	}

	if (!loadJumpList())
		close();

	if (!loadDataDetails())
		close();

	if (infoFlag) {
		// Informational mode - list the RTLink data
		listInfo();

	} else {
		// Processing mode - make a copy of the executable and update it
		if (!fOut.open(outputFilename, kFileWriteMode)) {
			printf("The specified output file '%s' could not be created", outputFilename);
			close();
		}

		updateRelocationEntries();

		processExecutable();

		fOut.close();
	}

	fExe.close();
	fOvl.close();
}
