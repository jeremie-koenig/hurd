/* Sock functions

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

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

#include <string.h>		/* For bzero() */

#include <cthreads.h>

#include <hurd/pipe.h>

#include "sock.h"
#include "sserver.h"

#include "debug.h"

/* ---------------------------------------------------------------- */

/* Returns the pipe that SOCK is reading from in PIPE, locked and with an
   additional reference, or an error saying why it's not possible.  SOCK
   mustn't be locked.  */
error_t
sock_aquire_read_pipe (struct sock *sock, struct pipe **pipe)
{
  error_t err = 0;
debug (sock, "in");

debug (sock, "lock");
  mutex_lock (&sock->lock);

  *pipe = sock->read_pipe;
  assert (*pipe);		/* A socket always has a read pipe.  */

  if (((*pipe)->flags & PIPE_BROKEN)
      && ! (sock->flags & SOCK_CONNECTED)
      && ! (sock->flags & SOCK_SHUTDOWN_READ))
    /* A broken pipe with no peer is not connected (only connection-oriented
       sockets can have broken pipes.  However this is not true if the
       read-half has been explicitly shutdown [at least in netbsd].  */
{debug (sock, "enotconn");
    err = ENOTCONN;
}
  else
    pipe_aquire (*pipe);

debug (sock, "unlock");
  mutex_unlock (&sock->lock);

debug (sock, "out");
  return err;
}

/* Returns the pipe that SOCK is writing to in PIPE, locked and with an
   additional reference, or an error saying why it's not possible.  SOCK
   mustn't be locked.  */
error_t
sock_aquire_write_pipe (struct sock *sock, struct pipe **pipe)
{
  error_t err = 0;
debug (sock, "in");

debug (sock, "lock");
  mutex_lock (&sock->lock);
  *pipe = sock->write_pipe;
  if (*pipe != NULL)
    pipe_aquire (*pipe);	/* Do this before unlocking the sock!  */
  else if (sock->flags & SOCK_SHUTDOWN_WRITE)
    /* Writing on a socket with the write-half shutdown always acts as if the
       pipe were broken, even if the socket isn't connected yet [at least in
       netbsd].  */
{debug (sock, "epipe");
    err = EPIPE;
}
  else if (sock->read_pipe->class->flags & PIPE_CLASS_CONNECTIONLESS)
    /* Connectionless protocols give a different error when unconnected.  */
{debug (sock, "edestaddrreq");
    err = EDESTADDRREQ;
}
  else
{debug (sock, "enotconn");
    err = ENOTCONN;
}
debug (sock, "unlock");
  mutex_unlock (&sock->lock);

debug (sock, "out");
  return err;
}

/* ---------------------------------------------------------------- */

/* Return a new socket with the given pipe class in SOCK.  */
error_t
sock_create (struct pipe_class *pipe_class, struct sock **sock)
{
  error_t err;
  static unsigned next_sock_id = 0;
  struct sock *new = malloc (sizeof (struct sock));

  if (new == NULL)
    return ENOMEM;

  /* A socket always has a read pipe, so create it here.  */
  err = pipe_create (pipe_class, &new->read_pipe);
  if (err)
    {
      free (new);
      return err;
    }
  if (! (pipe_class->flags & PIPE_CLASS_CONNECTIONLESS))
    /* No data source yet.  */
    new->read_pipe->flags |= PIPE_BROKEN;
  new->read_pipe->refs++;

  new->refs = 0;
  new->flags = 0;
  new->write_pipe = NULL;
  new->id = next_sock_id++;
  new->connq = NULL;
  new->addr = NULL;
  bzero (&new->change_time, sizeof (new->change_time));
  mutex_init (&new->lock);

  *sock = new;
  return 0;
}

/* Free SOCK, assuming there are no more handle on it.  */
void
sock_free (struct sock *sock)
{
debug (sock, "in");
  /* sock_shutdown will get rid of the write pipe.  */
  sock_shutdown (sock, SOCK_SHUTDOWN_READ | SOCK_SHUTDOWN_WRITE);

  /* But we must do the read pipe ourselves.  */
  pipe_release (sock->read_pipe);

debug (sock, "bye");
  free (sock);
}

/* Free a sock derefed too far.  */
void
_sock_norefs (struct sock *sock)
{
  /* A sock should never have an address when it has 0 refs, as the
     address should hold a reference to the sock!  */
  assert (sock->addr == NULL);
  mutex_unlock (&sock->lock);	/* Unlock so sock_free can do stuff.  */
  sock_free (sock);
}

/* ---------------------------------------------------------------- */

/* Return a new socket largely copied from TEMPLATE.  */
error_t
sock_clone (struct sock *template, struct sock **sock)
{
  error_t err = sock_create (template->read_pipe->class, sock);

  if (err)
    return err;

  /* Copy some properties from TEMPLATE.  */
  (*sock)->flags = template->flags & ~SOCK_CONNECTED;

  return 0;
}

/* ---------------------------------------------------------------- */

struct port_class *sock_user_port_class;

/* Get rid of a user reference to a socket.  */
static void
sock_user_clean (void *vuser)
{
  struct sock_user *user = vuser;
debug (user, "bye");
  sock_deref (user->sock);
}

/* Return a new user port on SOCK in PORT.  */
error_t
sock_create_port (struct sock *sock, mach_port_t *port)
{
  struct sock_user *user =
    ports_allocate_port (sock_port_bucket,
			 sizeof (struct sock_user), sock_user_port_class);

  if (!user)
    return ENOMEM;

  ensure_sock_server ();

debug (sock, "lock, refs++");
  mutex_lock (&sock->lock);
  sock->refs++;
debug (sock, "unlock");
  mutex_unlock (&sock->lock);

  user->sock = sock;

  *port = ports_get_right (user);
  ports_port_deref (user);	/* We only want one ref, for the send right. */
debug (sock, "user: %p, refs: %d", user, user->pi.refcnt);

  return 0;
}

/* ---------------------------------------------------------------- */
/* Address manipulation.  */

struct addr
{
  struct port_info pi;
  struct sock *sock;
  struct mutex lock;
};

struct port_class *addr_port_class;

/* Get rid of ADDR's socket's reference to it, in preparation for ADDR going
   away.  */
static void
addr_unbind (void *vaddr)
{
  struct sock *sock;
  struct addr *addr = vaddr;

debug (addr, "in");
debug (addr, "lock");
  mutex_lock (&addr->lock);
  sock = addr->sock;
  if (sock)
    {
debug (sock, "(sock) lock");
      mutex_lock (&sock->lock);
      sock->addr = NULL;
      addr->sock = NULL;
      ports_port_deref_weak (addr);
debug (sock, "(sock) unlock");
      mutex_unlock (&sock->lock);
      sock_deref (sock);
    }
debug (addr, "unlock");
  mutex_unlock (&addr->lock);
debug (addr, "out");
}

/* Cleanup after the address ADDR, which is going away... */
static void
addr_clean (void *vaddr)
{
  struct addr *addr = vaddr;
  /* ADDR should never have a socket bound to it at this point, as it should
     have been removed by addr_unbind dropping the socket's weak reference
     it.  */
debug (addr, "bye");
  assert (addr->sock == NULL);
}

/* Return a new address, not connected to any socket yet, ADDR.  */
inline error_t
addr_create (struct addr **addr)
{
  *addr =
    ports_allocate_port (sock_port_bucket,
			 sizeof (struct addr), addr_port_class);
  if (! *addr)
    return ENOMEM;

  ensure_sock_server ();

  (*addr)->sock = NULL;
  mutex_init (&(*addr)->lock);

  return 0;
}

/* Bind SOCK to ADDR.  */
error_t
sock_bind (struct sock *sock, struct addr *addr)
{
  error_t err = 0;
  struct addr *old_addr;

debug (addr, "(addr) lock");
  mutex_lock (&addr->lock);
debug (sock, "lock");
  mutex_lock (&sock->lock);

  old_addr = sock->addr;
  if (addr && old_addr)
    err = EINVAL;		/* SOCK already bound.  */
  else if (addr && addr->sock)
    err = EADDRINUSE;		/* Something else already bound ADDR.  */
  else if (addr)
    addr->sock = sock;		/* First binding for SOCK.  */
  else
    old_addr->sock = NULL;	/* Unbinding SOCK.  */

  if (!err)
    sock->addr = addr;

  if (addr)
{debug (sock, "refs++");
    sock->refs++;
}
  if (old_addr)
    {
      /* Note that we don't have to worry about SOCK's ref count going to zero
	 because whoever's calling us should be holding a ref somehow.  */
debug (sock, "refs--");
      sock->refs--;
      assert (sock->refs > 0);	/* But make sure... */
    }

debug (sock, "unlock");
  mutex_unlock (&sock->lock);
debug (addr, "(addr) unlock");
  mutex_unlock (&addr->lock);

  return err;
}

/* Returns SOCK's addr, with an additional reference, fabricating one if
   necessary.  SOCK should be locked.  */
static inline error_t
ensure_addr (struct sock *sock, struct addr **addr)
{
  error_t err = 0;

  if (! sock->addr)
    {
      err = addr_create (&sock->addr);
      if (!err)
	{
	  sock->addr->sock = sock;
	  sock->refs++;
	  ports_port_ref_weak (sock->addr);
	}
    }
  else
    ports_port_ref (sock->addr);

  if (!err)
    *addr = sock->addr;

  return err;
}

/* Returns the socket bound to ADDR in SOCK, or EADDRNOTAVAIL.  The returned
   sock will have one reference added to it.  */
error_t
addr_get_sock (struct addr *addr, struct sock **sock)
{
debug (addr, "lock");
  mutex_lock (&addr->lock);
  *sock = addr->sock;
  if (*sock)
    (*sock)->refs++;
debug (addr, "unlock");
  mutex_unlock (&addr->lock);
  return *sock ? 0 : EADDRNOTAVAIL;
}

/* Returns SOCK's address in ADDR, with an additional reference added.  If
   SOCK doesn't currently have an address, one is fabricated first.  */
error_t
sock_get_addr (struct sock *sock, struct addr **addr)
{
  error_t err;

debug (sock, "lock");
  mutex_lock (&sock->lock);
  err = ensure_addr (sock, addr);
debug (sock, "unlock");
  mutex_unlock (&sock->lock);

  return err;			/* XXX */
}

/* ---------------------------------------------------------------- */

/* We hold this lock before we lock two sockets at once, to prevent someone
   else trying to lock the same two sockets in the reverse order, resulting
   in a deadlock.  */
static struct mutex socket_pair_lock;

/* Connect SOCK1 and SOCK2.  */
error_t
sock_connect (struct sock *sock1, struct sock *sock2)
{
  error_t err = 0;
  /* In the case of a connectionless protocol, an already-connected socket may
     be reconnected, so save the old destination for later disposal.  */
  struct pipe *old_sock1_write_pipe = NULL;
  struct addr *old_sock1_write_addr = NULL;
  struct pipe_class *pipe_class = sock1->read_pipe->class;
  /* True if this protocol is a connectionless one.  */
  int connless = (pipe_class->flags & PIPE_CLASS_CONNECTIONLESS);

  void connect (struct sock *wr, struct sock *rd)
    {
      if (!(   (wr->flags & SOCK_SHUTDOWN_WRITE)
	    || (rd->flags & SOCK_SHUTDOWN_READ)))
	{
	  struct pipe *pipe = rd->read_pipe;
debug (wr, "connect: %p, pipe: %p", rd, pipe);
	  pipe_aquire (pipe);
	  pipe->flags &= ~PIPE_BROKEN; /* Not yet...  */
	  wr->write_pipe = pipe;
debug (pipe, "(pipe) unlock");
	  mutex_unlock (&pipe->lock);
	}
    }

debug (sock1, "in: %p", sock2);
  if (sock2->read_pipe->class != pipe_class)
    /* Incompatible socket types.  */
{debug (sock1, "eopnotsupp");
    return EOPNOTSUPP;		/* XXX?? */
}

debug (sock1, "socket pair lock");
  mutex_lock (&socket_pair_lock);
debug (sock1, "lock");
  mutex_lock (&sock1->lock);
  if (sock1 != sock2)
    /* If SOCK1 == SOCK2, then we get a fifo!  */
{debug (sock2, "lock");
    mutex_lock (&sock2->lock);
}

  if ((sock1->flags & SOCK_CONNECTED) || (sock2->flags & SOCK_CONNECTED))
    /* An already-connected socket.  */
{debug (sock1, "eisconn");
    err = EISCONN;
}
  else
    {
      old_sock1_write_pipe = sock1->write_pipe;
      old_sock1_write_addr = sock1->write_addr;

      /* Always make the forward connection.  */
      connect (sock1, sock2);

      /* Only make the reverse for connection-oriented protocols.  */
      if (! connless)
	{
	  sock1->flags |= SOCK_CONNECTED;
	  if (sock1 != sock2)
	    {
	      connect (sock2, sock1);
	      sock2->flags |= SOCK_CONNECTED;
	    }
	}
    }

  if (sock1 != sock2)
{debug (sock2, "unlock");
    mutex_unlock (&sock2->lock);
}
debug (sock1, "unlock");
  mutex_unlock (&sock1->lock);
debug (sock1, "socket pair unlock");
  mutex_unlock (&socket_pair_lock);

  if (old_sock1_write_pipe)
    {
      pipe_break (old_sock1_write_pipe);
      ports_port_deref (old_sock1_write_addr);
    }

debug (sock1, "out");
  return err;
}

/* ---------------------------------------------------------------- */

/* Shutdown either the read or write halves of SOCK, depending on whether the
   SOCK_SHUTDOWN_READ or SOCK_SHUTDOWN_WRITE flags are set in FLAGS.  */
void
sock_shutdown (struct sock *sock, unsigned flags)
{
  unsigned old_flags;

debug (sock, "in");
debug (sock, "lock");
  mutex_lock (&sock->lock);

  old_flags = sock->flags;
  sock->flags |= flags;

  if (flags & SOCK_SHUTDOWN_READ && !(old_flags & SOCK_SHUTDOWN_READ))
    /* Shutdown the read half.  We keep the pipe around though.  */
    {
      struct pipe *pipe = sock->read_pipe;
debug (sock, "read half");
debug (pipe, "(pipe) lock");
      mutex_lock (&pipe->lock);
      /* This will prevent any further writes to PIPE.  */
      pipe->flags |= PIPE_BROKEN;
      /* Make sure subsequent reads return EOF.  */
      pipe_drain (pipe);
debug (pipe, "(pipe) unlock");
      mutex_unlock (&pipe->lock);
    }

  if (flags & SOCK_SHUTDOWN_WRITE && !(old_flags & SOCK_SHUTDOWN_WRITE))
    /* Shutdown the write half.  */
    {
      struct pipe *pipe = sock->write_pipe;
debug (sock, "write half");
      if (pipe != NULL)
	{
	  sock->write_pipe = NULL;
	  /* Unlock SOCK here, as we may subsequently wake up other threads. */
debug (sock, "unlock");
	  mutex_unlock (&sock->lock);
	  pipe_break (pipe);
	}
      else
{debug (sock, "unlock");
	mutex_unlock (&sock->lock);
}
    }
  else
{debug (sock, "unlock");
    mutex_unlock (&sock->lock);
}
debug (sock, "out");
}

/* ---------------------------------------------------------------- */

error_t
sock_global_init ()
{
  sock_port_bucket = ports_create_bucket ();
  sock_user_port_class = ports_create_class (sock_user_clean, NULL);
  addr_port_class = ports_create_class (addr_clean, addr_unbind);
  return 0;
}

/* Try to shutdown any active sockets, returning EBUSY if we can't.  */
error_t
sock_global_shutdown ()
{
  int num_ports = ports_count_bucket (sock_port_bucket);
  ports_enable_bucket (sock_port_bucket);
  return (num_ports == 0 ? 0 : EBUSY);
}
