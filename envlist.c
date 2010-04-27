
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "envlist.h"
#include "log.h"


struct envlist *envlist_new(void)
{
	struct envlist *envp;
	envp = malloc(sizeof(struct envlist));
	if (! envp) {
		return envp;
	}
	envp->env = malloc(sizeof(char *) * 10);
	envp->maxlen = 10;
	envp->len = 0;
	return envp;
}


void envlist_add(struct envlist *envp, char *envvar)
{
	if (! envp->env) {
		/* New array */
		envp->maxlen = 10;
		envp->env = malloc(sizeof(char*)*envp->maxlen);
		if (! envp->env) {
			logparent(CM_ERROR,
				  "cannot malloc() for env: %s\n",
				  strerror(errno));
			exit(2);
		}



	} else if (envp->len == envp->maxlen-2) {
		/* Extend the existing array */
		char **new_env;
		envp->maxlen += 10;
		new_env = realloc(envp->env, sizeof(char*)*envp->maxlen);
		if (! new_env) {
			logparent(CM_ERROR,
				  "cannot realloc() for env: %s\n",
				  strerror(errno));
			exit(2);
		}
	}
	envp->env[envp->len++] = envvar;
	envp->env[envp->len  ] = NULL;
}

