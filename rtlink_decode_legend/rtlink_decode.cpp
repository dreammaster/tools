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

uint32 dataSegmentOffset, dataSize;

uint32 jumpOffset, segmentsOffset, segmentsSize;
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
 * Translates the relocation entry to a file offset
 */
uint RelocationEntry::fileOffset() const {
	return codeOffset + ((_value >> 16) << 4) + (_value & 0xffff);
}

/**
 * Add an amount onto the segment portion of a relocation entry
 */
void RelocationEntry::addSegment(uint16 seg) {
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
		if (se.filenameOffset && se.isExecutable && se.headerOffset > dataSegmentOffset && se.headerOffset < offset) {
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

/**
 * Copies a specified number of bytes from the executable to the new 
 * file being created
 */
void copyBytes(int numBytes, File *f = &fExe) {
	byte buffer[BUFFER_SIZE];

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
			seg.relocations.push_back(RelocationEntry(segmentVal - seg.loadSegment, offsetVal));
		}

		// Sort the list of relocations into relative order
		seg.relocations.sort();
	}

	// Finally, move backwards from the start of the segments list to find the first matching
	// entry in the executable's relocations list. This will tell us the RTLink segment,
	// which we'll need later to create new relocation offsets for all the thunks
	int relocIndex;
	while ((relocIndex = relocations.indexOf(firstSegmentOffset)) == -1) {
		--firstSegmentOffset;
		assert(firstSegmentOffset >= 0);
	}
	rtlinkSegment = relocations[relocIndex].getSegment();
	rtlinkSegmentStart = codeOffset + rtlinkSegment * 16;

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
		uint16 offsetInSegment = fExe.readWord();
		uint16 segment = fExe.readWord();
		int segmentIndex = fExe.readWord();

		SegmentEntry &segEntry = segmentList[segmentIndex];

		JumpEntry rec;
		rec.fileOffset = fileOffset;
		rec.segmentIndex = segmentIndex;
		rec.segmentOffset = segment - segEntry.loadSegment;
		rec.offsetInSegment = offsetInSegment;
		jumpList.push_back(rec);

		byteVal = fExe.readByte();
	}

	return true;
}

/**
 * Loads the start and size of the data segment
 */
bool loadDataDetails() {
	const char *DataSegmentStr = "MS Run-Time Library";
	int fileOffset = scanExecutable((const byte *)DataSegmentStr, strlen(DataSegmentStr));
	if (fileOffset == -1) {
		printf("Failure occurred - could not locate data segment\n");
		return false;
	}

	assert((fileOffset % 8) == 0);
	dataSegmentOffset = fileOffset - 8;

	// Find the earliest dynamic segment, if any, that follows the data segment. 
	// This will be used as the end of the data segment
	SegmentEntry *firstEntry = segmentList.firstEndingSegment();
	if (firstEntry) {
		dataSize = firstEntry->headerOffset - dataSegmentOffset;
	} else {
		dataSize = fExe.size() - dataSegmentOffset;
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
	// Figure out the code start in the new executable, allowing enough room to put
	// in all the relocations that be copied out from the rtlink segments
	int originalCount = relocations.size();
	int totalRelocations = originalCount + extraRelocations;
	outputCodeOffset = (((relocOffset + totalRelocations * 4) + 511) / 512) * 512;
	if (outputCodeOffset < codeOffset)
		outputCodeOffset = codeOffset;

	// The rtlink segments will be moved to where the data segment was in the original
	// executable, which will then follow them at the end of the file
	uint32 outputOffset = (outputCodeOffset - codeOffset) + dataSegmentOffset;

	// Iterate through each of the rtlink segments
	for (uint segmentNum = 0; segmentNum < segmentList.size(); ++segmentNum) {
		SegmentEntry &se = segmentList[segmentNum];

		if (se.isExecutable && se.codeOffset < dataSegmentOffset) {
			// It's a segment before the data segment in the executable. In which
			// case use the original offset, simply adjusted by any resizing of
			// the relocation table at the start of the executable
			se.outputCodeOffset = (outputCodeOffset - codeOffset) + se.codeOffset;
		} else {
			// It's a segment to be relocated, so use the current output offset
			se.outputCodeOffset = outputOffset;
			outputOffset += se.codeSize;
		}
		
		// Iterate through the dynamic relocation entries for the segment and add them in
		uint baseSegment = (se.outputCodeOffset - outputCodeOffset) >> 4;
		for (uint idx = 0; idx < se.relocations.size(); ++idx) {
			relocations.push_back(se.relocations[idx]);
			relocations[relocations.size() - 1].addSegment(baseSegment);
		}
	}

	// The data segment goes at the end of the new file after all these segments
	outputDataOffset = outputOffset;

	// Process the original set of relocation entries. Any relocation entries
	// that were pointing into the data segment will need to be adjusted to 
	// point to the new location
	for (int idx = 0; idx < originalCount; ++idx) {
		RelocationEntry &re = relocations[idx];
		if (re.fileOffset() > dataSegmentOffset) {
			re.addSegment((outputDataOffset - dataSegmentOffset) >> 4);
		}
	}

	Common::sort(relocations.begin(), relocations.end(), relocationSortHelper);
	assert(relocations.size() == totalRelocations);
}

void processExecutable() {
	assert(relocOffset % 2 == 0);
	uint16 *header = new uint16[relocOffset / 2];

	// Read in the header data, process it, and write it out
	fExe.seek(0);
	for (int i = 0; i < relocOffset / 2; ++i) header[i] = fExe.readWord();

	// Set the filesize, taking into account extra space needed for extra relocation entries
	uint32 newSize = outputDataOffset + dataSize;
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

		// Copy over the call to the rtlink load method, and the opcode
		// byte for the following FAR JMP instruction
		copyBytes(4);

		// Write out the new JMP
		fOut.writeWord(je.offsetInSegment);
		fOut.writeWord(je.segmentOffset + (segEntry.outputCodeOffset - outputCodeOffset) / 16);
		fExe.skip(4);

		// Nop out the segment number at the end of the thunk method
		fOut.writeByte(0x90, 2);
		fExe.skip(2);
	}

	// Write out the data to the start of the data segment
	copyBytes(dataSegmentOffset - fExe.pos());

	// Iterate through writing the code for each rtlink segment in turn
	for (uint idx = 0; idx < segmentList.size(); ++idx) {
		SegmentEntry &se = segmentList[idx];

		// If the dynamic segment was already present in the executable earlier
		// than the data segment, it's already been written out, and can be ignored
		if (se.isExecutable && se.codeOffset < dataSegmentOffset)
			continue;

		// Write out the dynamic segment
		assert(fOut.pos() == se.outputCodeOffset);
		File *file = se.isExecutable ? &fExe : &fOvl;
		file->seek(se.codeOffset);
		copyBytes(se.codeSize, file);
	}

	// Finally, write out the data segment
	fExe.seek(dataSegmentOffset);
	copyBytes(dataSize);

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
