/* Copyright (c) 2002,2003,2004,2009 James M. Kretchmar
 *
 * This file is part of Owl.
 *
 * Owl is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Owl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Owl.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------
 * 
 * As of Owl version 2.1.12 there are patches contributed by
 * developers of the branched BarnOwl project, Copyright (c)
 * 2006-2009 The BarnOwl Developers. All rights reserved.
 */

/* This select loop implementation was contributed by developers of
 * the branched BarnOwl project.
 */
 
#include "owl.h"

static const char fileIdent[] = "$Id $";

static int dispatch_active = 0;

/* Returns the index of the dispatch for the file descriptor. */
int owl_select_find_dispatch(int fd)
{
  int i, len;
  owl_list *dl;
  owl_dispatch *d;

  dl = owl_global_get_dispatchlist(&g);
  len = owl_list_get_size(dl);
  owl_function_debugmsg("test: len is %i", len);
  for(i = 0; i < len; i++) {
    d = (owl_dispatch*)owl_list_get_element(dl, i);
    if (d->fd == fd) return i;
  }
  return -1;
}

void owl_select_remove_dispatch_at(int elt) /* noproto */
{
  owl_list *dl;
  owl_dispatch *d;

  dl = owl_global_get_dispatchlist(&g);
  d = (owl_dispatch*)owl_list_get_element(dl, elt);
  owl_list_remove_element(dl, elt);
  if (d->destroy) {
    d->destroy(d);
  }
}

/* Adds a new owl_dispatch to the list, replacing existing ones if needed. */
void owl_select_add_dispatch(owl_dispatch *d)
{
  int elt;
  owl_list *dl;

  d->needs_gc = 0;

  elt = owl_select_find_dispatch(d->fd);
  dl = owl_global_get_dispatchlist(&g);

  if (elt != -1) {  /* If we have a dispatch for this FD */
    owl_dispatch *d_old;
    owl_function_debugmsg("select: duplicate dispatch found");
    d_old = (owl_dispatch*)owl_list_get_element(dl, elt);
    /* Ignore if we're adding the same dispatch again.  Otherwise
       replace the old dispatch. */
    if (d_old != d) {
      owl_select_remove_dispatch_at(elt);
    }
  }
  owl_list_append_element(dl, d);
}

/* Removes an owl_dispatch to the list, based on it's file descriptor. */
void owl_select_remove_dispatch(int fd)
{
  int elt;
  owl_list *dl;
  owl_dispatch *d;

  elt = owl_select_find_dispatch(fd);
  if(elt == -1) {
    return;
  } else if(dispatch_active) {
    /* Defer the removal until dispatch is done walking the list */
    dl = owl_global_get_dispatchlist(&g);
    d = (owl_dispatch*)owl_list_get_element(dl, elt);
    d->needs_gc = 1;
  } else {
    owl_select_remove_dispatch_at(elt);
  }
}

int owl_select_dispatch_count()
{
  return owl_list_get_size(owl_global_get_dispatchlist(&g));
}

int owl_select_dispatch_prepare_fd_sets(fd_set *r, fd_set *e)
{
  int i, len, max_fd;
  owl_dispatch *d;
  owl_list *dl;

  dl = owl_global_get_dispatchlist(&g);
  FD_ZERO(r);
  FD_ZERO(e);
  max_fd = 0;
  len = owl_select_dispatch_count(g);
  for(i = 0; i < len; i++) {
    d = (owl_dispatch*)owl_list_get_element(dl, i);
    FD_SET(d->fd, r);
    FD_SET(d->fd, e);
    if (max_fd < d->fd) max_fd = d->fd;
  }
  return max_fd + 1;
}

void owl_select_gc()
{
  int i;
  owl_list *dl;

  dl = owl_global_get_dispatchlist(&g);
  /*
   * Count down so we aren't set off by removing items from the list
   * during the iteration.
   */
  for(i = owl_list_get_size(dl) - 1; i >= 0; i--) {
    owl_dispatch *d = owl_list_get_element(dl, i);
    if(d->needs_gc) {
      owl_select_remove_dispatch_at(i);
    }
  }
}

void owl_select_dispatch(fd_set *fds, int max_fd)
{
  int i, len;
  owl_dispatch *d;
  owl_list *dl;

  dl = owl_global_get_dispatchlist(&g);
  len = owl_select_dispatch_count();

  dispatch_active = 1;

  for(i = 0; i < len; i++) {
    d = (owl_dispatch*)owl_list_get_element(dl, i);
    /* While d shouldn't normally be null, the list may be altered by
     * functions we dispatch to. */
    if (d != NULL && !d->needs_gc && FD_ISSET(d->fd, fds)) {
      if (d->cfunc != NULL) {
        d->cfunc(d);
      }
    }
  }

  dispatch_active = 0;
  owl_select_gc();
}

int owl_select_aim_hack(fd_set *rfds, fd_set *wfds)
{
  aim_conn_t *cur;
  aim_session_t *sess;
  int max_fd;

  FD_ZERO(rfds);
  FD_ZERO(wfds);
  max_fd = 0;
  sess = owl_global_get_aimsess(&g);
  for (cur = sess->connlist, max_fd = 0; cur; cur = cur->next) {
    if (cur->fd != -1) {
      FD_SET(cur->fd, rfds);
      if (cur->status & AIM_CONN_STATUS_INPROGRESS) {
        /* Yes, we're checking writable sockets here. Without it, AIM
           login is really slow. */
        FD_SET(cur->fd, wfds);
      }
      
      if (cur->fd > max_fd)
        max_fd = cur->fd;
    }
  }
  return max_fd;
}

void owl_select()
{
  int i, max_fd, aim_max_fd, aim_done;
  fd_set r;
  fd_set e;
  fd_set aim_rfds, aim_wfds;
  struct timeval timeout;

  /* owl_select_process_timers(&timeout); */

  /* settings to 5 seconds for the moment, we can raise this when the
   * odd select behavior with zephyr is understood
   */
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;

  max_fd = owl_select_dispatch_prepare_fd_sets(&r, &e);

  /* AIM HACK: 
   *
   *  The problem - I'm not sure where to hook into the owl/faim
   *  interface to keep track of when the AIM socket(s) open and
   *  close. In particular, the bosconn thing throws me off. So,
   *  rather than register particular dispatchers for AIM, I look up
   *  the relevant FDs and add them to select's watch lists, then
   *  check for them individually before moving on to the other
   *  dispatchers. --asedeno
   */
  aim_done = 1;
  FD_ZERO(&aim_rfds);
  FD_ZERO(&aim_wfds);
  if (owl_global_is_doaimevents(&g)) {
    aim_done = 0;
    aim_max_fd = owl_select_aim_hack(&aim_rfds, &aim_wfds);
    if (max_fd < aim_max_fd) max_fd = aim_max_fd;
    for(i = 0; i <= aim_max_fd; i++) {
      if (FD_ISSET(i, &aim_rfds)) {
        FD_SET(i, &r);
        FD_SET(i, &e);
      }
    }
  }
  /* END AIM HACK */

  if ( select(max_fd+1, &r, &aim_wfds, &e, &timeout) ) {
    /* Merge fd_sets and clear AIM FDs. */
    for(i = 0; i <= max_fd; i++) {
      /* Merge all interesting FDs into one set, since we have a
         single dispatch per FD. */
      if (FD_ISSET(i, &r) || FD_ISSET(i, &aim_wfds) || FD_ISSET(i, &e)) {
        /* AIM HACK: no separate dispatch, just process here if
           needed, and only once per run through. */
        if (!aim_done && (FD_ISSET(i, &aim_rfds) || FD_ISSET(i, &aim_wfds))) {
          owl_process_aim();
          aim_done = 1;
        }
        else {
          FD_SET(i, &r);
        }
      }
    }
    /* NOTE: the same dispatch function is called for both exceptional
       and read ready FDs. */
    owl_select_dispatch(&r, max_fd);
  }
}
