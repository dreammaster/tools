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
	uint16 segmentIndex;
	uint16 segmentOffset;
};

struct RelocationEntry {
	uint32 _value;

	RelocationEntry(uint32 v) : _value(v) {}

	RelocationEntry(uint16 seg, uint16 ofs) : _value(((uint32)seg << 16) + ofs) {}

	uint fileOffset() const;

	uint adjust() const;

	operator uint() const { return _value; }

	uint getOffset() const { return _value & 0xffff; }

	uint getSegment() const { return _value >> 16; }
};

class RelocationArray : public Common::Array<RelocationEntry> {
public:
	int indexOf(uint fileOffset) const;

	bool contains(uint fileOffset) const;

	void sort();
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
	int numRelocations;
	RelocationArray relocations;

	SegmentEntry() : offset(0), segmentIndex(0), filenameOffset(0), headerOffset(0),
		codeOffset(0), codeSize(0), flags(0), isExecutable(false) {}
};

class SegmentArray : public Common::Array<SegmentEntry> {
public:
	SegmentEntry *getSegment(int segmentIndex);

	SegmentEntry *firstEndingSegment() const;
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
