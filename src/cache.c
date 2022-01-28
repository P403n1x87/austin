// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2021 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#ifdef DEBUG
#include <math.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "cache.h"
#include "logging.h"

// -- Queue -------------------------------------------------------------------

// ----------------------------------------------------------------------------
queue_item_t *
queue_item_new(value_t value, key_dt key) {
  queue_item_t *item = (queue_item_t *)calloc(1, sizeof(queue_item_t));

  item->value = value;
  item->key = key;

  return item;
}

// ----------------------------------------------------------------------------
void
queue_item__destroy(queue_item_t * self, void (*deallocator)(value_t)) {
  if (!isvalid(self))
    return;

  deallocator(self->value);

  free(self);
}

// ----------------------------------------------------------------------------
queue_t *
queue_new(int capacity, void (*deallocator)(value_t)) {
  queue_t *queue = (queue_t *)calloc(1, sizeof(queue_t));

  queue->capacity    = capacity;
  queue->deallocator = deallocator;

  return queue;
}

// ----------------------------------------------------------------------------
int
queue__is_full(queue_t *queue) {
  return queue->count == queue->capacity;
}

// ----------------------------------------------------------------------------
int
queue__is_empty(queue_t *queue) {
  return queue->rear == NULL;
}

// ----------------------------------------------------------------------------
value_t
queue__dequeue(queue_t *queue) {
  if (queue__is_empty(queue))
    return NULL;

  if (queue->front == queue->rear)
    queue->front = NULL;

  queue_item_t *temp = queue->rear;
  queue->rear = queue->rear->prev;

  if (queue->rear)
    queue->rear->next = NULL;

  void *value = temp->value;
  free(temp);

  queue->count--;

  return value;
}

// ----------------------------------------------------------------------------
queue_item_t *
queue__enqueue(queue_t *self, value_t value, key_dt key) {
  if (queue__is_full(self)) 
    return NULL;

  queue_item_t *temp = queue_item_new(value, key);
  temp->next = self->front;

  if (queue__is_empty(self))
    self->rear = self->front = temp;
  else {
    self->front->prev = temp;
    self->front = temp;
  }

  self->count++;

  return temp;
}

// ----------------------------------------------------------------------------
void
queue__destroy(queue_t *self) {
  if (!isvalid(self))
    return;

  queue_item_t * next = NULL;
  for (queue_item_t *item = self->front; isvalid(item); item = next) {
    next = item->next;
    queue_item__destroy(item, self->deallocator);
  }

  free(self);
}


// -- Hash Table --------------------------------------------------------------

// ----------------------------------------------------------------------------
chain_t *
chain_new(key_dt key, value_t value) {
  chain_t *chain = (chain_t *)calloc(1, sizeof(chain_t));

  chain->key = key;
  chain->value = value;

  return chain;
}

// ----------------------------------------------------------------------------
int
chain__add(chain_t *self, key_dt key, value_t value) {
  if (!isvalid(self))
    return 0;

  if (!isvalid(self->next)) {
    self->next = chain_new(key, value);
    return 1;
  }

  if (self->next->key == key) {
    self->next->value = value;
  } else
    return chain__add(self->next, key, value);

  return 0;
}

// ----------------------------------------------------------------------------
int
chain__remove(chain_t * self, key_dt key) {
  if (!isvalid(self) || !isvalid(self->next))
    return FALSE;

  if (self->next->key == key) {
    chain_t * next = self->next;
    self->next = next->next;
    next->next = NULL;

    free(next);

    return TRUE;
  }

  return chain__remove(self->next, key);
}

// ----------------------------------------------------------------------------
value_t
chain__find(chain_t * self, key_dt key) {
  if (!isvalid(self))
    return NULL;

  if (self->key == key)
    return self->value;

  return chain__find(self->next, key);
}

// ----------------------------------------------------------------------------
int
chain__has(chain_t * self, key_dt key) {
  if (!isvalid(self))
    return FALSE;

  if (self->key == key)
    return TRUE;

  return chain__has(self->next, key);
}

// ----------------------------------------------------------------------------
void chain__destroy(chain_t * self) {
  if (!isvalid(self))
    return;

  chain__destroy(self->next);

  free(self);
}

// ----------------------------------------------------------------------------
hash_table_t *
hash_table_new(int capacity) {
  hash_table_t *hash = (hash_table_t *) calloc(1, sizeof(hash_table_t));

  hash->capacity = capacity;
  hash->chains = (chain_t **) calloc(hash->capacity, sizeof(chain_t *));

  return hash;
}

// ----------------------------------------------------------------------------
#define MAGIC 2654435761

static inline index_t
_hash_table__index(hash_table_t *self, key_dt key) {
  return (uintptr_t)((key * MAGIC) % self->capacity);
}

// ----------------------------------------------------------------------------
value_t
hash_table__get(hash_table_t *self, key_dt key) {
  if (!isvalid(self))
    return NULL;

  chain_t * chain = self->chains[_hash_table__index(self, key)];
  if (!isvalid(chain))
    return NULL;

  return chain__find(chain, key);
}

// ----------------------------------------------------------------------------
#ifdef DEBUG
static unsigned int _set_total = 0;
static unsigned int _set_empty = 0;
#endif

void
hash_table__set(hash_table_t *self, key_dt key, value_t value) {
  if (!isvalid(self))
    return;

  index_t index = _hash_table__index(self, key);

  #ifdef DEBUG
  _set_total++;
  #endif

  chain_t * chain = self->chains[index];
  if (!isvalid(chain)) {
    if (self->size >= self->capacity)
      return;
    #ifdef DEBUG
    _set_empty++;
    #endif
    self->chains[index] = chain_head();
    self->size += chain__add(self->chains[index], key, value);
    return;
  }

  if ((self->size >= self->capacity) && !chain__has(chain, key))
    return;

  self->size += chain__add(self->chains[index], key, value);
}

// ----------------------------------------------------------------------------
void
hash_table__del(hash_table_t * self, key_dt key) {
  if (!isvalid(self) || self->size == 0)
    return;

  index_t   index = _hash_table__index(self, key);
  chain_t * chain = self->chains[index];

  if (!isvalid(chain))
    return;

  self->size -= chain__remove(chain, key);

  if (chain__is_empty(chain)) {
    chain__destroy(chain);
    self->chains[index] = NULL;
  }
}

// ----------------------------------------------------------------------------
void
hash_table__destroy(hash_table_t *self) {
  if (!isvalid(self))
    return;

  if (isvalid(self->chains)) {
    for (int i = 0; i < self->capacity; chain__destroy(self->chains[i++]));
    sfree(self->chains);
  }

  free(self);
}


// -- LRU Cache ---------------------------------------------------------------

// ----------------------------------------------------------------------------
lru_cache_t *
lru_cache_new(int capacity, void (*deallocator)(value_t)) {
  lru_cache_t *cache = (lru_cache_t *)calloc(1, sizeof(lru_cache_t));

  cache->capacity = capacity;
  cache->queue    = queue_new(capacity, deallocator);
  cache->hash     = hash_table_new((capacity * 4 / 3) | 1);

  return cache;
}

// ----------------------------------------------------------------------------
value_t
lru_cache__maybe_hit(lru_cache_t *self, key_dt key) {
  queue_item_t *item = (queue_item_t *)hash_table__get(self->hash, key);

  if (!isvalid(item))
    return NULL;

  // Bring hit element to the front of the queue
  if (item != self->queue->front)
  {
    item->prev->next = item->next;
    if (item->next)
      item->next->prev = item->prev;

    if (item == self->queue->rear)
    {
      self->queue->rear = item->prev;
      self->queue->rear->next = NULL;
    }

    item->next = self->queue->front;
    item->prev = NULL;

    item->next->prev = item;

    self->queue->front = item;
  }

  return item->value;
}

// ----------------------------------------------------------------------------
void
lru_cache__store(lru_cache_t *self, key_dt key, value_t value) {
  queue_t * queue = self->queue;

  if (queue__is_full(queue)) {
    hash_table__del(self->hash, queue->rear->key);
    
    value_t value = queue__dequeue(queue);
    if (isvalid(value))
      queue->deallocator(value);
  }

  hash_table__set(self->hash, key, queue__enqueue(self->queue, value, key));
}

// ----------------------------------------------------------------------------
void
lru_cache__destroy(lru_cache_t *self) {
  if (!isvalid(self))
    return;

  log_d(
    "LRU cache collisions: %d/%d (%0.2f%%, prob: %0.2f%%)\n",
    _set_total - _set_empty,
    _set_total,
    (_set_total - _set_empty) * 100.0 / _set_total,
    100.0 * (1 - exp(-((double) self->queue->count) * (self->queue->count - 1.0) / 2.0 / self->hash->capacity))
  );

  queue__destroy(self->queue);
  hash_table__destroy(self->hash);

  free(self);
}
