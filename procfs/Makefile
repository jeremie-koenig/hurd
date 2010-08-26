# Makefile for procfs
# 
#   Copyright (C) 2010 Free Software Foundation, Inc.
#
#   This program is free software; you can redistribute it and/or
#   modify it under the terms of the GNU General Public License as
#   published by the Free Software Foundation; either version 2, or (at
#   your option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#   General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

dir := procfs
makemode := server
target := procfs

SRCS = procfs.c netfs.c procfs_dir.c dircat.c \
	process.c proclist.c rootdir.c main.c
LOCALHDRS = procfs.h procfs_dir.h dircat.h \
	process.h proclist.h rootdir.h main.h
HURDLIBS = ps netfs fshelp iohelp ports threads ihash shouldbeinlibc

OBJS := $(SRCS:.c=.o)

include ../Makeconf
