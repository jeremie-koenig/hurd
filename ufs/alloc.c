/* Disk allocation routines
   Copyright (C) 1993,94,95,96,98,2002 Free Software Foundation, Inc.

This file is part of the GNU Hurd.

The GNU Hurd is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

The GNU Hurd is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with the GNU Hurd; see the file COPYING.  If not, write to
the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* Modified from UCB by Michael I. Bushnell.  */
/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ffs_alloc.c	8.8 (Berkeley) 2/21/94
 */

#include "ufs.h"
#include <stdio.h>
#include <string.h>


/* These don't work *at all* here; don't even try setting them. */
#undef DIAGNOSTIC
#undef QUOTA

extern u_long nextgennumber;

spin_lock_t alloclock = SPIN_LOCK_INITIALIZER;

/* Forward declarations */
static u_long ffs_hashalloc (struct node *, int, long, int,
			     u_long (*)(struct node *, int, daddr_t, int));
static u_long ffs_alloccg (struct node *, int, daddr_t, int);
static daddr_t ffs_fragextend (struct node *, int, long, int, int);
static ino_t ffs_dirpref (struct fs *);
static u_long ffs_nodealloccg (struct node *, int, daddr_t, int);
static daddr_t ffs_alloccgblk (struct fs *, struct cg *, daddr_t);
static daddr_t ffs_mapsearch (struct fs *, struct cg *, daddr_t, int);
static void ffs_clusteracct (struct fs *, struct cg *, daddr_t, int);

/* Sync all allocation information and nod eNP if diskfs_synchronous. */
inline void
alloc_sync (struct node *np)
{
  if (diskfs_synchronous)
    {
      if (np)
	diskfs_node_update (np, 1);
      copy_sblock ();
      diskfs_set_hypermetadata (1, 0);
      sync_disk (1);
    }
}

/* Byteswap everything in CGP. */
void
swab_cg (struct cg *cg)
{
  int i, j;

  if (swab_long (cg->cg_magic) == CG_MAGIC
      || cg->cg_magic == CG_MAGIC)
    {
      cg->cg_magic = swab_long (cg->cg_magic);
      cg->cg_time = swab_long (cg->cg_time);
      cg->cg_cgx = swab_long (cg->cg_cgx);
      cg->cg_ncyl = swab_short (cg->cg_ncyl);
      cg->cg_niblk = swab_short (cg->cg_niblk);
      cg->cg_cs.cs_ndir = swab_long (cg->cg_cs.cs_ndir);
      cg->cg_cs.cs_nbfree = swab_long (cg->cg_cs.cs_nbfree);
      cg->cg_cs.cs_nifree = swab_long (cg->cg_cs.cs_nifree);
      cg->cg_cs.cs_nffree = swab_long (cg->cg_cs.cs_nffree);
      cg->cg_rotor = swab_long (cg->cg_rotor);
      cg->cg_irotor = swab_long (cg->cg_irotor);
      for (i = 0; i < MAXFRAG; i++)
	cg->cg_frsum[i] = swab_long (cg->cg_frsum[i]);
      cg->cg_btotoff = swab_long (cg->cg_btotoff);
      cg->cg_boff = swab_long (cg->cg_boff);
      cg->cg_iusedoff = swab_long (cg->cg_iusedoff);
      cg->cg_freeoff = swab_long (cg->cg_freeoff);
      cg->cg_nextfreeoff = swab_long (cg->cg_nextfreeoff);
      cg->cg_clustersumoff = swab_long (cg->cg_clustersumoff);
      cg->cg_clusteroff = swab_long (cg->cg_clusteroff);
      cg->cg_nclusterblks = swab_long (cg->cg_nclusterblks);

      /* blktot map */
      for (i = 0; i < cg->cg_ncyl; i++)
	cg_blktot(cg)[i] = swab_long (cg_blktot(cg)[i]);

      /* blks map */
      for (i = 0; i < cg->cg_ncyl; i++)
	for (j = 0; j < sblock->fs_nrpos; j++)
	  cg_blks(sblock, cg, i)[j] = swab_short (cg_blks (sblock, cg, i)[j]);

      for (i = 0; i < sblock->fs_contigsumsize; i++)
	cg_clustersum(cg)[i] = swab_long (cg_clustersum(cg)[i]);

      /* inosused, blksfree, and cg_clustersfree are char arrays */
    }
  else
    {
      /* Old format cylinder group... */
      struct ocg *ocg = (struct ocg *) cg;

      if (swab_long (ocg->cg_magic) != CG_MAGIC
	  && ocg->cg_magic != CG_MAGIC)
	return;

      ocg->cg_time = swab_long (ocg->cg_time);
      ocg->cg_cgx = swab_long (ocg->cg_cgx);
      ocg->cg_ncyl = swab_short (ocg->cg_ncyl);
      ocg->cg_niblk = swab_short (ocg->cg_niblk);
      ocg->cg_ndblk = swab_long (ocg->cg_ndblk);
      ocg->cg_cs.cs_ndir = swab_long (ocg->cg_cs.cs_ndir);
      ocg->cg_cs.cs_nbfree = swab_long (ocg->cg_cs.cs_nbfree);
      ocg->cg_cs.cs_nifree = swab_long (ocg->cg_cs.cs_nifree);
      ocg->cg_cs.cs_nffree = swab_long (ocg->cg_cs.cs_nffree);
      ocg->cg_rotor = swab_long (ocg->cg_rotor);
      ocg->cg_frotor = swab_long (ocg->cg_frotor);
      ocg->cg_irotor = swab_long (ocg->cg_irotor);
      for (i = 0; i < 8; i++)
	ocg->cg_frsum[i] = swab_long (ocg->cg_frsum[i]);
      for (i = 0; i < 32; i++)
	ocg->cg_btot[i] = swab_long (ocg->cg_btot[i]);
      for (i = 0; i < 32; i++)
	for (j = 0; j < 8; j++)
	  ocg->cg_b[i][j] = swab_short (ocg->cg_b[i][j]);
      ocg->cg_magic = swab_long (ocg->cg_magic);
    }
}


/* Read cylinder group indexed CG.  Set *CGPP to point at it.
   Return 1 if caller should call release_cgp when we're done with it;
   otherwise zero. */
int
read_cg (int cg, struct cg **cgpp)
{
  struct cg *diskcg = cg_locate (cg);

  if (swab_disk)
    {
      *cgpp = malloc (sblock->fs_cgsize);
      bcopy (diskcg, *cgpp, sblock->fs_cgsize);
      swab_cg (*cgpp);
      return 1;
    }
  else
    {
      *cgpp = diskcg;
      return 0;
    }
}

/* Caller of read_cg is done with cg; write it back to disk (swapping it
   along the way) and free the memory allocated in read_cg. */
void
release_cg (struct cg *cgp)
{
  int cgx = cgp->cg_cgx;
  swab_cg (cgp);
  bcopy (cgp, cg_locate (cgx), sblock->fs_cgsize);
  free (cgp);
}


/*
 * Allocate a block in the file system.
 *
 * The size of the requested block is given, which must be some
 * multiple of fs_fsize and <= fs_bsize.
 * A preference may be optionally specified. If a preference is given
 * the following hierarchy is used to allocate a block:
 *   1) allocate the requested block.
 *   2) allocate a rotationally optimal block in the same cylinder.
 *   3) allocate a block in the same cylinder group.
 *   4) quadradically rehash into other cylinder groups, until an
 *      available block is located.
 * If no block preference is given the following heirarchy is used
 * to allocate a block:
 *   1) allocate a block in the cylinder group that contains the
 *      inode for the file.
 *   2) quadradically rehash into other cylinder groups, until an
 *      available block is located.
 */
error_t
ffs_alloc(register struct node *np,
	  daddr_t lbn,
	  daddr_t bpref,
	  int size,
	  daddr_t *bnp,
	  struct protid *cred)
{
	register struct fs *fs;
	daddr_t bno;
	int cg;

	*bnp = 0;
	fs = sblock;
#ifdef DIAGNOSTIC
	if ((u_int)size > fs->fs_bsize || fragoff(fs, size) != 0) {
		printf("dev = 0x%x, bsize = %d, size = %d, fs = %s\n",
		    ip->i_dev, fs->fs_bsize, size, fs->fs_fsmnt);
		panic("ffs_alloc: bad size");
	}
	assert (cred);
#endif /* DIAGNOSTIC */
	spin_lock (&alloclock);
	if (size == fs->fs_bsize && fs->fs_cstotal.cs_nbfree == 0)
		goto nospace;
	if (cred && !idvec_contains (cred->user->uids, 0)
	    && freespace(fs, fs->fs_minfree) <= 0)
		goto nospace;
#ifdef QUOTA
	if (error = chkdq(ip, (long)btodb(size), cred, 0))
		return (error);
#endif
	if (bpref >= fs->fs_size)
		bpref = 0;
	if (bpref == 0)
		cg = ino_to_cg(fs, np->dn->number);
	else
		cg = dtog(fs, bpref);
	bno = (daddr_t)ffs_hashalloc(np, cg, (long)bpref, size,
	    (u_long (*)())ffs_alloccg);
	if (bno > 0) {
		spin_unlock (&alloclock);
		np->dn_stat.st_blocks += btodb(size);
		np->dn_set_ctime = 1;
		np->dn_set_mtime = 1;
		*bnp = bno;
		alloc_sync (np);
		return (0);
	}
#ifdef QUOTA
	/*
	 * Restore user's disk quota because allocation failed.
	 */
	(void) chkdq(ip, (long)-btodb(size), cred, FORCE);
#endif
nospace:
	spin_unlock (&alloclock);
	printf ("file system full");
/*	ffs_fserr(fs, cred->cr_uid, "file system full"); */
/* 	uprintf("\n%s: write failed, file system is full\n", fs->fs_fsmnt); */
	return (ENOSPC);
}

/*
 * Reallocate a fragment to a bigger size
 *
 * The number and size of the old block is given, and a preference
 * and new size is also specified. The allocator attempts to extend
 * the original block. Failing that, the regular block allocator is
 * invoked to get an appropriate block.
 */
error_t
ffs_realloccg(register struct node *np,
	      daddr_t lbprev,
	      volatile daddr_t bpref,
	      int osize,
	      int nsize,
	      daddr_t *pbn,
	      struct protid *cred)
{
	register struct fs *fs;
	int cg, error;
	volatile int request;
	daddr_t bprev, bno;

	*pbn = 0;
	fs = sblock;
#ifdef DIAGNOSTIC
	if ((u_int)osize > fs->fs_bsize || fragoff(fs, osize) != 0 ||
	    (u_int)nsize > fs->fs_bsize || fragoff(fs, nsize) != 0) {
		printf(
		    "dev = 0x%x, bsize = %d, osize = %d, nsize = %d, fs = %s\n",
		    ip->i_dev, fs->fs_bsize, osize, nsize, fs->fs_fsmnt);
		panic("ffs_realloccg: bad size");
	}
	if (cred == NOCRED)
		panic("ffs_realloccg: missing credential\n");
#endif /* DIAGNOSTIC */

	spin_lock (&alloclock);

	if (!idvec_contains (cred->user->uids, 0)
	    && freespace(fs, fs->fs_minfree) <= 0)
		goto nospace;
	error = diskfs_catch_exception ();
	if (error)
	  return error;
	bprev = read_disk_entry ((dino (np->dn->number))->di_db[lbprev]);
	diskfs_end_catch_exception ();
	assert ("old block not allocated" && bprev);

#if 0 /* Not needed in GNU Hurd ufs */
	/*
	 * Allocate the extra space in the buffer.
	 */
	if (error = bread(ITOV(ip), lbprev, osize, NOCRED, &bp)) {
		brelse(bp);
		return (error);
	}
#ifdef QUOTA
	if (error = chkdq(ip, (long)btodb(nsize - osize), cred, 0)) {
		brelse(bp);
		return (error);
	}
#endif
#endif /* 0 */

	/*
	 * Check for extension in the existing location.
	 */
	cg = dtog(fs, bprev);
	bno = ffs_fragextend(np, cg, (long)bprev, osize, nsize);
	if (bno) {
		assert (bno == bprev);
		spin_unlock (&alloclock);
		np->dn_stat.st_blocks += btodb(nsize - osize);
		np->dn_set_ctime = 1;
		np->dn_set_mtime = 1;
		*pbn = bno;
#if 0 /* Not done this way in GNU Hurd ufs. */
		allocbuf(bp, nsize);
		bp->b_flags |= B_DONE;
		bzero((char *)bp->b_data + osize, (u_int)nsize - osize);
		*bpp = bp;
#endif
		alloc_sync (np);
		return (0);
	}
	/*
	 * Allocate a new disk location.
	 */
	if (bpref >= fs->fs_size)
		bpref = 0;
	switch ((int)fs->fs_optim) {
	case FS_OPTSPACE:
		/*
		 * Allocate an exact sized fragment. Although this makes
		 * best use of space, we will waste time relocating it if
		 * the file continues to grow. If the fragmentation is
		 * less than half of the minimum free reserve, we choose
		 * to begin optimizing for time.
		 */
		request = nsize;
		if (fs->fs_minfree < 5 ||
		    fs->fs_cstotal.cs_nffree >
		    fs->fs_dsize * fs->fs_minfree / (2 * 100))
			break;
		printf ("%s: optimization changed from SPACE to TIME\n",
			fs->fs_fsmnt);
		fs->fs_optim = FS_OPTTIME;
		break;
	case FS_OPTTIME:
		/*
		 * At this point we have discovered a file that is trying to
		 * grow a small fragment to a larger fragment. To save time,
		 * we allocate a full sized block, then free the unused portion.
		 * If the file continues to grow, the `ffs_fragextend' call
		 * above will be able to grow it in place without further
		 * copying. If aberrant programs cause disk fragmentation to
		 * grow within 2% of the free reserve, we choose to begin
		 * optimizing for space.
		 */
		request = fs->fs_bsize;
		if (fs->fs_cstotal.cs_nffree <
		    fs->fs_dsize * (fs->fs_minfree - 2) / 100)
			break;
		printf ("%s: optimization changed from TIME to SPACE\n",
			fs->fs_fsmnt);
		fs->fs_optim = FS_OPTSPACE;
		break;
	default:
		assert (0);
		/* NOTREACHED */
	}
	bno = (daddr_t)ffs_hashalloc(np, cg, (long)bpref, request,
	    (u_long (*)())ffs_alloccg);
	if (bno > 0) {
#if 0 /* Not necessary in GNU Hurd ufs */
		bp->b_blkno = fsbtodb(fs, bno);
		(void) vnode_pager_uncache(ITOV(ip));
#endif
/* Commented out here for Hurd; we don't want to free this until we've
   saved the old contents.  Callers are responsible for freeing the
   block when they are done with it. */
/*		ffs_blkfree(np, bprev, (long)osize); */
		if (nsize < request)
			ffs_blkfree(np, bno + numfrags(fs, nsize),
			    (long)(request - nsize));
		spin_unlock (&alloclock);
		np->dn_stat.st_blocks += btodb(nsize - osize);
		np->dn_set_mtime = 1;
		np->dn_set_ctime = 1;
		*pbn = bno;
#if 0 /* Not done this way in GNU Hurd ufs */
		allocbuf(bp, nsize);
		bp->b_flags |= B_DONE;
		bzero((char *)bp->b_data + osize, (u_int)nsize - osize);
		*bpp = bp;
#endif /* 0 */
		alloc_sync (np);
		return (0);
	}
#ifdef QUOTA
	/*
	 * Restore user's disk quota because allocation failed.
	 */
	(void) chkdq(ip, (long)-btodb(nsize - osize), cred, FORCE);
#endif
#if 0 /* Not necesarry in GNU Hurd ufs */
	brelse(bp);
#endif
nospace:
	/*
	 * no space available
	 */
	spin_unlock (&alloclock);
	printf ("file system full");
/* 	ffs_fserr(fs, cred->cr_uid, "file system full"); */
/* 	uprintf("\n%s: write failed, file system is full\n", fs->fs_fsmnt); */
	return (ENOSPC);
}

#if 0 /* Not used (yet?) in GNU Hurd ufs */
/*
 * Reallocate a sequence of blocks into a contiguous sequence of blocks.
 *
 * The vnode and an array of buffer pointers for a range of sequential
 * logical blocks to be made contiguous is given. The allocator attempts
 * to find a range of sequential blocks starting as close as possible to
 * an fs_rotdelay offset from the end of the allocation for the logical
 * block immediately preceeding the current range. If successful, the
 * physical block numbers in the buffer pointers and in the inode are
 * changed to reflect the new allocation. If unsuccessful, the allocation
 * is left unchanged. The success in doing the reallocation is returned.
 * Note that the error return is not reflected back to the user. Rather
 * the previous block allocation will be used.
 */
#include <sys/sysctl.h>
int doasyncfree = 1;
struct ctldebug debug14 = { "doasyncfree", &doasyncfree };
int
ffs_reallocblks(ap)
	struct vop_reallocblks_args /* {
		struct vnode *a_vp;
		struct cluster_save *a_buflist;
	} */ *ap;
{
	struct fs *fs;
	struct inode *ip;
	struct vnode *vp;
	struct buf *sbp, *ebp;
	daddr_t *bap, *sbap, *ebap;
	struct cluster_save *buflist;
	daddr_t start_lbn, end_lbn, soff, eoff, newblk, blkno;
	struct indir start_ap[NIADDR + 1], end_ap[NIADDR + 1], *idp;
	int i, len, start_lvl, end_lvl, pref, ssize;

	vp = ap->a_vp;
	ip = VTOI(vp);
	fs = ip->i_fs;
	if (fs->fs_contigsumsize <= 0)
		return (ENOSPC);
	buflist = ap->a_buflist;
	len = buflist->bs_nchildren;
	start_lbn = buflist->bs_children[0]->b_lblkno;
	end_lbn = start_lbn + len - 1;
#ifdef DIAGNOSTIC
	for (i = 1; i < len; i++)
		if (buflist->bs_children[i]->b_lblkno != start_lbn + i)
			panic("ffs_reallocblks: non-cluster");
#endif
	/*
	 * If the latest allocation is in a new cylinder group, assume that
	 * the filesystem has decided to move and do not force it back to
	 * the previous cylinder group.
	 */
	if (dtog(fs, dbtofsb(fs, buflist->bs_children[0]->b_blkno)) !=
	    dtog(fs, dbtofsb(fs, buflist->bs_children[len - 1]->b_blkno)))
		return (ENOSPC);
	if (ufs_getlbns(vp, start_lbn, start_ap, &start_lvl) ||
	    ufs_getlbns(vp, end_lbn, end_ap, &end_lvl))
		return (ENOSPC);
	/*
	 * Get the starting offset and block map for the first block.
	 */
	if (start_lvl == 0) {
		sbap = &ip->i_db[0];
		soff = start_lbn;
	} else {
		idp = &start_ap[start_lvl - 1];
		if (bread(vp, idp->in_lbn, (int)fs->fs_bsize, NOCRED, &sbp)) {
			brelse(sbp);
			return (ENOSPC);
		}
		sbap = (daddr_t *)sbp->b_data;
		soff = idp->in_off;
	}
	/*
	 * Find the preferred location for the cluster.
	 */
	pref = ffs_blkpref(ip, start_lbn, soff, sbap);
	/*
	 * If the block range spans two block maps, get the second map.
	 */
	if (end_lvl == 0 || (idp = &end_ap[end_lvl - 1])->in_off + 1 >= len) {
		ssize = len;
	} else {
#ifdef DIAGNOSTIC
		if (start_ap[start_lvl-1].in_lbn == idp->in_lbn)
			panic("ffs_reallocblk: start == end");
#endif
		ssize = len - (idp->in_off + 1);
		if (bread(vp, idp->in_lbn, (int)fs->fs_bsize, NOCRED, &ebp))
			goto fail;
		ebap = (daddr_t *)ebp->b_data;
	}
	/*
	 * Search the block map looking for an allocation of the desired size.
	 */
	if ((newblk = (daddr_t)ffs_hashalloc(ip, dtog(fs, pref), (long)pref,
	    len, (u_long (*)())ffs_clusteralloc)) == 0)
		goto fail;
	/*
	 * We have found a new contiguous block.
	 *
	 * First we have to replace the old block pointers with the new
	 * block pointers in the inode and indirect blocks associated
	 * with the file.
	 */
	blkno = newblk;
	for (bap = &sbap[soff], i = 0; i < len; i++, blkno += fs->fs_frag) {
		if (i == ssize)
			bap = ebap;
#ifdef DIAGNOSTIC
		if (buflist->bs_children[i]->b_blkno != fsbtodb(fs, *bap))
			panic("ffs_reallocblks: alloc mismatch");
#endif
		*bap++ = blkno;
	}
	/*
	 * Next we must write out the modified inode and indirect blocks.
	 * For strict correctness, the writes should be synchronous since
	 * the old block values may have been written to disk. In practise
	 * they are almost never written, but if we are concerned about
	 * strict correctness, the `doasyncfree' flag should be set to zero.
	 *
	 * The test on `doasyncfree' should be changed to test a flag
	 * that shows whether the associated buffers and inodes have
	 * been written. The flag should be set when the cluster is
	 * started and cleared whenever the buffer or inode is flushed.
	 * We can then check below to see if it is set, and do the
	 * synchronous write only when it has been cleared.
	 */
	if (sbap != &ip->i_db[0]) {
		if (doasyncfree)
			bdwrite(sbp);
		else
			bwrite(sbp);
	} else {
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		if (!doasyncfree)
			VOP_UPDATE(vp, &time, &time, MNT_WAIT);
	}
	if (ssize < len)
		if (doasyncfree)
			bdwrite(ebp);
		else
			bwrite(ebp);
	/*
	 * Last, free the old blocks and assign the new blocks to the buffers.
	 */
	for (blkno = newblk, i = 0; i < len; i++, blkno += fs->fs_frag) {
		ffs_blkfree(ip, dbtofsb(fs, buflist->bs_children[i]->b_blkno),
		    fs->fs_bsize);
		buflist->bs_children[i]->b_blkno = fsbtodb(fs, blkno);
	}
	return (0);

fail:
	if (ssize < len)
		brelse(ebp);
	if (sbap != &ip->i_db[0])
		brelse(sbp);
	return (ENOSPC);
}
#endif /* 0 */

/*
 * Allocate an inode in the file system.
 *
 * If allocating a directory, use ffs_dirpref to select the inode.
 * If allocating in a directory, the following hierarchy is followed:
 *   1) allocate the preferred inode.
 *   2) allocate an inode in the same cylinder group.
 *   3) quadradically rehash into other cylinder groups, until an
 *      available inode is located.
 * If no inode preference is given the following heirarchy is used
 * to allocate an inode:
 *   1) allocate an inode in cylinder group 0.
 *   2) quadradically rehash into other cylinder groups, until an
 *      available inode is located.
 */
/* This is now the diskfs_alloc_node callback from the diskfs library
   (described in <hurd/diskfs.h>).  It used to be ffs_valloc in BSD. */
error_t
diskfs_alloc_node (struct node *dir,
		   mode_t mode,
		   struct node **npp)
{
	register struct fs *fs;
	struct node *np;
	ino_t ino, ipref;
	int cg, error;
	int sex;

	fs = sblock;


	spin_lock (&alloclock);

	if (fs->fs_cstotal.cs_nifree == 0)
	  {
	    spin_unlock (&alloclock);
	    goto noinodes;
	  }

	if (S_ISDIR (mode))
		ipref = ffs_dirpref(fs);
	else
		ipref = dir->dn->number;

	if (ipref >= fs->fs_ncg * fs->fs_ipg)
		ipref = 0;
	cg = ino_to_cg(fs, ipref);
	ino = (ino_t)ffs_hashalloc(dir, cg, (long)ipref,
				   mode, ffs_nodealloccg);
	spin_unlock (&alloclock);
	if (ino == 0)
		goto noinodes;
	error = diskfs_cached_lookup (ino, &np);
	assert ("duplicate allocation" && !np->dn_stat.st_mode);
	assert (! (np->dn_stat.st_mode & S_IPTRANS));
	if (np->dn_stat.st_blocks) {
	  printf("free inode %Ld had %Ld blocks\n",
		 ino, np->dn_stat.st_blocks);
	  np->dn_stat.st_blocks = 0;
	  np->dn_set_ctime = 1;
	}
	np->dn_stat.st_flags = 0;
	/*
	 * Set up a new generation number for this inode.
	 */
	spin_lock (&gennumberlock);
	sex = diskfs_mtime->seconds;
	if (++nextgennumber < (u_long)sex)
		nextgennumber = sex;
	np->dn_stat.st_gen = nextgennumber;
	spin_unlock (&gennumberlock);

	*npp = np;
	alloc_sync (np);
	return (0);
noinodes:
	printf ("out of inodes");
/* 	ffs_fserr(fs, ap->a_cred->cr_uid, "out of inodes"); */
/*    uprintf("\n%s: create/symlink failed, no inodes free\n", fs->fs_fsmnt);*/
	return (ENOSPC);
}

/*
 * Find a cylinder to place a directory.
 *
 * The policy implemented by this algorithm is to select from
 * among those cylinder groups with above the average number of
 * free inodes, the one with the smallest number of directories.
 */
static ino_t
ffs_dirpref(register struct fs *fs)
{
	int cg, minndir, mincg, avgifree;

	avgifree = fs->fs_cstotal.cs_nifree / fs->fs_ncg;
	minndir = fs->fs_ipg;
	mincg = 0;
	for (cg = 0; cg < fs->fs_ncg; cg++)
		if (csum[cg].cs_ndir < minndir &&
		    csum[cg].cs_nifree >= avgifree) {
			mincg = cg;
			minndir = csum[cg].cs_ndir;
		}
	return ((ino_t)(fs->fs_ipg * mincg));
}

/*
 * Select the desired position for the next block in a file.  The file is
 * logically divided into sections. The first section is composed of the
 * direct blocks. Each additional section contains fs_maxbpg blocks.
 *
 * If no blocks have been allocated in the first section, the policy is to
 * request a block in the same cylinder group as the inode that describes
 * the file. If no blocks have been allocated in any other section, the
 * policy is to place the section in a cylinder group with a greater than
 * average number of free blocks.  An appropriate cylinder group is found
 * by using a rotor that sweeps the cylinder groups. When a new group of
 * blocks is needed, the sweep begins in the cylinder group following the
 * cylinder group from which the previous allocation was made. The sweep
 * continues until a cylinder group with greater than the average number
 * of free blocks is found. If the allocation is for the first block in an
 * indirect block, the information on the previous allocation is unavailable;
 * here a best guess is made based upon the logical block number being
 * allocated.
 *
 * If a section is already partially allocated, the policy is to
 * contiguously allocate fs_maxcontig blocks.  The end of one of these
 * contiguous blocks and the beginning of the next is physically separated
 * so that the disk head will be in transit between them for at least
 * fs_rotdelay milliseconds.  This is to allow time for the processor to
 * schedule another I/O transfer.
 */
daddr_t
ffs_blkpref(struct node *np,
	    daddr_t lbn,
	    int indx,
	    daddr_t *bap)
{
	register struct fs *fs;
	register int cg;
	int avgbfree, startcg;
	daddr_t nextblk;

	fs = sblock;
	spin_lock (&alloclock);
	if (indx % fs->fs_maxbpg == 0 || bap[indx - 1] == 0) {
		if (lbn < NDADDR) {
			cg = ino_to_cg(fs, np->dn->number);
			spin_unlock (&alloclock);
			return (fs->fs_fpg * cg + fs->fs_frag);
		}
		/*
		 * Find a cylinder with greater than average number of
		 * unused data blocks.
		 */
		if (indx == 0 || bap[indx - 1] == 0)
			startcg =
			    (ino_to_cg(fs, np->dn->number)
			     + lbn / fs->fs_maxbpg);
		else
			startcg = dtog(fs,
				       read_disk_entry (bap[indx - 1])) + 1;
		startcg %= fs->fs_ncg;
		avgbfree = fs->fs_cstotal.cs_nbfree / fs->fs_ncg;
		for (cg = startcg; cg < fs->fs_ncg; cg++)
			if (csum[cg].cs_nbfree >= avgbfree) {
				fs->fs_cgrotor = cg;
				spin_unlock (&alloclock);
				return (fs->fs_fpg * cg + fs->fs_frag);
			}
		for (cg = 0; cg <= startcg; cg++)
			if (csum[cg].cs_nbfree >= avgbfree) {
				fs->fs_cgrotor = cg;
				spin_unlock (&alloclock);
				return (fs->fs_fpg * cg + fs->fs_frag);
			}
		spin_unlock (&alloclock);
		return 0;
	}
	spin_unlock (&alloclock);
	/*
	 * One or more previous blocks have been laid out. If less
	 * than fs_maxcontig previous blocks are contiguous, the
	 * next block is requested contiguously, otherwise it is
	 * requested rotationally delayed by fs_rotdelay milliseconds.
	 */
	nextblk = read_disk_entry (bap[indx - 1]) + fs->fs_frag;
	if (indx < fs->fs_maxcontig
	    || (read_disk_entry (bap[indx - fs->fs_maxcontig]) +
		blkstofrags(fs, fs->fs_maxcontig) != nextblk))
	  {
	    return (nextblk);
	  }
	if (fs->fs_rotdelay != 0)
		/*
		 * Here we convert ms of delay to frags as:
		 * (frags) = (ms) * (rev/sec) * (sect/rev) /
		 *	((sect/frag) * (ms/sec))
		 * then round up to the next block.
		 */
		nextblk += roundup(fs->fs_rotdelay * fs->fs_rps * fs->fs_nsect /
		    (NSPF(fs) * 1000), fs->fs_frag);
	return (nextblk);
}

/*
 * Implement the cylinder overflow algorithm.
 *
 * The policy implemented by this algorithm is:
 *   1) allocate the block in its requested cylinder group.
 *   2) quadradically rehash on the cylinder group number.
 *   3) brute force search for a free block.
 */
/*VARARGS5*/
static u_long
ffs_hashalloc(struct node *np,
	      int cg,
	      long pref,
	      int size,	/* size for data blocks, mode for inodes */
	      u_long (*allocator)())
{
	register struct fs *fs;
	long result;
	int i, icg = cg;

	fs = sblock;
	/*
	 * 1: preferred cylinder group
	 */
	result = (*allocator)(np, cg, pref, size);
	if (result)
		return (result);
	/*
	 * 2: quadratic rehash
	 */
	for (i = 1; i < fs->fs_ncg; i *= 2) {
		cg += i;
		if (cg >= fs->fs_ncg)
			cg -= fs->fs_ncg;
		result = (*allocator)(np, cg, 0, size);
		if (result)
			return (result);
	}
	/*
	 * 3: brute force search
	 * Note that we start at i == 2, since 0 was checked initially,
	 * and 1 is always checked in the quadratic rehash.
	 */
	cg = (icg + 2) % fs->fs_ncg;
	for (i = 2; i < fs->fs_ncg; i++) {
		result = (*allocator)(np, cg, 0, size);
		if (result)
			return (result);
		cg++;
		if (cg == fs->fs_ncg)
			cg = 0;
	}
	return 0;
}

/*
 * Determine whether a fragment can be extended.
 *
 * Check to see if the necessary fragments are available, and
 * if they are, allocate them.
 */
static daddr_t
ffs_fragextend(struct node *np,
	       int cg,
	       long bprev,
	       int osize,
	       int nsize)
{
	register struct fs *fs;
	struct cg *cgp;
	long bno;
	int frags, bbase;
	int i;
	int releasecg;

	fs = sblock;
	if (csum[cg].cs_nffree < numfrags(fs, nsize - osize))
		return 0;
	frags = numfrags(fs, nsize);
	bbase = fragnum(fs, bprev);
	if (bbase > fragnum(fs, (bprev + frags - 1))) {
		/* cannot extend across a block boundary */
		return 0;
	}
#if 0 /* Wrong for GNU Hurd ufs */
	error = bread(ip->i_devvp, fsbtodb(fs, cgtod(fs, cg)),
		(int)fs->fs_cgsize, NOCRED, &bp);
	if (error) {
		brelse(bp);
		return (NULL);
	}
	cgp = (struct cg *)bp->b_data;
#else
	releasecg = read_cg (cg, &cgp);
#endif
	if (!cg_chkmagic(cgp)) {
/* 		brelse(bp); */
		if (releasecg)
			release_cg (cgp);
		return 0;
	}
	cgp->cg_time = diskfs_mtime->seconds;
	bno = dtogd(fs, bprev);
	for (i = numfrags(fs, osize); i < frags; i++)
		if (isclr(cg_blksfree(cgp), bno + i)) {
/* 			brelse(bp); */
			if (releasecg)
				release_cg (cgp);
			return 0;
		}
	/*
	 * the current fragment can be extended
	 * deduct the count on fragment being extended into
	 * increase the count on the remaining fragment (if any)
	 * allocate the extended piece
	 */
	for (i = frags; i < fs->fs_frag - bbase; i++)
		if (isclr(cg_blksfree(cgp), bno + i))
			break;
	cgp->cg_frsum[i - numfrags(fs, osize)]--;
	if (i != frags)
		cgp->cg_frsum[i - frags]++;
	for (i = numfrags(fs, osize); i < frags; i++) {
		clrbit(cg_blksfree(cgp), bno + i);
		cgp->cg_cs.cs_nffree--;
		fs->fs_cstotal.cs_nffree--;
		csum[cg].cs_nffree--;
	}
	if (releasecg)
		release_cg (cgp);
	record_poke (cgp, sblock->fs_cgsize);
	csum_dirty = 1;
	sblock_dirty = 1;
	fs->fs_fmod = 1;
/*	bdwrite(bp); */
	return (bprev);
}

/*
 * Determine whether a block can be allocated.
 *
 * Check to see if a block of the appropriate size is available,
 * and if it is, allocate it.
 */
static u_long
ffs_alloccg(struct node *np,
	    int cg,
	    daddr_t bpref,
	    int size)
{
	register struct fs *fs;
	struct cg *cgp;
	register int i;
	int bno, frags, allocsiz;
	int releasecg;

	fs = sblock;
	if (csum[cg].cs_nbfree == 0 && size == fs->fs_bsize)
		return 0;
#if 0 /* Not this way in GNU Hurd ufs */
	error = bread(ip->i_devvp, fsbtodb(fs, cgtod(fs, cg)),
		(int)fs->fs_cgsize, NOCRED, &bp);
	if (error) {
		brelse(bp);
		return (NULL);
	}
	cgp = (struct cg *)bp->b_data;
#else
	releasecg = read_cg (cg, &cgp);
#endif
	if (!cg_chkmagic(cgp) ||
	    (cgp->cg_cs.cs_nbfree == 0 && size == fs->fs_bsize)) {
/* 		brelse(bp); */
		if (releasecg)
			release_cg (cgp);
		return 0;
	}
	cgp->cg_time = diskfs_mtime->seconds;
	if (size == fs->fs_bsize) {
		bno = ffs_alloccgblk(fs, cgp, bpref);
/* 		bdwrite(bp); */
		if (releasecg)
			release_cg (cgp);
		return (bno);
	}
	/*
	 * check to see if any fragments are already available
	 * allocsiz is the size which will be allocated, hacking
	 * it down to a smaller size if necessary
	 */
	frags = numfrags(fs, size);
	for (allocsiz = frags; allocsiz < fs->fs_frag; allocsiz++)
		if (cgp->cg_frsum[allocsiz] != 0)
			break;
	if (allocsiz == fs->fs_frag) {
		/*
		 * no fragments were available, so a block will be
		 * allocated, and hacked up
		 */
		if (cgp->cg_cs.cs_nbfree == 0) {
/* 			brelse(bp); */
			if (releasecg)
				release_cg (cgp);
			return 0;
		}
		bno = ffs_alloccgblk(fs, cgp, bpref);
		bpref = dtogd(fs, bno);
		for (i = frags; i < fs->fs_frag; i++)
			setbit(cg_blksfree(cgp), bpref + i);
		i = fs->fs_frag - frags;
		cgp->cg_cs.cs_nffree += i;
		fs->fs_cstotal.cs_nffree += i;
		csum[cg].cs_nffree += i;
		fs->fs_fmod = 1;
		cgp->cg_frsum[i]++;

		if (releasecg)
			release_cg (cgp);
		record_poke (cgp, sblock->fs_cgsize);
		csum_dirty = 1;
		sblock_dirty = 1;
/* 		bdwrite(bp); */
		return (bno);
	}
	bno = ffs_mapsearch(fs, cgp, bpref, allocsiz);
	if (bno < 0) {
/* 		brelse(bp); */
		if (releasecg)
			release_cg (cgp);
		return 0;
	}
	for (i = 0; i < frags; i++)
		clrbit(cg_blksfree(cgp), bno + i);
	cgp->cg_cs.cs_nffree -= frags;
	fs->fs_cstotal.cs_nffree -= frags;
	csum[cg].cs_nffree -= frags;
	fs->fs_fmod = 1;
	cgp->cg_frsum[allocsiz]--;
	if (frags != allocsiz)
		cgp->cg_frsum[allocsiz - frags]++;
	if (releasecg)
		release_cg (cgp);
	record_poke (cgp, sblock->fs_cgsize);
	csum_dirty = 1;
	sblock_dirty = 1;
/* 	bdwrite(bp); */
	return (cg * fs->fs_fpg + bno);
}

/*
 * Allocate a block in a cylinder group.
 *
 * This algorithm implements the following policy:
 *   1) allocate the requested block.
 *   2) allocate a rotationally optimal block in the same cylinder.
 *   3) allocate the next available block on the block rotor for the
 *      specified cylinder group.
 * Note that this routine only allocates fs_bsize blocks; these
 * blocks may be fragmented by the routine that allocates them.
 */
static daddr_t
ffs_alloccgblk(register struct fs *fs,
	       register struct cg *cgp,
	       daddr_t bpref)
{
	daddr_t bno, blkno;
	int cylno, pos, delta;
	short *cylbp;
	register int i;

	if (bpref == 0 || dtog(fs, bpref) != cgp->cg_cgx) {
		bpref = cgp->cg_rotor;
		goto norot;
	}
	bpref = blknum(fs, bpref);
	bpref = dtogd(fs, bpref);
	/*
	 * if the requested block is available, use it
	 */
	if (ffs_isblock(fs, cg_blksfree(cgp), fragstoblks(fs, bpref))) {
		bno = bpref;
		goto gotit;
	}
	/*
	 * check for a block available on the same cylinder
	 */
	cylno = cbtocylno(fs, bpref);
	if (cg_blktot(cgp)[cylno] == 0)
		goto norot;
	if (fs->fs_cpc == 0) {
		/*
		 * Block layout information is not available.
		 * Leaving bpref unchanged means we take the
		 * next available free block following the one
		 * we just allocated. Hopefully this will at
		 * least hit a track cache on drives of unknown
		 * geometry (e.g. SCSI).
		 */
		goto norot;
	}
	/*
	 * check the summary information to see if a block is
	 * available in the requested cylinder starting at the
	 * requested rotational position and proceeding around.
	 */
	cylbp = cg_blks(fs, cgp, cylno);
	pos = cbtorpos(fs, bpref);
	for (i = pos; i < fs->fs_nrpos; i++)
		if (cylbp[i] > 0)
			break;
	if (i == fs->fs_nrpos)
		for (i = 0; i < pos; i++)
			if (cylbp[i] > 0)
				break;
	if (cylbp[i] > 0) {
		/*
		 * found a rotational position, now find the actual
		 * block. A panic if none is actually there.
		 */
		pos = cylno % fs->fs_cpc;
		bno = (cylno - pos) * fs->fs_spc / NSPB(fs);
		assert (fs_postbl(fs, pos)[i] != -1);
		for (i = fs_postbl(fs, pos)[i];; ) {
			if (ffs_isblock(fs, cg_blksfree(cgp), bno + i)) {
				bno = blkstofrags(fs, (bno + i));
				goto gotit;
			}
			delta = fs_rotbl(fs)[i];
			if (delta <= 0 ||
			    delta + i > fragstoblks(fs, fs->fs_fpg))
				break;
			i += delta;
		}
		printf("pos = %d, i = %d, fs = %s\n", pos, i, fs->fs_fsmnt);
		assert (0);
	}
norot:
	/*
	 * no blocks in the requested cylinder, so take next
	 * available one in this cylinder group.
	 */
	bno = ffs_mapsearch(fs, cgp, bpref, (int)fs->fs_frag);
	if (bno < 0)
		return 0;
	cgp->cg_rotor = bno;
gotit:
	blkno = fragstoblks(fs, bno);
	ffs_clrblock(fs, cg_blksfree(cgp), (long)blkno);
	ffs_clusteracct(fs, cgp, blkno, -1);
	cgp->cg_cs.cs_nbfree--;
	fs->fs_cstotal.cs_nbfree--;
	csum[cgp->cg_cgx].cs_nbfree--;
	cylno = cbtocylno(fs, bno);
	cg_blks(fs, cgp, cylno)[cbtorpos(fs, bno)]--;
	cg_blktot(cgp)[cylno]--;
	fs->fs_fmod = 1;
	record_poke (cgp, sblock->fs_cgsize);
	csum_dirty = 1;
	sblock_dirty = 1;
	return (cgp->cg_cgx * fs->fs_fpg + bno);
}

#if 0 /* Not needed in GNU Hurd ufs (yet?) */
/*
 * Determine whether a cluster can be allocated.
 *
 * We do not currently check for optimal rotational layout if there
 * are multiple choices in the same cylinder group. Instead we just
 * take the first one that we find following bpref.
 */
static daddr_t
ffs_clusteralloc(ip, cg, bpref, len)
	struct inode *ip;
	int cg;
	daddr_t bpref;
	int len;
{
	register struct fs *fs;
	register struct cg *cgp;
	struct buf *bp;
	int i, run, bno, bit, map;
	u_char *mapp;

	fs = ip->i_fs;
	if (fs->fs_cs(fs, cg).cs_nbfree < len)
		return (NULL);
	if (bread(ip->i_devvp, fsbtodb(fs, cgtod(fs, cg)), (int)fs->fs_cgsize,
	    NOCRED, &bp))
		goto fail;
	cgp = (struct cg *)bp->b_data;
	if (!cg_chkmagic(cgp))
		goto fail;
	/*
	 * Check to see if a cluster of the needed size (or bigger) is
	 * available in this cylinder group.
	 */
	for (i = len; i <= fs->fs_contigsumsize; i++)
		if (cg_clustersum(cgp)[i] > 0)
			break;
	if (i > fs->fs_contigsumsize)
		goto fail;
	/*
	 * Search the cluster map to find a big enough cluster.
	 * We take the first one that we find, even if it is larger
	 * than we need as we prefer to get one close to the previous
	 * block allocation. We do not search before the current
	 * preference point as we do not want to allocate a block
	 * that is allocated before the previous one (as we will
	 * then have to wait for another pass of the elevator
	 * algorithm before it will be read). We prefer to fail and
	 * be recalled to try an allocation in the next cylinder group.
	 */
	if (dtog(fs, bpref) != cg)
		bpref = 0;
	else
		bpref = fragstoblks(fs, dtogd(fs, blknum(fs, bpref)));
	mapp = &cg_clustersfree(cgp)[bpref / NBBY];
	map = *mapp++;
	bit = 1 << (bpref % NBBY);
	for (run = 0, i = bpref; i < cgp->cg_nclusterblks; i++) {
		if ((map & bit) == 0) {
			run = 0;
		} else {
			run++;
			if (run == len)
				break;
		}
		if ((i & (NBBY - 1)) != (NBBY - 1)) {
			bit <<= 1;
		} else {
			map = *mapp++;
			bit = 1;
		}
	}
	if (i == cgp->cg_nclusterblks)
		goto fail;
	/*
	 * Allocate the cluster that we have found.
	 */
	bno = cg * fs->fs_fpg + blkstofrags(fs, i - run + 1);
	len = blkstofrags(fs, len);
	for (i = 0; i < len; i += fs->fs_frag)
		if (ffs_alloccgblk(fs, cgp, bno + i) != bno + i)
			panic("ffs_clusteralloc: lost block");
	brelse(bp);
	return (bno);

fail:
	brelse(bp);
	return (0);
}
#endif

/*
 * Determine whether an inode can be allocated.
 *
 * Check to see if an inode is available, and if it is,
 * allocate it using the following policy:
 *   1) allocate the requested inode.
 *   2) allocate the next available inode after the requested
 *      inode in the specified cylinder group.
 */
static u_long
ffs_nodealloccg(struct node *np,
		int cg,
		daddr_t ipref,
		int mode)
{
	register struct fs *fs;
	struct cg *cgp;
	int start, len, loc, map, i;
	int releasecg;

	fs = sblock;
	if (csum[cg].cs_nifree == 0)
		return 0;
#if 0 /* Not this way in GNU Hurd ufs */
	error = bread(ip->i_devvp, fsbtodb(fs, cgtod(fs, cg)),
		(int)fs->fs_cgsize, NOCRED, &bp);
	if (error) {
		brelse(bp);
		return (NULL);
	}
	cgp = (struct cg *)bp->b_data;
#else
	releasecg = read_cg (cg, &cgp);
#endif
	if (!cg_chkmagic(cgp) || cgp->cg_cs.cs_nifree == 0) {
/*		brelse(bp); */
		if (releasecg)
			release_cg (cgp);
		return 0;
	}
	cgp->cg_time = diskfs_mtime->seconds;
	if (ipref) {
		ipref %= fs->fs_ipg;
		if (isclr(cg_inosused(cgp), ipref))
			goto gotit;
	}
	start = cgp->cg_irotor / NBBY;
	len = howmany(fs->fs_ipg - cgp->cg_irotor, NBBY);
	loc = skpc(0xff, len, &cg_inosused(cgp)[start]);
	if (loc == 0) {
		len = start + 1;
		start = 0;
		loc = skpc(0xff, len, &cg_inosused(cgp)[0]);
		assert (loc != 0);
	}
	i = start + len - loc;
	map = cg_inosused(cgp)[i];
	ipref = i * NBBY;
	for (i = 1; i < (1 << NBBY); i <<= 1, ipref++) {
		if ((map & i) == 0) {
			cgp->cg_irotor = ipref;
			goto gotit;
		}
	}
	assert (0);
	/* NOTREACHED */
gotit:
	setbit(cg_inosused(cgp), ipref);
	cgp->cg_cs.cs_nifree--;
	fs->fs_cstotal.cs_nifree--;
	csum[cg].cs_nifree--;
	fs->fs_fmod = 1;
	if ((mode & IFMT) == IFDIR) {
		cgp->cg_cs.cs_ndir++;
		fs->fs_cstotal.cs_ndir++;
		csum[cg].cs_ndir++;
	}
	if (releasecg)
		release_cg (cgp);
	record_poke (cgp, sblock->fs_cgsize);
	csum_dirty = 1;
	sblock_dirty = 1;
/* 	bdwrite(bp); */
	return (cg * fs->fs_ipg + ipref);
}

/*
 * Free a block or fragment.
 *
 * The specified block or fragment is placed back in the
 * free map. If a fragment is deallocated, a possible
 * block reassembly is checked.
 */
void
ffs_blkfree(register struct node *np,
	    daddr_t bno,
	    long size)
{
	register struct fs *fs;
	struct cg *cgp;
	daddr_t blkno;
	int i, cg, blk, frags, bbase;
	int releasecg;

	fs = sblock;
	assert ((u_int)size <= fs->fs_bsize && !fragoff (fs, size));
	cg = dtog(fs, bno);
	if ((u_int)bno >= fs->fs_size) {
		printf("bad block %ld, ino %Ld\n", bno, np->dn->number);
/*		ffs_fserr(fs, ip->i_uid, "bad block"); */
		return;
	}
#if 0 /* Not this way in GNU Hurd ufs */
	error = bread(ip->i_devvp, fsbtodb(fs, cgtod(fs, cg)),
		(int)fs->fs_cgsize, NOCRED, &bp);
	if (error) {
		brelse(bp);
		return;
	}
	cgp = (struct cg *)bp->b_data;
#else
	releasecg = read_cg (cg, &cgp);
#endif
	if (!cg_chkmagic(cgp)) {
/* 		brelse(bp); */
		if (releasecg)
			release_cg (cgp);
		return;
	}
	cgp->cg_time = diskfs_mtime->seconds;
	bno = dtogd(fs, bno);
	if (size == fs->fs_bsize) {
		blkno = fragstoblks(fs, bno);
		assert (!ffs_isblock(fs, cg_blksfree (cgp), blkno));
		ffs_setblock(fs, cg_blksfree(cgp), blkno);
		ffs_clusteracct(fs, cgp, blkno, 1);
		cgp->cg_cs.cs_nbfree++;
		fs->fs_cstotal.cs_nbfree++;
		csum[cg].cs_nbfree++;
		i = cbtocylno(fs, bno);
		cg_blks(fs, cgp, i)[cbtorpos(fs, bno)]++;
		cg_blktot(cgp)[i]++;
	} else {
		bbase = bno - fragnum(fs, bno);
		/*
		 * decrement the counts associated with the old frags
		 */
		blk = blkmap(fs, cg_blksfree(cgp), bbase);
		ffs_fragacct(fs, blk, cgp->cg_frsum, -1);
		/*
		 * deallocate the fragment
		 */
		frags = numfrags(fs, size);
		for (i = 0; i < frags; i++) {
		  assert (!isset (cg_blksfree(cgp), bno + i));
		  setbit(cg_blksfree(cgp), bno + i);
		}
		cgp->cg_cs.cs_nffree += i;
		fs->fs_cstotal.cs_nffree += i;
		csum[cg].cs_nffree += i;
		/*
		 * add back in counts associated with the new frags
		 */
		blk = blkmap(fs, cg_blksfree(cgp), bbase);
		ffs_fragacct(fs, blk, cgp->cg_frsum, 1);
		/*
		 * if a complete block has been reassembled, account for it
		 */
		blkno = fragstoblks(fs, bbase);
		if (ffs_isblock(fs, cg_blksfree(cgp), blkno)) {
			cgp->cg_cs.cs_nffree -= fs->fs_frag;
			fs->fs_cstotal.cs_nffree -= fs->fs_frag;
			csum[cg].cs_nffree -= fs->fs_frag;
			ffs_clusteracct(fs, cgp, blkno, 1);
			cgp->cg_cs.cs_nbfree++;
			fs->fs_cstotal.cs_nbfree++;
			csum[cg].cs_nbfree++;
			i = cbtocylno(fs, bbase);
			cg_blks(fs, cgp, i)[cbtorpos(fs, bbase)]++;
			cg_blktot(cgp)[i]++;
		}
	}
	if (releasecg)
		release_cg (cgp);
	record_poke (cgp, sblock->fs_cgsize);
	csum_dirty = 1;
	sblock_dirty = 1;
	fs->fs_fmod = 1;
	alloc_sync (np);
/* 	bdwrite(bp); */
}

/*
 * Free an inode.
 *
 * The specified inode is placed back in the free map.
 */
/* Implement diskfs call back diskfs_free_node (described in
   <hurd/diskfs.h>.  This was called ffs_vfree in BSD. */
void
diskfs_free_node (struct node *np, mode_t mode)
{
	register struct fs *fs;
	struct cg *cgp;
	ino_t ino = np->dn->number;
	int cg;
	int releasecg;

	fs = sblock;
	assert (ino < fs->fs_ipg * fs->fs_ncg);
	cg = ino_to_cg(fs, ino);
#if 0 /* Not this way in GNU Hurd ufs */
	error = bread(pip->i_devvp, fsbtodb(fs, cgtod(fs, cg)),
		(int)fs->fs_cgsize, NOCRED, &bp);
	if (error) {
		brelse(bp);
		return (0);
	}
	cgp = (struct cg *)bp->b_data;
#else
	releasecg = read_cg (cg, &cgp);
#endif
	if (!cg_chkmagic(cgp)) {
/* 		brelse(bp); */
		if (releasecg)
			release_cg (cgp);
		return;
	}
	cgp->cg_time = diskfs_mtime->seconds;
	ino %= fs->fs_ipg;
	if (isclr(cg_inosused(cgp), ino)) {
/*		printf("dev = 0x%x, ino = %Ld, fs = %s\n",
		    pip->i_dev, ino, fs->fs_fsmnt); */
		assert (diskfs_readonly);
	}
	clrbit(cg_inosused(cgp), ino);
	if (ino < cgp->cg_irotor)
		cgp->cg_irotor = ino;
	cgp->cg_cs.cs_nifree++;
	fs->fs_cstotal.cs_nifree++;
	csum[cg].cs_nifree++;
	if ((mode & IFMT) == IFDIR) {
		cgp->cg_cs.cs_ndir--;
		fs->fs_cstotal.cs_ndir--;
		csum[cg].cs_ndir--;
	}
	if (releasecg)
		release_cg (cgp);
	record_poke (cgp, sblock->fs_cgsize);
	csum_dirty = 1;
	sblock_dirty = 1;
	fs->fs_fmod = 1;
	alloc_sync (np);
/* 	bdwrite(bp); */
}

/*
 * Find a block of the specified size in the specified cylinder group.
 *
 * It is a panic if a request is made to find a block if none are
 * available.
 */
static daddr_t
ffs_mapsearch(register struct fs *fs,
	      register struct cg *cgp,
	      daddr_t bpref,
	      int allocsiz)
{
	daddr_t bno;
	int start, len, loc, i;
	int blk, field, subfield, pos;

	/*
	 * find the fragment by searching through the free block
	 * map for an appropriate bit pattern
	 */
	if (bpref)
		start = dtogd(fs, bpref) / NBBY;
	else
		start = cgp->cg_frotor / NBBY;
	len = howmany(fs->fs_fpg, NBBY) - start;
	loc = scanc((u_int)len, (u_char *)&cg_blksfree(cgp)[start],
		(u_char *)fragtbl[fs->fs_frag],
		(u_char)(1 << (allocsiz - 1 + (fs->fs_frag % NBBY))));
	if (loc == 0) {
		len = start + 1;
		start = 0;
		loc = scanc((u_int)len, (u_char *)&cg_blksfree(cgp)[0],
			(u_char *)fragtbl[fs->fs_frag],
			(u_char)(1 << (allocsiz - 1 + (fs->fs_frag % NBBY))));
		assert (loc);

	}
	bno = (start + len - loc) * NBBY;
	cgp->cg_frotor = bno;
	/*
	 * found the byte in the map
	 * sift through the bits to find the selected frag
	 */
	for (i = bno + NBBY; bno < i; bno += fs->fs_frag) {
		blk = blkmap(fs, cg_blksfree(cgp), bno);
		blk <<= 1;
		field = around[allocsiz];
		subfield = inside[allocsiz];
		for (pos = 0; pos <= fs->fs_frag - allocsiz; pos++) {
			if ((blk & field) == subfield)
				return (bno + pos);
			field <<= 1;
			subfield <<= 1;
		}
	}
	assert (0);
	return (-1);
}

/*
 * Update the cluster map because of an allocation or free.
 *
 * Cnt == 1 means free; cnt == -1 means allocating.
 */
static void
ffs_clusteracct(struct fs *fs,
		struct cg *cgp,
		daddr_t blkno,
		int cnt)
{
	long *sump;
	u_char *freemapp, *mapp;
	int i, start, end, forw, back, map, bit;

	if (fs->fs_contigsumsize <= 0)
		return;
	freemapp = cg_clustersfree(cgp);
	sump = cg_clustersum(cgp);
	/*
	 * Allocate or clear the actual block.
	 */
	if (cnt > 0)
		setbit(freemapp, blkno);
	else
		clrbit(freemapp, blkno);
	/*
	 * Find the size of the cluster going forward.
	 */
	start = blkno + 1;
	end = start + fs->fs_contigsumsize;
	if (end >= cgp->cg_nclusterblks)
		end = cgp->cg_nclusterblks;
	mapp = &freemapp[start / NBBY];
	map = *mapp++;
	bit = 1 << (start % NBBY);
	for (i = start; i < end; i++) {
		if ((map & bit) == 0)
			break;
		if ((i & (NBBY - 1)) != (NBBY - 1)) {
			bit <<= 1;
		} else {
			map = *mapp++;
			bit = 1;
		}
	}
	forw = i - start;
	/*
	 * Find the size of the cluster going backward.
	 */
	start = blkno - 1;
	end = start - fs->fs_contigsumsize;
	if (end < 0)
		end = -1;
	mapp = &freemapp[start / NBBY];
	map = *mapp--;
	bit = 1 << (start % NBBY);
	for (i = start; i > end; i--) {
		if ((map & bit) == 0)
			break;
		if ((i & (NBBY - 1)) != 0) {
			bit >>= 1;
		} else {
			map = *mapp--;
			bit = 1 << (NBBY - 1);
		}
	}
	back = start - i;
	/*
	 * Account for old cluster and the possibly new forward and
	 * back clusters.
	 */
	i = back + forw + 1;
	if (i > fs->fs_contigsumsize)
		i = fs->fs_contigsumsize;
	sump[i] += cnt;
	if (back > 0)
		sump[back] -= cnt;
	if (forw > 0)
		sump[forw] -= cnt;
}

#if 0
/*
 * Fserr prints the name of a file system with an error diagnostic.
 *
 * The form of the error message is:
 *	fs: error message
 */
static void
ffs_fserr(fs, uid, cp)
	struct fs *fs;
	u_int uid;
	char *cp;
{

	log(LOG_ERR, "uid %d on %s: %s\n", uid, fs->fs_fsmnt, cp);
}
#endif
