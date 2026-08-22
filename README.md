# roguevm-tools
RogueVM tools repository

## rtlink_decode
Is a tool written to process games using the RTLink/Plus overlay manager and produce
a flat executable suitable for disassembling with tools such as IDA. It includes
a Makefile, but I've only really tested compiling it with Visual Studio.

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

Some far calls/jmps target a memory slot with several mutually-exclusive
alternate segments (only one ever resident at a time, picked by game state at
runtime), and can't be resolved to a single target at decode time. Rather than
guess wrong, the tool leaves those words untouched and lists their file offsets
in a companion `<output>.polymorphic.txt` report next to the decoded exe.
`rtlink_decode/ida_scripts/apply_polymorphic_comments.py` reads that report and
drops a comment at each flagged location in an IDA database, so they're visibly
flagged rather than silently wrong -- it's generic across any RTLink-decoded
executable, not tied to a specific game.
