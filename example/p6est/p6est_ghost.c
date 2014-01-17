/*
  This file is part of p4est.
  p4est is a C library to manage a collection (a forest) of multiple
  connected adaptive quadtrees or octrees in parallel.

  Copyright (C) 2014 The University of Texas System
  Written by Carsten Burstedde, Lucas C. Wilcox, and Tobin Isaac

  p4est is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  p4est is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with p4est; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <p6est_ghost.h>

static int
p2est_quadrant_compare_piggy (const void *A, const void *B)
{
  const p2est_quadrant_t *a = (const p2est_quadrant_t *) A;
  const p2est_quadrant_t *b = (const p2est_quadrant_t *) B;
  p4est_qcoord_t      qdiff;
  p4est_topidx_t      tdiff;

  tdiff = a->p.which_tree - b->p.which_tree;
  if (tdiff) {
    return tdiff;
  }

  qdiff = a->z - b->z;
  if (qdiff) {
    return qdiff;
  }

  return (int) (a->level - b->level);
}

static              size_t
ghost_tree_type (sc_array_t * array, size_t zindex, void *data)
{
  p2est_quadrant_t   *q;

  P4EST_ASSERT (array->elem_size == sizeof (p2est_quadrant_t));

  q = (p2est_quadrant_t *) sc_array_index (array, zindex);
  return (size_t) q->p.which_tree;
}

static void
p6est_ghost_send_front_layers (p6est_ghost_t * ghost,
                               int nneighin,
                               p6est_t * p6est,
                               p4est_locidx_t * recv_off,
                               p4est_locidx_t * recv_count)
{
  sc_array_t         *recv_requests, *recv_procs;
  int                 i, j, *ip;
  int                 mpisize = p6est->mpisize, mpiret, nneighout;
  p4est_locidx_t      lfirst, llast, lj, midx, lcount, loffset;
  MPI_Request        *req;
  p4est_ghost_t      *cghost = ghost->column_ghost;
  sc_array_t         *glayers = &ghost->ghosts;
  sc_array_t         *cmirrors = &cghost->mirrors;
  p4est_locidx_t     *cmpfo = cghost->mirror_proc_front_offsets;
  p4est_locidx_t     *cmpf = cghost->mirror_proc_fronts;
  sc_array_t         *send, *send_requests;
  p4est_locidx_t     *mpm = ghost->mirror_proc_mirrors;
  p4est_locidx_t     *mpo = ghost->mirror_proc_offsets;
  p4est_locidx_t     *mpf = ghost->mirror_proc_fronts;
  p4est_locidx_t     *mpfo = ghost->mirror_proc_front_offsets;
  p4est_locidx_t     *lmpfo;
  p4est_quadrant_t   *col;
  p4est_topidx_t      which_tree;
  p4est_tree_t       *tree;
  size_t              zfirst, zlast, zz, old_count;
  p4est_locidx_t      nlmirror;
  sc_array_t         *layers = p6est->layers;
  p2est_quadrant_t   *layer, *mlayer;
  sc_array_t         *lmirrors = &ghost->mirrors, *new_mirrors;
  sc_array_t         *nmpfa, *nmpma;
  p4est_topidx_t      num_trees = ghost->num_trees;

  /* create the recv data */
  recv_requests = sc_array_new_size (sizeof (MPI_Request), (size_t) nneighin);
  recv_procs = sc_array_new_size (sizeof (int), (size_t) nneighin);

  /* post the receives */
  j = 0;
  for (i = 0; i < mpisize; i++) {
    lfirst = recv_off[i];
    lcount = recv_count[i];
    if (lcount) {
      req = (MPI_Request *) sc_array_index (recv_requests, j);
      ip = (int *) sc_array_index (recv_procs, j++);
      *ip = i;
      mpiret = MPI_Irecv (sc_array_index (glayers, (size_t) lfirst),
                          (int) lcount * sizeof (p2est_quadrant_t), MPI_BYTE,
                          i, P6EST_COMM_GHOST, p6est->mpicomm, req);
      SC_CHECK_MPI (mpiret);
    }
  }
  P4EST_ASSERT (j == nneighin);

  /* loop over the column mirror fronts to determine counts */
  lmpfo = P4EST_ALLOC (p4est_locidx_t, mpisize + 1);
  loffset = 0;

  j = 0;
  for (i = 0; i < mpisize; i++) {
    lmpfo[i] = loffset;
    lfirst = cmpfo[i];
    llast = cmpfo[i + 1];
    if (lfirst == llast) {
      continue;
    }
    j++;
    for (lj = lfirst; lj < llast; lj++) {
      midx = cmpf[lj];
      col = p4est_quadrant_array_index (cmirrors, midx);
      which_tree = col->p.piggy3.which_tree;
      midx = col->p.piggy3.local_num;
      tree = p4est_tree_array_index (p6est->columns->trees, which_tree);
      col = p4est_quadrant_array_index (&tree->quadrants,
                                        midx - tree->quadrants_offset);
      P6EST_COLUMN_GET_RANGE (col, &zfirst, &zlast);
      P4EST_ASSERT (zlast > zfirst);
      loffset += (zlast - zfirst);
    }
  }
  lmpfo[mpisize] = nlmirror = loffset;
  nneighout = j;

  send = sc_array_new_size (sizeof (p2est_quadrant_t), (size_t) nlmirror);
  send_requests = sc_array_new_size (sizeof (MPI_Request), nneighout);

  /* fill the send buffer */
  j = 0;
  loffset = 0;
  for (i = 0; i < mpisize; i++) {
    P4EST_ASSERT (lmpfo[i] == loffset);
    lfirst = cmpfo[i];
    llast = cmpfo[i + 1];
    lcount = llast - lfirst;
    if (!lcount) {
      continue;
    }
    req = (MPI_Request *) sc_array_index (send_requests, j);
    j++;
    for (lj = lfirst; lj < llast; lj++) {
      midx = cmpf[lj];
      col = p4est_quadrant_array_index (cmirrors, midx);
      which_tree = col->p.piggy3.which_tree;
      midx = col->p.piggy3.local_num;
      tree = p4est_tree_array_index (p6est->columns->trees, which_tree);
      col = p4est_quadrant_array_index (&tree->quadrants,
                                        midx - tree->quadrants_offset);
      P6EST_COLUMN_GET_RANGE (col, &zfirst, &zlast);
      for (zz = zfirst; zz < zlast; zz++) {
        layer = p2est_quadrant_array_index (layers, zz);
        mlayer = p2est_quadrant_array_index (send, loffset++);
        *mlayer = *layer;
        mlayer->p.piggy3.which_tree = which_tree;
        mlayer->p.piggy3.local_num = (p4est_locidx_t) zz;
      }
    }
    lfirst = lmpfo[i];
    llast = lmpfo[i + 1];
    lcount = llast - lfirst;
    P4EST_ASSERT (lcount);
    mpiret = MPI_Isend (sc_array_index (send, (size_t) lfirst),
                        (int) lcount * sizeof (p2est_quadrant_t), MPI_BYTE,
                        i, P6EST_COMM_GHOST, p6est->mpicomm, req);
    P4EST_ASSERT (lmpfo[i + 1] == loffset);
  }

  /* update all of the mirror structures */
  new_mirrors = sc_array_new_size (lmirrors->elem_size, lmirrors->elem_count);
  sc_array_copy (new_mirrors, lmirrors);
  old_count = lmirrors->elem_count;
  sc_array_resize (new_mirrors, old_count + nlmirror);
  if (send->elem_count) {
    memcpy (sc_array_index (new_mirrors, old_count),
            sc_array_index (send, 0), send->elem_count * send->elem_size);
  }
  sc_array_sort (new_mirrors, p2est_quadrant_compare_piggy);
  sc_array_uniq (new_mirrors, p2est_quadrant_compare_piggy);

  P4EST_ASSERT (new_mirrors->elem_count >= old_count);
  if (new_mirrors->elem_count > old_count) {
    p4est_topidx_t      ti;
    sc_array_t          split;

    /* update mirror_tree_offsets */
    sc_array_init (&split, sizeof (size_t));
    sc_array_split (new_mirrors, &split, (size_t) num_trees, ghost_tree_type,
                    NULL);
    P4EST_ASSERT (split.elem_count == (size_t) num_trees + 1);
    for (ti = 0; ti <= num_trees; ti++) {
      size_t             *ppz;

      ppz = (size_t *) sc_array_index (&split, (size_t) ti);
      ghost->mirror_tree_offsets[ti] = *ppz;
    }
    sc_array_reset (&split);
  }

  /* update mirror_proc_fronts */
  nmpfa = sc_array_new (sizeof (p4est_locidx_t));
  for (i = 0; i < mpisize; i++) {
    size_t              offset = nmpfa->elem_count;

    sc_array_resize (nmpfa, offset + lmpfo[i + 1] - lmpfo[i]);
    mpfo[i] = (p4est_locidx_t) offset;

    for (zz = lmpfo[i]; zz < lmpfo[i + 1]; zz++) {
      ssize_t             idx;
      p2est_quadrant_t   *q1 = p2est_quadrant_array_index (send, zz);
      p4est_locidx_t     *lp = (p4est_locidx_t *) sc_array_index (nmpfa, zz);

      idx = sc_array_bsearch (new_mirrors, q1, p2est_quadrant_compare_piggy);
      P4EST_ASSERT (idx >= 0);
      *lp = (p4est_locidx_t) idx;
    }
  }
  mpfo[mpisize] = nmpfa->elem_count;
  if (ghost->mirror_proc_fronts != NULL) {
    if (ghost->mirror_proc_fronts != ghost->mirror_proc_mirrors) {
      P4EST_FREE (ghost->mirror_proc_fronts);
    }
  }
  ghost->mirror_proc_fronts = mpf =
    P4EST_ALLOC (p4est_locidx_t, nmpfa->elem_count);
  memcpy (mpf, nmpfa->array, nmpfa->elem_size * nmpfa->elem_count);
  sc_array_destroy (nmpfa);

  if (ghost->mirror_proc_mirrors == NULL) {
    P4EST_ASSERT (ghost->mirror_proc_offsets == NULL);
    /* this is the first ghost layer construction: the fronts are all of the
     * mirrors */
    ghost->mirror_proc_mirrors = ghost->mirror_proc_fronts;
    ghost->mirror_proc_offsets = ghost->mirror_proc_front_offsets;
  }
  else {
    /* update mirror_proc_mirrors */
    nmpma = sc_array_new (sizeof (p4est_locidx_t));
    for (i = 0; i < mpisize; i++) {
      size_t              frontsize = mpfo[i + 1] - mpfo[i];
      size_t              offset = nmpma->elem_count;
      p4est_locidx_t      old_offset = mpo[i];

      old_count = mpo[i + 1] - old_offset;

      mpo[i] = offset;

      sc_array_resize (nmpma, offset + old_count + frontsize);
      memcpy (nmpma->array + nmpma->elem_size * offset,
              mpf + mpfo[i], sizeof (p4est_locidx_t) * frontsize);

      if (old_count) {
        sc_array_t          pview;

        for (zz = 0; zz < old_count; zz++) {
          ssize_t             idx;
          p2est_quadrant_t   *q1 = p2est_quadrant_array_index (lmirrors,
                                                               mpm[zz +
                                                                   old_offset]);
          p4est_locidx_t     *lp = (p4est_locidx_t *)
            sc_array_index (nmpma, offset + frontsize + zz);

          idx = sc_array_bsearch (new_mirrors, q1,
                                  p2est_quadrant_compare_piggy);
          P4EST_ASSERT (idx >= 0);
          *lp = (p4est_locidx_t) idx;
        }
        sc_array_init_view (&pview, nmpma, offset, old_count + frontsize);
        sc_array_sort (&pview, p4est_locidx_compare);
        sc_array_reset (&pview);
      }
    }
    mpo[mpisize] = nmpma->elem_count;
    if (ghost->mirror_proc_mirrors != NULL) {
      P4EST_FREE (ghost->mirror_proc_mirrors);
    }
    ghost->mirror_proc_mirrors =
      P4EST_ALLOC (p4est_locidx_t, nmpma->elem_count);
    memcpy (mpm, nmpma->array, nmpma->elem_size * nmpma->elem_count);
    sc_array_destroy (nmpma);
  }

  sc_array_resize (lmirrors, new_mirrors->elem_count);
  sc_array_copy (lmirrors, new_mirrors);
  sc_array_destroy (new_mirrors);
  P4EST_FREE (lmpfo);

  {
    int                 outcount;
    int                *array_of_indices = P4EST_ALLOC (int, nneighin);
    int                 nleft = nneighin;

    /* finish the receives */
    while (nleft) {
      mpiret = MPI_Waitsome (nneighin, (MPI_Request *) recv_requests->array,
                             &outcount, array_of_indices,
                             MPI_STATUSES_IGNORE);
      SC_CHECK_MPI (mpiret);

      if (recv_off != ghost->proc_offsets) {
        for (i = 0; i < outcount; i++) {
          sc_array_t          pview;

          j = array_of_indices[i];
          j = *((int *) sc_array_index_int (recv_procs, j));

          sc_array_init_view (&pview, &ghost->ghosts, ghost->proc_offsets[j],
                              ghost->proc_offsets[j + 1] -
                              ghost->proc_offsets[j]);
          sc_array_sort (&pview, p2est_quadrant_compare_piggy);
          sc_array_reset (&pview);
        }
      }
      nleft -= outcount;
    }

    P4EST_FREE (array_of_indices);

    sc_array_destroy (recv_requests);
    sc_array_destroy (recv_procs);
  }

  /* finish the sends */
  mpiret = MPI_Waitall (nneighout, (MPI_Request *) send_requests->array,
                        MPI_STATUSES_IGNORE);
  SC_CHECK_MPI (mpiret);

  sc_array_destroy (send);
  sc_array_destroy (send_requests);

  return;
}

typedef union p4est_quadrant_data p4est_quadrant_data_t;

p6est_ghost_t      *
p6est_ghost_new (p6est_t * p6est, p4est_connect_type_t btype)
{
  p4est_t            *columns = p6est->columns;
  p4est_ghost_t      *cghost;
  sc_array_t         *gcols;
  p6est_ghost_t      *ghost = P4EST_ALLOC (p6est_ghost_t, 1);
  p4est_topidx_t      num_trees, it, thiscol;
  p4est_locidx_t      il, ngcol, nglayer, offset;
  p4est_locidx_t     *clo, *ctree_off, *cproc_off, *tree_off, *proc_off;
  p4est_locidx_t     *proc_count;
  p4est_quadrant_data_t *cdata;
  size_t              first, last, count;
  int                 nneigh;
  int                 i, mpisize;

  /* create the column ghost layer */
  ghost->column_ghost = cghost = p4est_ghost_new (columns, btype);
  ghost->mpisize = mpisize = cghost->mpisize;
  ghost->num_trees = num_trees = cghost->num_trees;
  ghost->btype = btype;

  /* create the column layer offsets */
  gcols = &cghost->ghosts;
  ngcol = gcols->elem_count;
  ghost->column_layer_offsets =
    sc_array_new_size (sizeof (p4est_locidx_t), (size_t) ngcol + 1);
  clo = (p4est_locidx_t *) ghost->column_layer_offsets->array;

  /* create an array of the data for each ghost column */
  cdata = P4EST_ALLOC (p4est_quadrant_data_t, ngcol);

  /* get the ghost column data using p4est_ghost_exchange_data */
  P4EST_ASSERT (sizeof (void *) == sizeof (p4est_quadrant_data_t));
  p4est_ghost_exchange_data (columns, cghost, cdata);

  /* fill column layer offsets using each ghost column's count */
  offset = 0;
  for (il = 0; il < ngcol; il++) {
    p4est_quadrant_t    dummy;

    clo[il] = offset;
    dummy.p = cdata[il];
    P6EST_COLUMN_GET_RANGE (&dummy, &first, &last);
    count = last - first;
    offset += count;
  }
  clo[ngcol] = nglayer = offset;
  P4EST_FREE (cdata);

  /* create the tree and proc offsets */
  ghost->tree_offsets = tree_off =
    P4EST_ALLOC (p4est_locidx_t, num_trees + 1);
  ghost->proc_offsets = proc_off = P4EST_ALLOC (int, mpisize + 1);
  ghost->mirror_proc_front_offsets = P4EST_ALLOC (int, mpisize + 1);
  ghost->mirror_tree_offsets = P4EST_ALLOC (p4est_locidx_t, num_trees + 1);
  ghost->mirror_proc_fronts = NULL;     /* these three are set below */
  ghost->mirror_proc_offsets = NULL;
  ghost->mirror_proc_mirrors = NULL;
  proc_count = P4EST_ALLOC (int, mpisize);
  ctree_off = cghost->tree_offsets;
  cproc_off = cghost->proc_offsets;

  /* fill tree offsets */
  tree_off[0] = 0;
  for (it = 1; it < num_trees; it++) {
    if (ctree_off[it] == ctree_off[it - 1]) {
      tree_off[it] = tree_off[it - 1];
    }
    else {
      thiscol = ctree_off[it];
      tree_off[it] = clo[thiscol];
    }
  }
  tree_off[num_trees] = nglayer;

  /* fill proc offsets */
  proc_off[0] = 0;
  nneigh = 0;
  for (i = 1; i <= mpisize; i++) {
    if (cproc_off[i] == cproc_off[i - 1]) {
      proc_off[i] = proc_off[i - 1];
      proc_count[i - 1] = 0;
    }
    else {
      nneigh++;
      if (i < mpisize) {
        thiscol = cproc_off[i];
        proc_off[i] = clo[thiscol];
      }
      else {
        proc_off[i] = nglayer;
      }
      P4EST_ASSERT (proc_off[i] > proc_off[i - 1]);
      proc_count[i - 1] = (proc_off[i] - proc_off[i - 1]);
    }
  }

  /* create the ghost array */
  sc_array_init_size (&ghost->ghosts, sizeof (p2est_quadrant_t), nglayer);
  sc_array_init (&ghost->mirrors, sizeof (p2est_quadrant_t));

  p6est_ghost_send_front_layers (ghost, nneigh, p6est, proc_off, proc_count);

  P4EST_FREE (proc_count);

  return ghost;
}

void
p6est_ghost_destroy (p6est_ghost_t * ghost)
{
  p4est_ghost_destroy (ghost->column_ghost);
  sc_array_destroy (ghost->column_layer_offsets);
  sc_array_reset (&ghost->ghosts);
  P4EST_FREE (ghost->tree_offsets);
  P4EST_FREE (ghost->proc_offsets);
  sc_array_reset (&ghost->mirrors);
  P4EST_FREE (ghost->mirror_tree_offsets);
  if (ghost->mirror_proc_fronts != ghost->mirror_proc_mirrors) {
    P4EST_ASSERT (ghost->mirror_proc_front_offsets !=
                  ghost->mirror_proc_offsets);
    P4EST_FREE (ghost->mirror_proc_fronts);
    P4EST_FREE (ghost->mirror_proc_front_offsets);
  }
  P4EST_FREE (ghost->mirror_proc_mirrors);
  P4EST_FREE (ghost->mirror_proc_offsets);
  P4EST_FREE (ghost);
}
