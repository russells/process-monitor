#ifndef __xmalloc_h__
#define __xmalloc_h__

#include <stdlib.h>

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);

#endif
