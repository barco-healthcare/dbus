/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-mempool.h Memory pools
 * 
 * Copyright (C) 2002  Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 1.2
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-mempool.h"

/**
 * @defgroup DBusMemPool memory pools
 * @ingroup  DBusInternals
 * @brief DBusMemPool object
 *
 * Types and functions related to DBusMemPool.  A memory pool is used
 * to decrease memory fragmentation/overhead and increase speed for
 * blocks of small uniformly-sized objects. The main point is to avoid
 * the overhead of a malloc block for each small object, speed is
 * secondary.
 */

/**
 * @defgroup DBusMemPoolInternals Memory pool implementation details
 * @ingroup  DBusInternals
 * @brief DBusMemPool implementation details
 *
 * The guts of DBusMemPool.
 *
 * @{
 */

/**
 * typedef so DBusFreedElement struct can refer to itself.
 */
typedef struct DBusFreedElement DBusFreedElement;

/**
 * struct representing an element on the free list.
 * We just cast freed elements to this so we can
 * make a list out of them.
 */
struct DBusFreedElement
{
  DBusFreedElement *next; /**< next element of the free list */
};

/**
 * The dummy size of the variable-length "elements"
 * field in DBusMemBlock
 */
#define ELEMENT_PADDING 4

/**
 * Typedef for DBusMemBlock so the struct can recursively
 * point to itself.
 */
typedef struct DBusMemBlock DBusMemBlock;

/**
 * DBusMemBlock object represents a single malloc()-returned
 * block that gets chunked up into objects in the memory pool.
 */
struct DBusMemBlock
{
  DBusMemBlock *next;  /**< next block in the list, which is already used up;
                        *   only saved so we can free all the blocks
                        *   when we free the mem pool.
                        */

  int used_so_far;     /**< bytes of this block already allocated as elements. */
  
  unsigned char elements[ELEMENT_PADDING]; /**< the block data, actually allocated to required size */
};

/**
 * Internals fields of DBusMemPool
 */
struct DBusMemPool
{
  int element_size;                /**< size of a single object in the pool */
  int block_size;                  /**< size of most recently allocated block */
  unsigned int zero_elements : 1;  /**< whether to zero-init allocated elements */

  DBusFreedElement *free_elements; /**< a free list of elements to recycle */
  DBusMemBlock *blocks;            /**< blocks of memory from malloc() */
};

/** @} */

/**
 * @addtogroup DBusMemPool
 *
 * @{
 */

/**
 * @typedef DBusMemPool
 *
 * Opaque object representing a memory pool. Memory pools allow
 * avoiding per-malloc-block memory overhead when allocating a lot of
 * small objects that are all the same size. They are slightly
 * faster than calling malloc() also.
 */

/**
 * Creates a new memory pool, or returns #NULL on failure.  Objects in
 * the pool must be at least sizeof(void*) bytes each, due to the way
 * memory pools work. To avoid creating 64 bit problems, this means at
 * least 8 bytes on all platforms, unless you are 4 bytes on 32-bit
 * and 8 bytes on 64-bit.
 *
 * @param element_size size of an element allocated from the pool.
 * @param zero_elements whether to zero-initialize elements
 * @returns the new pool or #NULL
 */
DBusMemPool*
_dbus_mem_pool_new (int element_size,
                    dbus_bool_t zero_elements)
{
  DBusMemPool *pool;

  pool = dbus_new0 (DBusMemPool, 1);
  if (pool == NULL)
    return NULL;

  /* these assertions are equivalent but the first is more clear
   * to programmers that see it fail.
   */
  _dbus_assert (element_size >= (int) sizeof (void*));
  _dbus_assert (element_size >= (int) sizeof (DBusFreedElement));
  
  pool->element_size = element_size;
  pool->zero_elements = zero_elements != FALSE;

  /* pick a size for the first block; it increases
   * for each block we need to allocate. This is
   * actually half the initial block size
   * since _dbus_mem_pool_alloc() unconditionally
   * doubles it prior to creating a new block.
   */
  pool->block_size = element_size * 8;

  _dbus_assert ((pool->block_size %
                 pool->element_size) == 0);
  
  return pool;
}

/**
 * Frees a memory pool (and all elements allocated from it).
 *
 * @param pool the memory pool.
 */
void
_dbus_mem_pool_free (DBusMemPool *pool)
{
  DBusMemBlock *block;

  block = pool->blocks;
  while (block != NULL)
    {
      DBusMemBlock *next = block->next;

      dbus_free (block);

      block = next;
    }

  dbus_free (pool);
}

/**
 * Allocates an object from the memory pool.
 * The object must be freed with _dbus_mem_pool_dealloc().
 *
 * @param pool the memory pool
 * @returns the allocated object or #NULL if no memory.
 */
void*
_dbus_mem_pool_alloc (DBusMemPool *pool)
{
  if (pool->free_elements)
    {
      DBusFreedElement *element = pool->free_elements;

      pool->free_elements = pool->free_elements->next;

      if (pool->zero_elements)
        memset (element, '\0', pool->element_size);
      
      return element;
    }
  else
    {
      void *element;
      
      if (pool->blocks == NULL ||
          pool->blocks->used_so_far == pool->block_size)
        {
          /* Need a new block */
          DBusMemBlock *block;
          int alloc_size;

          if (pool->block_size <= _DBUS_INT_MAX / 4) /* avoid overflow */
            {
              /* use a larger block size for our next block */
              pool->block_size *= 2;
              _dbus_assert ((pool->block_size %
                             pool->element_size) == 0);
            }

          alloc_size = sizeof (DBusMemBlock) - ELEMENT_PADDING + pool->block_size;
          
          if (pool->zero_elements)
            block = dbus_malloc0 (alloc_size);
          else
            block = dbus_malloc (alloc_size);

          if (block == NULL)
            return NULL;

          block->used_so_far = 0;
          block->next = pool->blocks;
          pool->blocks = block;
        }
      
      element = &pool->blocks->elements[pool->blocks->used_so_far];

      pool->blocks->used_so_far += pool->element_size;

      return element;
    }
}

/**
 * Deallocates an object previously created with
 * _dbus_mem_pool_alloc(). The previous object
 * must have come from this same pool.
 * @param pool the memory pool
 * @param element the element earlier allocated.
 */
void
_dbus_mem_pool_dealloc (DBusMemPool *pool,
                        void        *element)
{
  DBusFreedElement *freed;

  freed = element;
  freed->next = pool->free_elements;
  pool->free_elements = freed;
}

/** @} */

#ifdef DBUS_BUILD_TESTS
#include "dbus-test.h"
#include <stdio.h>
#include <time.h>

static void
time_for_size (int size)
{
  int i;
  int j;
  clock_t start;
  clock_t end;
#define FREE_ARRAY_SIZE 512
#define N_ITERATIONS FREE_ARRAY_SIZE * 512
  void *to_free[FREE_ARRAY_SIZE];
  DBusMemPool *pool;

  printf ("Timings for size %d\n", size);
  
  printf (" malloc\n");
  
  start = clock ();
  
  i = 0;
  j = 0;
  while (i < N_ITERATIONS)
    {
      to_free[j] = dbus_malloc (size);
      _dbus_assert (to_free[j] != NULL); /* in a real app of course this is wrong */

      ++j;

      if (j == FREE_ARRAY_SIZE)
        {
          j = 0;
          while (j < FREE_ARRAY_SIZE)
            {
              dbus_free (to_free[j]);
              ++j;
            }

          j = 0;
        }
      
      ++i;
    }

  end = clock ();

  printf ("  created/destroyed %d elements in %g seconds\n",
          N_ITERATIONS, (end - start) / (double) CLOCKS_PER_SEC);



  printf (" mempools\n");
  
  start = clock ();

  pool = _dbus_mem_pool_new (size, FALSE);
  
  i = 0;
  j = 0;
  while (i < N_ITERATIONS)
    {
      to_free[j] = _dbus_mem_pool_alloc (pool); 
      _dbus_assert (to_free[j] != NULL);  /* in a real app of course this is wrong */

      ++j;

      if (j == FREE_ARRAY_SIZE)
        {
          j = 0;
          while (j < FREE_ARRAY_SIZE)
            {
              _dbus_mem_pool_dealloc (pool, to_free[j]);
              ++j;
            }

          j = 0;
        }
      
      ++i;
    }

  _dbus_mem_pool_free (pool);
  
  end = clock ();

  printf ("  created/destroyed %d elements in %g seconds\n",
          N_ITERATIONS, (end - start) / (double) CLOCKS_PER_SEC);

  printf (" zeroed malloc\n");
    
  start = clock ();
  
  i = 0;
  j = 0;
  while (i < N_ITERATIONS)
    {
      to_free[j] = dbus_malloc0 (size);
      _dbus_assert (to_free[j] != NULL); /* in a real app of course this is wrong */

      ++j;

      if (j == FREE_ARRAY_SIZE)
        {
          j = 0;
          while (j < FREE_ARRAY_SIZE)
            {
              dbus_free (to_free[j]);
              ++j;
            }

          j = 0;
        }
      
      ++i;
    }

  end = clock ();

  printf ("  created/destroyed %d elements in %g seconds\n",
          N_ITERATIONS, (end - start) / (double) CLOCKS_PER_SEC);
  
  printf (" zeroed mempools\n");
  
  start = clock ();

  pool = _dbus_mem_pool_new (size, TRUE);
  
  i = 0;
  j = 0;
  while (i < N_ITERATIONS)
    {
      to_free[j] = _dbus_mem_pool_alloc (pool); 
      _dbus_assert (to_free[j] != NULL);  /* in a real app of course this is wrong */

      ++j;

      if (j == FREE_ARRAY_SIZE)
        {
          j = 0;
          while (j < FREE_ARRAY_SIZE)
            {
              _dbus_mem_pool_dealloc (pool, to_free[j]);
              ++j;
            }

          j = 0;
        }
      
      ++i;
    }

  _dbus_mem_pool_free (pool);
  
  end = clock ();

  printf ("  created/destroyed %d elements in %g seconds\n",
          N_ITERATIONS, (end - start) / (double) CLOCKS_PER_SEC);
}

/**
 * @ingroup DBusMemPoolInternals
 * Unit test for DBusMemPool
 * @returns #TRUE on success.
 */
dbus_bool_t
_dbus_mem_pool_test (void)
{
  int i;
  int element_sizes[] = { 4, 8, 16, 50, 124 };
  
  i = 0;
  while (i < _DBUS_N_ELEMENTS (element_sizes))
    {
      time_for_size (element_sizes[i]);
      ++i;
    }
  
  return TRUE;
}

#endif /* DBUS_BUILD_TESTS */
