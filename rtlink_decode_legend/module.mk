# $URL: https://www.switchlink.se/svn/m4/tools/rtlink_decode/module.mk $
# $Id: module.mk 763 2008-04-13 12:52:50Z dreammaster $

MODULE := tools/rtlink_decode

MODULE_OBJS := \
	rtlink_decode.o 

# Set the name of the executable
TOOL_EXECUTABLE := rtlink_decode

# Include common rules
include $(srcdir)/rules.mk
