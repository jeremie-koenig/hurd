/* vcons-refresh.c - Redraw a virtual console.
   Copyright (C) 2002 Free Software Foundation, Inc.
   Written by Marcus Brinkmann.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <errno.h>
#include <assert.h>

#include "cons.h"

/* Redraw the virtual console VCONS, which is locked.  */
void
cons_vcons_refresh (vcons_t vcons)
{
  vcons->state.screen.cur_line = vcons->display->screen.cur_line;
  vcons->state.screen.scr_lines = vcons->display->screen.scr_lines;
  vcons->state.cursor.col = vcons->display->cursor.col;
  vcons->state.cursor.row = vcons->display->cursor.row;
  vcons->state.cursor.status = vcons->display->cursor.status;
  vcons->state.bell.audible = vcons->display->bell.audible;
  vcons->state.bell.visible = vcons->display->bell.visible;
  vcons->state.flags = vcons->display->flags;
  vcons->state.changes.written = vcons->display->changes.written;

  cons_vcons_write (vcons, vcons->state.screen.matrix
		    + (vcons->state.screen.cur_line % vcons->state.screen.lines)
		    * vcons->state.screen.width,
		    ((vcons->state.screen.lines
		      - (vcons->state.screen.cur_line % vcons->state.screen.lines)
		      < vcons->state.screen.height)
		     ? vcons->state.screen.lines
		     - (vcons->state.screen.cur_line % vcons->state.screen.lines)
		     : vcons->state.screen.height)
		    * vcons->state.screen.width, 0, 0);
  if (vcons->state.screen.lines
      - (vcons->state.screen.cur_line % vcons->state.screen.lines)
      < vcons->state.screen.height)
    cons_vcons_write (vcons, vcons->state.screen.matrix,
		      vcons->state.screen.height * vcons->state.screen.width
		      - (vcons->state.screen.lines
			 - (vcons->state.screen.cur_line % vcons->state.screen.lines))
		      * vcons->state.screen.width, 0,
		      vcons->state.screen.lines
		      - (vcons->state.screen.cur_line % vcons->state.screen.lines));

  cons_vcons_set_cursor_pos (vcons, vcons->state.cursor.col,
			     vcons->state.cursor.row);
  cons_vcons_set_cursor_status (vcons, vcons->state.cursor.status);
  cons_vcons_set_scroll_lock (vcons, vcons->state.flags & CONS_FLAGS_SCROLL_LOCK);
  cons_vcons_update (vcons);
}
