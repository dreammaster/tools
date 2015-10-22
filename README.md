# roguevm-tools
RogueVM tools repository

## rtlink_decode_mads and rtlink_decode_legend
Tools written to process games using the RTLink/Plus overlay manager and produce
a flat executable suitable for disassembling with tools such as IDA. They include
Makefiles, but I've only really tested compiling them with Visual Studio.
The project requires the presence of a scummvm/ folder in the same directory that
you clone the roguevm-tools repository into.

The MADS version of the tool tries to be somewhat generic, but it does make
a few assumptions that might need to be updated for other games, such as that the
function thunks to call methods in a dynamic segment will each start with a far
call to the segment loader routine. Whereas in the Legend tool the segment loading
call was a near call. Also, the MADS version tries to dynamically locate all the
needed data in the executable, so it's the better version to start with before 
making any needed changes if it cant handle a particular game. The Legend tool is a 
bit more quick and dirty, and has the offsets for various tables hardcoded.
