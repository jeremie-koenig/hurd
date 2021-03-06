/*
   Copyright (C) 1994,2002 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "priv.h"

kern_return_t
trivfs_S_file_getcontrol (struct trivfs_protid *cred,
			  mach_port_t reply, mach_msg_type_name_t reply_type,
			  mach_port_t *cntl, mach_msg_type_name_t *cntltype)
{
  if (!cred)
    return EOPNOTSUPP;
  if (!cred->isroot)
    return EPERM;

  *cntl = ports_get_right (cred->po->cntl);
  *cntltype = MACH_MSG_TYPE_MAKE_SEND;
  return 0;
}
