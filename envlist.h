/* Keep a list of strings for environment setting. */

#ifndef __envlist_h__
#define __envlist_h__

/**
 * Contains a list of strings to set or unset in the environment.  We guarantee
 * that env[maxlen]==NULL.
 */
struct envlist {
	char **env;		/* Strings */
	size_t len;		/* The number we have */
	size_t maxlen;		/* Max size of env */
};

extern struct envlist *envlist_new();
extern void envlist_add(struct envlist *el, char *envvar);

#endif
