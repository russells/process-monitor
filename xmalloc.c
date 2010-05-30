#include "xmalloc.h"
#include "log.h"

#include <string.h>
#include <errno.h>


void *xmalloc(size_t size)
{
	void *p;

	p = malloc(size);
	if (!p && !size)
		return NULL;
	if (! size)
		return p;
	if (! p) {
		logparent(CM_ERROR, "cannot allocate memory: %s\n",
			  strerror(errno));
		exit(5);
	}
	return p;
}


void *xrealloc(void *ptr, size_t size)
{
	void *p;

	if (! ptr)
		return xmalloc(size);
	if (! size) {
		free(ptr);
		return NULL;
	}
	p = realloc(ptr, size);
	if (!p) {
		logparent(CM_ERROR, "cannot (re)allocate memory: %s\n",
			  strerror(errno));
		exit(5);
	}
	return p;
}
