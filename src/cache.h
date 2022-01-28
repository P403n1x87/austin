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

#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>

#include "hints.h"

typedef uintptr_t key_dt;
typedef void * value_t;


// -- Queue -------------------------------------------------------------------

typedef struct queue_item_t {
    struct queue_item_t *prev, *next;
    key_dt key;
    value_t value; // Takes ownership of a free-able object
} queue_item_t;

typedef struct queue_t {
    unsigned count;
    unsigned capacity;
    queue_item_t *front, *rear;
    void (*deallocator)(value_t);
} queue_t;


/**
 * Create a new queue item.
 * 
 * Note that the newly created item is meant to take ownership of the value.
 * 
 * @param value_t  The value to add.
 * @param key_dt   An optional integer key that identifies the value.
 * 
 * @return a reference to a valid queue item, NULL otherwise.
 */
queue_item_t *
queue_item_new(value_t, key_dt);


/**
 * Destroy a queue item.
 * 
 * Since the queue item has ownership of the value, a destructor needs to be
 * passed in order to deallocate the owned value.
 * 
 * @param self         the queue item
 * @param deallocator  the deallocator
 */
void
queue_item__destroy(queue_item_t *, void (*)(value_t));


/**
 * Create a new queue object.
 * 
 * Any element added to the queue will be owned by the queue. This means that,
 * when the queue is destroyed, all the elements in it are also destroyed,
 * according to the given deallocator.
 * 
 * @param capacity     the queue maximum capacity
 * @param deallocator  the queue item value deallocator
 * 
 * @return a valid reference to a queue object, NULL otherwise.
 */
queue_t *
queue_new(int, void (*)(value_t));


/**
 * Check if the queue is full.
 * 
 * @param self  the queue
 * 
 * @return TRUE if the queue is full, else FALSE.
 */
int
queue__is_full(queue_t *);


/**
 * Check if the queue is empty.
 * 
 * @param self  the queue
 * 
 * @return TRUE if the queue is empty, else FALSE.
 */
int
queue__is_empty(queue_t *);


/**
 * Remove the first element in the queue.
 * 
 * @param self  the queue
 * 
 * @return the value stored in the queue item (if any), else NULL.
 */
value_t
queue__dequeue(queue_t *);


/**
 * Add an element to the queue.
 * 
 * @param self   the queue
 * @param value  the value to add
 * @param key    optional key to associate to the element
 * 
 * @return a reference to the queue item, if the queue was not full, else NULL.
 */
queue_item_t *
queue__enqueue(queue_t *, value_t, key_dt);


/**
 * Destroy the queue.
 * 
 * @param self  the queue
 */
void
queue__destroy(queue_t *);


// -- Hash Table --------------------------------------------------------------

typedef unsigned int index_t;

typedef struct _chain_t {
    struct _chain_t *next;
    key_dt key;
    value_t value;
} chain_t;

typedef struct hash_table_t {
    size_t capacity;
    size_t size;
    chain_t **chains;
} hash_table_t;


/**
 * Create a new chain.
 * 
 * This is an implementation of a linked list that used to implement chaining
 * for resolving collisions in a hash table and therefore should be regarded
 * as an internal detail implementation of the hash_table_t structure.
 * 
 * A chain is an element in the list *and* the list itself. This is why this
 * constructor takes a key and a value.
 * 
 * NOTE: Ideally, a chain should be started with a sentinel element. The
 * chain_head is a convenience macro to create chain heads.
 * 
 * @param key    the item key
 * @param value  the item value
 * 
 * @return a valid reference to a chain, NULL otherwise.
 */
chain_t *
chain_new(key_dt, value_t);


/**
 * Create a chain head item.
 * 
 * @return a valid reference to a chain head, NULL otherwise.
 */
#define chain_head()           (chain_new(0, NULL))


/**
 * Check if a chain is empty.
 * 
 * Under the assumption that each chain starts with a chain head, the check
 * involves the next element in the chain.
 * 
 * @param self  the chain.
 * 
 * @return TRUE if the chain is empty (i.e. there is no next element), FALSE
 *         otherwise.
 */
#define chain__is_empty(chain) (!isvalid(chain->next))


/**
 * Add a new item to the chain.
 * 
 * @param self   the chain to add to
 * @param key    the key for the new item
 * @param value  the value for the new item
 */
int
chain__add(chain_t *, key_dt, value_t);


/**
 * Remove an element from a chain given its key.
 * 
 * @param self  the chain to remove from
 * @param key   the key to match
 * 
 * @return 1 if a chain item was removed, 0 otherwise.
 */
int
chain__remove(chain_t *, key_dt);


/**
 * Find a value in the chain given its key.
 * 
 * NOTE: If you want to check whether a chain has an element or not, the right
 *       method to use is chain__has since NULL is a valid chain item value.
 * 
 * @param self  the chain to search
 * @param key   the key to matck
 * 
 * @return the value for the key if found, NULL otherwise.
 */
value_t
chain__find(chain_t *, key_dt);


/**
 * Check whether a chain has an item with the given key.
 * 
 * @param self  the chain to check
 * @param key   the key to match.
 * 
 * @return TRUE if the chain has an item with the given key, FALSE otherwise.
 */
int
chain__has(chain_t *, key_dt);


/**
 * Deallocate a chain.
 * 
 * @param self  the chain to deallocate.
 */
void
chain__destroy(chain_t *);


/**
 * Create a new hash table.
 * 
 * @param capacity  the hash table maximum capacity
 * 
 * @return a valid reference to a new hash table, NULL otherwise.
 */
hash_table_t *
hash_table_new(int);


/**
 * Get from the hash table.
 * 
 * NOTE: This method cannot be used to determine whether a hash table has a
 *       given key, unless all the items have a non-NULL value.
 * 
 * @param self  the hash table
 * @param key   the key to match
 * 
 * @return the value stored with the given key, NULL otherwise.
 */
value_t
hash_table__get(hash_table_t *, key_dt);


/**
 * Set a value into the table.
 * 
 * If the key is not already present and the table is full, this method does
 * nothing.
 * 
 * @param self   the hash table to set into
 * @param key    the key to set at
 * @param value  the value to set
 */
void
hash_table__set(hash_table_t *, key_dt, value_t);


#define hash_table__iter_start(table)                                          \
    for (int i = 0; i < table->capacity; i++) {                                \
        chain_t * chain = table->chains[i];                                    \
        if (!isvalid(chain))                                                   \
            continue;                                                          \
        while (isvalid(chain->next)) {                                         \
            chain = chain->next;                                               \
            value_t value = chain->value;                                      \
      

#define hash_table__iter_stop(table) }}


/**
 * Remove a value from the hash table.
 * 
 * @param self  the hash table to remove from
 * @param key   the key to remove
 */
void
hash_table__del(hash_table_t *, key_dt);


/**
 * Deallocate a hash table.
 * 
 * @param self  the hash table to deallocate
 */
void
hash_table__destroy(hash_table_t *);


// -- LRU Cache ---------------------------------------------------------------

typedef struct {
    int capacity;
    queue_t *queue;
    hash_table_t *hash;
} lru_cache_t;


/**
 * Create an LRU cache.
 * 
 * Internally, this makes use of a queue which takes ownership of the values
 * added to it. Therefore, the cache itself takes ownership of every value that
 * is stored within it.
 * 
 * @param capacity     the cache capacity
 * @param deallocator  the value deallocator
 * 
 * @return a valid reference to a cache, NULL otherwise.
 */
lru_cache_t *
lru_cache_new(int, void (*)(value_t));


/**
 * Try to hit the cache.
 * 
 * Since this method returns NULL on a cache miss, this only makes sense if all
 * the objects stored within the cache are non-NULL.
 * 
 * @param self  the cache to hit
 * @param key   the key to search
 * 
 * @return the value associated to the key if a hit occurred, NULL otherwise.
 */
value_t
lru_cache__maybe_hit(lru_cache_t *, key_dt);


/**
 * Store a value within the cache at the given key. If the cache is full, the
 * least recently used key/value pair is evicted.
 * 
 * @param self   the cache to store into
 * @param key    the key at which to store the value
 * @param value  the value to store
 */
void
lru_cache__store(lru_cache_t *, key_dt, value_t);


/**
 * Deallocate a cache.
 * 
 * @param self  the cache to deallocate
 */
void
lru_cache__destroy(lru_cache_t *);

#endif