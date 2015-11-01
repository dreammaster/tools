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
 * So far, I've discovered three different flavors of RTLink executables, so I'm
 * numbering them versions 1, 2, and 3
 */
enum RTLinkVersion { VERSION1, VERSION2, VERSION3 };

/*
 * Version 1: Example target: Companions of Xanth
 * Okay, here's how this version targets works:
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
 * segmentNum		word	An incrementing segment number. Not uesd for indexing
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

/*
 * Version 2: Example Target: Rex Nebular and the Cosmic Gender Bender
 * This version is similar to version 1, except for the following differences:
 * - The segment list is located differently, and has the following structure:
 * memorySegment	dword	Segment to load in memory. Only low 16-bits are used
 * ???	dword
 * fileOffset		dword	Offset in file for segment
 * unused			word
 * segmentNum		word	An increment segment number. Not uesd for indexing
 * unused			16bytes
 *
 * Note that in this version, there isn't any OVL file. All the data is in the
 * main executable. Also unlike version 1, the segment data pointed to in the
 * file isn't just relocations and code. Instead, it consists of a segment
 * header that itself contains the details of offset and size of the relocations
 * list as well as the code.
 *
 * This version also has slightly different thunk methods.. since they get located 
 * in their own separate segment, all the method calls to load the correct segment 
 * are far calls rather than version 1's near calls
 */

/*
 * Version 3: Example Target: Gateway
 * This version of RTLink uses a somewhat more complicated arrangement, with a
 * separate rtlinkst.com and .RTL file. I've still got to investigate this
 * version in more detail, but the executable does at least contain a segment list
 * like version 1. So I presume it also has similar thunk methods; I just haven't
 * gotten around to identifying them. This version does some weird shuffling
 * around in memory of the low-end code at the start of the executable.
 * Probably saving space by dumping core runtime startup code after it's executed,
 * and re-using the freed space.
 */


#ifndef __RTLINK_DECODE_H__
#define __RTLINK_DECODE_H__

#include <stdio.h>
#include <stdlib.h>
#include "common/scummsys.h"
#include "common/endian.h"
#include "common/array.h"

#undef FILE
#undef fopen
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef feof
#undef fclose

enum AccessMode {
	kFileReadMode = 1,
	kFileWriteMode = 2
};

struct JumpEntry {
	uint32 fileOffset;
	int segmentIndex;
	uint16 segmentOffset;
	uint16 offsetInSegment;
};

struct RelocationEntry {
	uint32 _value;
	int _segmentIndex;

	RelocationEntry(uint32 v) : _value(v), _segmentIndex(-1) {}

	RelocationEntry(uint16 seg, uint16 ofs) : _value(((uint32)seg << 16) + ofs),
		_segmentIndex(-1) {}

	void addSegment(uint16 seg);

	uint relativeOffset() const;

	uint fileOffset() const;

	operator uint() const { return _value; }

	uint getOffset() const { return _value & 0xffff; }

	uint getSegment() const { return _value >> 16; }
};

class RelocationArray : public Common::Array<RelocationEntry> {
public:
	int indexOf(uint fileOffset) const;

	bool contains(uint fileOffset) const;

	void sort();

	void sortNew();
};

class SegmentEntry {
public:
	uint32 offset;
	uint segmentIndex;
	uint filenameOffset;
	uint loadSegment;

	uint32 headerOffset;
	uint32 codeOffset;
	uint32 codeSize;
	byte flags;
	bool isExecutable;
	bool isDataSegment;
	int numRelocations;
	RelocationArray relocations;
	uint32 outputCodeOffset;

	SegmentEntry() : offset(0), segmentIndex(0), filenameOffset(0), headerOffset(0),
		codeOffset(0), codeSize(0), flags(0), isExecutable(false), isDataSegment(false) {}
};

class SegmentArray : public Common::Array<SegmentEntry> {
public:
	SegmentEntry &firstExeSegment();

	SegmentEntry &dataSegment();

	void sort();
};

class File {
private:
	FILE *f;
public:
	bool open(const char *filename, AccessMode mode = kFileReadMode) {
		f = fopen(filename, (mode == kFileReadMode) ? "rb" : "wb+");
		return (f != NULL);
	}
	void close() {
		if (f)
			fclose(f);
		f = NULL;
	}
	int seek(int32 offset, int whence = SEEK_SET) {
		return fseek(f, offset, whence);
	}
	void skip(int32 offset) {
		fseek(f, offset, SEEK_CUR);
	}
	long read(void *buffer, int len) {
		return fread(buffer, 1, len, f);
	}
	void write(const void *buffer, int len) {
		fwrite(buffer, 1, len, f);
	}
	byte readByte() {
		byte v;
		read(&v, sizeof(byte));
		return v;
	}
	uint16 readWord() {
		uint16 v;
		read(&v, sizeof(uint16));
		return FROM_LE_16(v);
	}
	uint32 readLong() {
		uint32 v;
		read(&v, sizeof(uint32));
		return FROM_LE_32(v);
	}
	void writeByte(byte v) {
		write(&v, sizeof(byte));
	}
	void writeByte(byte v, int len) {
		byte *b = new byte[len];
		memset(b, v, len);
		write(b, len);
		delete[] b;
	}
	void writeWord(uint16 v) {
		uint16 vTemp = TO_LE_16(v);
		write(&vTemp, sizeof(uint16));
	}
	void writeLong(uint32 v) {
		uint32 vTemp = TO_LE_32(v);
		write(&vTemp, sizeof(uint32));
	}
	uint32 pos() const {
		return ftell(f);
	}
	uint32 size() const {
		if (f) {
			uint32 currentPos = pos();
			fseek(f, 0, SEEK_END);
			uint32 result = pos();
			fseek(f, currentPos, SEEK_SET);
			return result;
		} else {
			return 0;
		}
	}
	bool eof() const { return feof(f) != 0; }
};

#define MAX_FILENAME_SIZE 1024
#define BUFFER_SIZE 1024
#define LARGE_BUFFER_SIZE 0x1000

#endif
