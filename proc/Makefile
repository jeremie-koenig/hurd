# Copyright (C) 1992, 1993, 1994, 1995, 1996 Free Software Foundation, Inc.
# This file is part of the GNU Hurd.
#
# The GNU Hurd is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# The GNU Hurd is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with the GNU Hurd; see the file COPYING.  If not, write to
# the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

dir := proc
makemode := server

target = proc
SRCS = wait.c hash.c host.c info.c main.c mgt.c	notify.c pgrp.c msg.c \
       cpu-types.c stubs.c
LCLHDRS = proc.h
DIST_FILES = proc_exc.defs ourmsg.defs

MIGSFLAGS="-DPROCESS_INTRAN=pstruct_t reqport_find (process_t)" \
	"-DPROCESS_DESTRUCTOR=process_drop (pstruct_t)" \
	"-DPROCESS_IMPORTS=import \"proc.h\";"

MIGSTUBS = processServer.o notifyServer.o \
	ourmsgUser.o proc_excUser.o proc_excServer.o
OBJS = $(SRCS:.c=.o) $(MIGSTUBS)
HURDLIBS=threads ihash ports shouldbeinlibc

include ../Makeconf

mutated_ourmsg_U.h: ourmsg_U.h
	sed -e 's/_msg_user_/_ourmsg_user_/' < $< > $@
