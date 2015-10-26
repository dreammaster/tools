# roguevm-tools
RogueVM tools repository

## rtlink_decode_mads and rtlink_decode_legend
Tools written to process games using the RTLink/Plus overlay manager and produce
a flat executable suitable for disassembling with tools such as IDA. They include
Makefiles, but I've only really tested compiling them with Visual Studio.
The project requires an installation of ScummVM in order to use some of it's classes.

The versions of RTLink/Plus that each tool handles is slightly different;
the names simply reflect the particular game series each was tool was first written
to target, but they should be generic enough that at least one of the two should
be able to handle other games using RTLink.

Please remember that produced executables aren't intended to be runnable, and will
only be useful for simplifying disassembly. Also, if you do debug a game using
RTLink/Plus, any of the dynamic segments may shift in and out of memory at any
time, so generally breakpoints can only be placed in the low segments, or in
the thunk methods that are used to pass control to dynamic segments.
