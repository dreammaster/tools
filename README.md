# roguevm-tools
RogueVM tools repository

## rtlink_decode
Is a tool written to process games using the RTLink/Plus overlay manager and produce
a flat executable suitable for disassembling with tools such as IDA. It includes
a Makefile, but I've only really tested compiling it with Visual Studio.
The project requires an installation of ScummVM in order to use some of it's classes.

The tool currently detects and handles two different versions of RTLink/Plus..
one version that supports having an external overlay file containing segments,
and another where all the segments are inside the executable. The tool also can
detect, but not yet handle, a third form where an external rtlinkst.com file is
used.

Please remember that produced executables aren't intended to be runnable, and will
only be useful for simplifying disassembly. Also, if you do debug a game using
RTLink/Plus, any of the dynamic segments may shift in and out of memory at any
time, so generally breakpoints can only be placed in the low segments, or in
the thunk methods that are used to pass control to dynamic segments.
