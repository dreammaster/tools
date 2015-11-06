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

bool validateExecutableV3() {
	int fileOffset = scanExecutable((const byte *)"RTL", 3);
	if (fileOffset > 0x100) {
		printf("Possible version 3, but file missing data\n");
		return false;
	}

	// Load in the relocation list
	fExe.seek(((fExe.pos() + 3 + 15) / 16) * 16);
	uint relEntry;
	while ((relEntry = fExe.readLong()) != 0)
		relocations.push_back(RelocationEntry(relEntry));

	printf("Version 3 - rtlinkst.com usage detected.\n");

	return true;
}