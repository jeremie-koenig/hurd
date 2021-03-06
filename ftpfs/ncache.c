/* Node caching

   Copyright (C) 1997 Free Software Foundation, Inc.
   Written by Miles Bader <miles@gnu.ai.mit.edu>
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

#include <unistd.h>
#include <string.h>

#include <hurd/netfs.h>

#include "ftpfs.h"

/* Remove NN's node from its position in FS's node cache.  */
static void
node_unlink (struct node *node, struct ftpfs *fs)
{
  struct netnode *nn = node->nn;
  if (nn->ncache_next)
    nn->ncache_next->nn->ncache_prev = nn->ncache_prev;
  if (nn->ncache_prev)
    nn->ncache_prev->nn->ncache_next = nn->ncache_next;
  if (fs->node_cache_mru == node)
    fs->node_cache_mru = nn->ncache_next;
  if (fs->node_cache_lru == node)
    fs->node_cache_lru = nn->ncache_prev;
  nn->ncache_next = 0;
  nn->ncache_prev = 0;
  fs->node_cache_len--;
}

/* Add NODE to the recently-used-node cache, which adds a reference to
   prevent it from going away.  NODE should be locked.  */
void
ftpfs_cache_node (struct node *node)
{
  struct netnode *nn = node->nn;
  struct ftpfs *fs = nn->fs;

  mutex_lock (&fs->node_cache_lock);

  if (fs->params.node_cache_max > 0 || fs->node_cache_len > 0)
    {
      if (fs->node_cache_mru != node)
	{
	  if (nn->ncache_next || nn->ncache_prev)
	    /* Node is already in the cache.  */
	    node_unlink (node, fs);
	  else
	    /* Add a reference from the cache.  */
	    netfs_nref (node);

	  nn->ncache_next = fs->node_cache_mru;
	  nn->ncache_prev = 0;
	  if (fs->node_cache_mru)
	    fs->node_cache_mru->nn->ncache_prev = node;
	  if (! fs->node_cache_lru)
	    fs->node_cache_lru = node;
	  fs->node_cache_mru = node;
	  fs->node_cache_len++;
	}

      /* Forget the least used nodes.  */
      while (fs->node_cache_len > fs->params.node_cache_max)
	{
	  struct node *lru = fs->node_cache_lru;
	  node_unlink (lru, fs);
	  netfs_nrele (lru);
	}
    }

  mutex_unlock (&fs->node_cache_lock);
}
