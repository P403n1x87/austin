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

#include "cache.h"
#include "logging.h"
#include "math.h"

// -- Queue -------------------------------------------------------------------

// ----------------------------------------------------------------------------
queue_item_t*
queue_item_new(value_t value, key_dt key) {
    queue_item_t* item = (queue_item_t*)calloc(1, sizeof(queue_item_t));
    if (!isvalid(item)) // GCOV_EXCL_LINE
        return NULL;    // GCOV_EXCL_LINE

    item->value = value;
    item->key   = key;

    return item;
}

// ----------------------------------------------------------------------------
void
queue_item__destroy(queue_item_t* self, void (*deallocator)(value_t)) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    deallocator(self->value);

    free(self);
}

// ----------------------------------------------------------------------------
queue_t*
queue_new(int capacity, void (*deallocator)(value_t)) {
    queue_t* queue = (queue_t*)calloc(1, sizeof(queue_t));
    if (!isvalid(queue)) // GCOV_EXCL_LINE
        return NULL;     // GCOV_EXCL_LINE

    queue->capacity    = capacity;
    queue->deallocator = deallocator;

    return queue;
}

// ----------------------------------------------------------------------------
bool
queue__is_full(queue_t* queue) {
    return queue->count == queue->capacity;
}

// ----------------------------------------------------------------------------
bool
queue__is_empty(queue_t* queue) {
    return queue->rear == NULL;
}

// ----------------------------------------------------------------------------
value_t
queue__dequeue(queue_t* queue) {
    if (queue__is_empty(queue))
        return NULL;

    if (queue->front == queue->rear)
        queue->front = NULL;

    queue_item_t* temp = queue->rear;
    queue->rear        = queue->rear->prev;

    if (queue->rear)
        queue->rear->next = NULL;

    void* value = temp->value;
    free(temp);

    queue->count--;

    return value;
}

// ----------------------------------------------------------------------------
queue_item_t*
queue__enqueue(queue_t* self, value_t value, key_dt key) {
    if (queue__is_full(self))
        return NULL;

    queue_item_t* temp = queue_item_new(value, key);
    temp->next         = self->front;

    if (queue__is_empty(self))
        self->rear = self->front = temp;
    else {
        self->front->prev = temp;
        self->front       = temp;
    }

    self->count++;

    return temp;
}

// ----------------------------------------------------------------------------
void
queue__destroy(queue_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    queue_item_t* next = NULL;
    for (queue_item_t* item = self->front; isvalid(item); item = next) {
        next = item->next;
        queue_item__destroy(item, self->deallocator);
    }

    free(self);
}

// -- Hash Table --------------------------------------------------------------

// ----------------------------------------------------------------------------
chain_t*
chain_new(key_dt key, value_t value) {
    chain_t* chain = (chain_t*)calloc(1, sizeof(chain_t));
    if (!isvalid(chain)) // GCOV_EXCL_LINE
        return NULL;     // GCOV_EXCL_LINE

    chain->key   = key;
    chain->value = value;

    return chain;
}

// ----------------------------------------------------------------------------
int
chain__add(chain_t* self, key_dt key, value_t value) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return 0;       // GCOV_EXCL_LINE

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
bool
chain__remove(chain_t* self, key_dt key) {
    if (!isvalid(self) || !isvalid(self->next)) // GCOV_EXCL_LINE
        return false;                           // GCOV_EXCL_LINE

    if (self->next->key == key) {
        chain_t* next = self->next;
        self->next    = next->next;
        next->next    = NULL;

        free(next);

        return true;
    }

    return chain__remove(self->next, key);
}

// ----------------------------------------------------------------------------
value_t
chain__find(chain_t* self, key_dt key) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return NULL;    // GCOV_EXCL_LINE

    if (self->key == key)
        return self->value;

    return chain__find(self->next, key);
}

// ----------------------------------------------------------------------------
bool
chain__has(chain_t* self, key_dt key) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return false;   // GCOV_EXCL_LINE

    if (self->key == key)
        return true;

    return chain__has(self->next, key);
}

// ----------------------------------------------------------------------------
void
chain__destroy(chain_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    chain__destroy(self->next);

    free(self);
}

// ----------------------------------------------------------------------------
hash_table_t*
hash_table_new(int capacity) {
    hash_table_t* hash = (hash_table_t*)calloc(1, sizeof(hash_table_t));
    if (!isvalid(hash)) // GCOV_EXCL_LINE
        return NULL;    // GCOV_EXCL_LINE

    hash->capacity    = capacity;
    hash->load_factor = 0.75 * capacity;
    hash->chains      = (chain_t**)calloc(hash->capacity, sizeof(chain_t*));

#ifdef DEBUG
    hash->set_total = 0;
    hash->set_empty = 0;
#endif

    return hash;
}

// ----------------------------------------------------------------------------
#define MAGIC 2654435761u

static inline index_t
_hash_table__index(hash_table_t* self, key_dt key) {
    return (uintptr_t)((key * MAGIC) % self->capacity);
}

// ----------------------------------------------------------------------------
value_t
hash_table__get(hash_table_t* self, key_dt key) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return NULL;    // GCOV_EXCL_LINE

    chain_t* chain = self->chains[_hash_table__index(self, key)];
    if (!isvalid(chain))
        return NULL;

    return chain__find(chain, key);
}

// ----------------------------------------------------------------------------
bool
hash_table__is_full(hash_table_t* self) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid hash table");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    return self->size >= self->load_factor;
}

// ----------------------------------------------------------------------------
void
hash_table__set(hash_table_t* self, key_dt key, value_t value) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    index_t index = _hash_table__index(self, key);

#ifdef DEBUG
    self->set_total++;
#endif

    chain_t* chain = self->chains[index];
    if (!isvalid(chain)) {
        if (self->size >= self->capacity)
            return;
#ifdef DEBUG
        self->set_empty++;
#endif
        self->chains[index]  = chain_head();
        self->size          += chain__add(self->chains[index], key, value);
        return;
    }

    if ((self->size >= self->capacity) && !chain__has(chain, key))
        return;

    self->size += chain__add(self->chains[index], key, value);
}

// ----------------------------------------------------------------------------
void
hash_table__del(hash_table_t* self, key_dt key) {
    if (!isvalid(self) || self->size == 0) // GCOV_EXCL_LINE
        return;                            // GCOV_EXCL_LINE

    index_t  index = _hash_table__index(self, key);
    chain_t* chain = self->chains[index];

    if (!isvalid(chain)) // GCOV_EXCL_LINE
        return;          // GCOV_EXCL_LINE

    self->size -= chain__remove(chain, key);

    if (chain__is_empty(chain)) {
        chain__destroy(chain);
        self->chains[index] = NULL;
    }
}

// ----------------------------------------------------------------------------
void
hash_table__destroy(hash_table_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    if (isvalid(self->chains)) {
        for (int i = 0; i < self->capacity; i++) {
            chain__destroy(self->chains[i]);
        }
        sfree(self->chains);
    }

    free(self);
}

// -- LRU Cache ---------------------------------------------------------------

// ----------------------------------------------------------------------------
lru_cache_t*
lru_cache_new(int capacity, void (*deallocator)(value_t)) {
    lru_cache_t* cache = (lru_cache_t*)calloc(1, sizeof(lru_cache_t));
    if (!isvalid(cache)) // GCOV_EXCL_LINE
        return NULL;     // GCOV_EXCL_LINE

    cache->capacity = capacity;

    capacity = capacity ? capacity : 1024;

    cache->queue = queue_new(capacity, deallocator);
    cache->hash  = hash_table_new((capacity * 4 / 3) | 1);

#ifdef DEBUG
    cache->hits   = 0;
    cache->misses = 0;
#endif

    return cache;
}

// ----------------------------------------------------------------------------
value_t
lru_cache__maybe_hit(lru_cache_t* self, key_dt key) {
    queue_item_t* item = (queue_item_t*)hash_table__get(self->hash, key);

    if (!isvalid(item)) {
#ifdef DEBUG
        self->misses++;
#endif
        return NULL;
    }

#ifdef DEBUG
    self->hits++;
#endif

    // Bring hit element to the front of the queue
    if (item != self->queue->front) {
        item->prev->next = item->next;
        if (item->next)
            item->next->prev = item->prev;

        if (item == self->queue->rear) {
            self->queue->rear       = item->prev;
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
bool
lru_cache__is_full(lru_cache_t* self) {
    return queue__is_full(self->queue);
}

// ----------------------------------------------------------------------------
void
lru_cache__store(lru_cache_t* self, key_dt key, value_t value) {
    queue_t* queue = self->queue;

    if (queue__is_full(queue)) {
        if (self->capacity == LRU_CACHE_EXPAND) {
            // Double the queue capacity.
            unsigned capacity = queue->capacity = (queue->capacity << 1);

            // Double the hash table and move the items across.
            hash_table_t* new_hash = hash_table_new((capacity * 4 / 3) | 1);

            hash_table__iter_start(self->hash, queue_item_t*, item) { hash_table__set(new_hash, item->key, item); }
            hash_table__iter_stop(self->hash);

            // Destroy the old hash table and replace it with the new one.
            hash_table__destroy(self->hash);
            self->hash = new_hash;
        } else {
            hash_table__del(self->hash, queue->rear->key);

            value_t value = queue__dequeue(queue);
            if (isvalid(value))
                queue->deallocator(value);
        }
    }

    hash_table__set(self->hash, key, queue__enqueue(self->queue, value, key));
}

// ----------------------------------------------------------------------------
void
lru_cache__invalidate(lru_cache_t* self) {
    if (!isvalid(self))
        return;

    unsigned queue_capacity            = self->queue->capacity;
    void (*queue_deallocator)(value_t) = self->queue->deallocator;
    unsigned hash_table_capacity       = self->hash->capacity;

    queue__destroy(self->queue);
    hash_table__destroy(self->hash);

    self->queue = queue_new(queue_capacity, queue_deallocator);
    self->hash  = hash_table_new(hash_table_capacity);
}

// ----------------------------------------------------------------------------
void
lru_cache__destroy(lru_cache_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

#ifdef DEBUG
    size_t total = self->hits + self->misses;
    log_d("(%s) hit ratio: %d/%d (%0.2f%%)\n", self->name, self->hits, total, (self->hits) * 100.0 / total);
    log_d(
        "(%s) hash collisions: %d/%d (%0.2f%%, prob: %0.2f%%)\n", self->name,
        self->hash->set_total - self->hash->set_empty, self->hash->set_total,
        (self->hash->set_total - self->hash->set_empty) * 100.0 / self->hash->set_total,
        100.0 * (1 - exp(-((double)self->queue->count) * (self->queue->count - 1.0) / 2.0 / self->hash->capacity))
    );
#endif

    queue__destroy(self->queue);
    hash_table__destroy(self->hash);

    free(self);
}

// -- Lookup ------------------------------------------------------------------

// ----------------------------------------------------------------------------
lookup_t*
lookup_new(int size) {
    lookup_t* lookup = (lookup_t*)calloc(1, sizeof(lookup_t));
    if (!isvalid(lookup)) // GCOV_EXCL_LINE
        return NULL;      // GCOV_EXCL_LINE

    lookup->hash = hash_table_new(size);

    return lookup;
}

// ----------------------------------------------------------------------------
value_t
lookup__get(lookup_t* self, key_dt key) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return NULL;    // GCOV_EXCL_LINE

    return hash_table__get(self->hash, key);
}

// ----------------------------------------------------------------------------
void
lookup__set(lookup_t* self, key_dt key, value_t value) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    if (hash_table__is_full(self->hash)) {
        // Double the hash table and move the items across.
        hash_table_t* new_hash = hash_table_new(self->hash->capacity << 1);

        hash_table__iteritems_start(self->hash, key_dt, _key, value_t, _value) {
            hash_table__set(new_hash, _key, _value);
        }
        hash_table__iter_stop(self->hash);

        // Destroy the old hash table and replace it with the new one.
        hash_table__destroy(self->hash);
        self->hash = new_hash;
    }

    hash_table__set(self->hash, key, value);
}

// ----------------------------------------------------------------------------
void
lookup__del(lookup_t* self, key_dt key) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    hash_table__del(self->hash, key);
}

// ----------------------------------------------------------------------------
void
lookup__clear(lookup_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    size_t size = self->hash->capacity;
    hash_table__destroy(self->hash);
    self->hash = hash_table_new(size);
}

// ----------------------------------------------------------------------------
void
lookup__destroy(lookup_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    hash_table__destroy(self->hash);
    self->hash = NULL;

    free(self);
}
