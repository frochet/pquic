#include "michelfralloc.h"
#include "sbrk-public.h"

#include <stdio.h>
#include <stdint.h>

/**
 * This is a wrapper around dlmalloc to allow it to allocate memory inside a fixed area
 */

/*
  malloc(size_t n)
  Returns a pointer to a newly allocated chunk of at least n bytes, or
  null if no space is available, in which case errno is set to ENOMEM
  on ANSI C systems.

  If n is zero, malloc returns a minimum-sized chunk. (The minimum
  size is 16 bytes on most 32bit systems, and 32 bytes on 64bit
  systems.)  Note that size_t is an unsigned type, so calls with
  arguments that would be negative if signed are interpreted as
  requests for huge amounts of space, which will often fail. The
  maximum supported value of n differs across systems, but is in all
  cases less than the maximum representable value of a size_t.
*/
void* dlmalloc(size_t);

/*
  free(void* p)
  Releases the chunk of memory pointed to by p, that had been previously
  allocated using malloc or a related routine such as realloc.
  It has no effect if p is null. If p was not malloced or already
  freed, free(p) will by default cuase the current program to abort.
*/
void  dlfree(void*);


/*
  realloc(void* p, size_t n)
  Returns a pointer to a chunk of size n that contains the same data
  as does chunk p up to the minimum of (n, p's size) bytes, or null
  if no space is available.

  The returned pointer may or may not be the same as p. The algorithm
  prefers extending p in most cases when possible, otherwise it
  employs the equivalent of a malloc-copy-free sequence.

  If p is null, realloc is equivalent to malloc.

  If space is not available, realloc returns null, errno is set (if on
  ANSI) and p is NOT freed.

  if n is for fewer bytes than already held by p, the newly unused
  space is lopped off and freed if possible.  realloc with a size
  argument of zero (re)allocates a minimum-sized chunk.

  The old unix realloc convention of allowing the last-free'd chunk
  to be used as an argument to realloc is not supported.
*/

void* dlrealloc(void*, size_t);


void *michelfralloc(plugin_dynamic_memory_pool_t *context, size_t size) {
    set_current_context(context);
    return dlmalloc(size);
}

void michelfree(plugin_dynamic_memory_pool_t *context, void *pointer) {
    set_current_context(context);
    return dlfree(pointer);
}

void *michelfrealloc(plugin_dynamic_memory_pool_t *context, void *pointer, size_t size) {
    set_current_context(context);
    return dlrealloc(pointer, size);
}
