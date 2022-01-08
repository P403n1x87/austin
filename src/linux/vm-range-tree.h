// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef VM_RANGE_TREE_H
#define VM_RANGE_TREE_H

#include <stdint.h>
#include <stdlib.h>

#include "../hints.h"

typedef uintptr_t addr_t;

typedef struct _vmrange {
  addr_t            lo, hi;
  char            * name;
  struct _vmrange * left;
  struct _vmrange * right;
  int               height;
} vm_range_t;

typedef struct{
  vm_range_t *root;
} vm_range_tree_t;

#define max(a, b) ((a > b) ? a : b)
#define vm_range__height(r) (isvalid(r) ? r->height : 0)

#ifdef PY_PROC_C

/**
 * Create a new VM range.
 * 
 * @param lo    the range lower bound
 * @param hi    the range upper bound
 * @param name  the name of the VM map (takes ownership)
 * 
 * @return a valid reference to a VM range object, NULL otherwise.
 */
vm_range_t *
vm_range_new(addr_t lo, addr_t hi, char *name) {
  vm_range_t *range = (vm_range_t *)malloc(sizeof(vm_range_t));

  range->lo = lo;
  range->hi = hi;
  range->name = name;
  range->left = NULL;
  range->right = NULL;
  range->height = 1;

  return range;
}

/**
 * Deallocate a VM range.
 * 
 * @param self  the VM range to deallocate.
 */
void
vm_range__destroy(vm_range_t *self) {
  if (!isvalid(self))
    return;

  sfree(self->name);
  
  vm_range__destroy(self->left);
  vm_range__destroy(self->right);

  free(self);
}


static inline vm_range_t *
_vm_range__rrot(vm_range_t *self) {
  vm_range_t *x = self->left;
  vm_range_t *T2 = x->right;

  x->right = self;
  self->left = T2;

  self->height = max(vm_range__height(self->left), vm_range__height(self->right)) + 1;
  x->height = max(vm_range__height(x->left), vm_range__height(x->right)) + 1;

  return x;
}


static inline vm_range_t *
_vm_range__lrot(vm_range_t *self) {
  vm_range_t *y = self->right;
  vm_range_t *T2 = y->left;

  y->left = self;
  self->right = T2;

  self->height = max(vm_range__height(self->left), vm_range__height(self->right)) + 1;
  y->height = max(vm_range__height(y->left), vm_range__height(y->right)) + 1;

  return y;
}


static inline int
_vm_range__bf(vm_range_t *self) {
  return isvalid(self)
             ? vm_range__height(self->left) - vm_range__height(self->right)
             : 0;
}

static inline vm_range_t *
_vm_range__add(vm_range_t *self, vm_range_t *range) {
  if (!isvalid(self))
    return range;

  if (range->lo < self->lo)
    self->left = _vm_range__add(self->left, range);
  else
    self->right = _vm_range__add(self->right, range);

  self->height = 1 + max(vm_range__height(self->left), vm_range__height(self->right));

  // Balance the tree
  int balance = _vm_range__bf(self);
  if (balance > 1)
  {
    if (range->lo < self->left->lo)
      return _vm_range__rrot(self);
    else
    {
      self->left = _vm_range__lrot(self->left);
      return _vm_range__rrot(self);
    }
  }
  else if (balance < -1)
  {
    if (range->lo > self->right->lo)
      return _vm_range__lrot(self);
    else
    {
      self->right = _vm_range__rrot(self->right);
      return _vm_range__lrot(self);
    }
  }

  return self;
}


/**
 * Create a new VM range tree. This is an implementation of an AVL tree that
 * is meant to store *non-overlapping* VM ranges for a fast look-up.
 * 
 * @return a valid reference to a new VM range tree, NULL otherwise.
 */
vm_range_tree_t *
vm_range_tree_new() {
  return (vm_range_tree_t *)calloc(1, sizeof(vm_range_tree_t));
}


/**
 * Add a new range to the VM range tree.
 * 
 * The callee has the responsibility of ensuring that all the VM ranges that
 * are added to this data structure are *non-overlapping*. Failure to comply to
 * this constraint will make lookups fairly pointless.
 */
void
vm_range_tree__add(vm_range_tree_t *self, vm_range_t *range) {
  self->root = _vm_range__add(self->root, range);
}


void
vm_range_tree__destroy(vm_range_tree_t *self) {
  if (!isvalid(self))
    return;

  vm_range__destroy(self->root);

  free(self);
}

#endif // PY_PROC_C

#ifdef PY_THREAD_C

static inline vm_range_t *
_vm_range__find(vm_range_t *self, addr_t addr) {
  if (!isvalid(self))
    return NULL;

  if (addr >= self->lo && addr < self->hi)
    return self;

  return _vm_range__find(addr < self->lo ? self->left : self->right, addr);
}


/**
 * Query the tree for the range that contains the given address (if any).
 * 
 * If any of the ranges stored within the VM range tree data structure overlap,
 * the result of this method might be meaningless.
 *
 * @param self  the VM range tree to query.
 * @param addr  the address to look up.
 * 
 * @return a valid reference to a VM range, NULL otherwise.
 */
vm_range_t *
vm_range_tree__find(vm_range_tree_t *self, addr_t addr) {
  return _vm_range__find(self->root, addr);
}

#endif // PY_THREAD_C

#endif
