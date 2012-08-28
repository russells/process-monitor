
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "envlist.h"
#include "log.h"
#include "xmalloc.h"


struct envlist *envlist_new(void)
{
	struct envlist *envp;
	envp = xmalloc(sizeof(struct envlist));
	if (! envp) {
		return envp;
	}
	envp->env = xmalloc(sizeof(char *) * 10);
	envp->maxlen = 10;
	envp->len = 0;
	return envp;
}


void envlist_add(struct envlist *envp, char *envvar)
{
	if (! envp->env) {
		/* New array */
		envp->maxlen = 10;
		envp->env = xmalloc(sizeof(char*)*envp->maxlen);

	} else if (envp->len == envp->maxlen-2) {
		/* Extend the existing array */
		char **new_env;
		envp->maxlen += 10;
		new_env = xrealloc(envp->env, sizeof(char*)*envp->maxlen);
		envp->env = new_env;
	}
	envp->env[envp->len++] = envvar;
	envp->env[envp->len  ] = NULL;
}

