/*
 * libsb2 -- scratchbox2 preload library
 *
 * Copyright (C) 2006,2007 Lauri Leukkunen <lle@rahina.org>
 * parts contributed by 
 * 	Riku Voipio <riku.voipio@movial.com>
 *	Toni Timonen <toni.timonen@movial.com>
 *	Lauri T. Aarnio
 * 
 * Heavily based on the libfakechroot library by 
 * Piotr Roszatycki <dexter@debian.org>
 */

/*
    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/

#include "libsb2.h"
#include "exported.h"


/* strchrnul(): Find the first occurrence of C in S or the final NUL byte.
 * This is not present on all systems, so we'll use our own version in sb2.
*/
static const char *sb2_strchrnul (const char *s, int c_in)
{
	const char	*ptr = strchr(s, c_in);

	if(!ptr) {
		/* this is exactly how strchrnul() performs if c_in was not
		 * found (see the manual page on modern Linuxes...): */
		ptr = s + strlen(s);
	}
	return(ptr);
}


static int (*next_execve) (const char *filename, char *const argv [],
			char *const envp[]) = NULL;

int sb_next_execve(const char *file, char *const *argv, char *const *envp)
{
	if (next_execve == NULL) {
		next_execve = sbox_find_next_symbol(1, "execve");
	}

	return next_execve(file, argv, envp);
}


/* collect exec arguments from a varargs list to an array.
 * returns an allocated array (use free() to free it if exec fails)
*/
static char **va_exec_args_to_argv(
	const char *realfnname, 
	const char *arg0, 
	va_list args,
	char ***envpp)	/* execlp needs to get envp, it is after the NULL.. */
{
	char *next_arg;
	char **argv = NULL;
	int  n_elem;	/* number of elements in argv array, including the
			 * final NULL pointer */

	/* first we'll need a small array for arg0 and a NULL: */
	n_elem = 2;
	argv = malloc (n_elem * sizeof(char*));
	argv[0] = (char*)arg0;
	argv[1] = NULL;

	SB_LOG(SB_LOGLEVEL_NOISE, "%s/varargs: 0=%s", realfnname, arg0);

	/* if there are any additional arguments, add them to argv
	 * calling realloc() every time (depending on what king of allocator
	 * is in use this might or might not be very efficient, but this 
	 * strategy keeps the code simple AND we can be sure that the 
	 * reallocation really works, unlike if this would reallocate 
	 * only after every 1024 elements or so... and after all, this 
	 * is used for exec-class functions, so this won't be executed 
	 * too often anyway => efficiency is probably not our primary concern)
	*/
	next_arg = va_arg (args, char *);
	while(next_arg) {
		n_elem++;
		argv = realloc (argv, n_elem * sizeof(char*));
		argv[n_elem - 2] = next_arg;
		argv[n_elem - 1] = NULL;

		SB_LOG(SB_LOGLEVEL_NOISE, "%s/varargs: %d=%s", 
			realfnname, n_elem-2, next_arg);

		next_arg = va_arg (args, char *);
	}
	/* next_arg==NULL now, get an optional envp if requested: */	
	if(envpp) {
		*envpp = va_arg (args, char **);
	}

	return(argv);
}


/* #include <unistd.h> */
int execl_gate(
	int (*real_execl_ptr)(const char *path, const char *arg, ...),
	const char *realfnname,
	const char *path,
	const char *arg,
	va_list args)
{
	char **argv;
	int ret;

	(void)real_execl_ptr; /* not used */

	argv = va_exec_args_to_argv(realfnname, arg, args, NULL);
	ret = execve_gate (NULL, realfnname, path, (char *const *) argv, 
		environ);
	free(argv);
	return(ret);
}


/* #include <unistd.h> */
int execle_gate(
	int (*real_execle_ptr)(const char *path, const char *arg, ...),
	const char *realfnname,
	const char *path,
	const char *arg,
	va_list args)
{
	char **argv;
	int ret;
	char **envp;

	(void)real_execle_ptr; /* not used */

	argv = va_exec_args_to_argv(realfnname, arg, args, &envp);
	ret = execve_gate (NULL, realfnname, path, (char *const *) argv, 
		(char *const *) envp);
	free(argv);
	return(ret);
}

/* Execute FILE, searching in the `PATH' environment variable if
   it contains no slashes, with all arguments after FILE until a
   NULL pointer and environment from `environ'.  */
int execlp_gate(
	int (*real_execlp_ptr)(const char *file, const char *arg, ...),
	const char *realfnname,
	const char *file,
	const char *arg,
	va_list args)
{
	char **argv;
	int ret;

	(void)real_execlp_ptr;	/* not used */

	argv = va_exec_args_to_argv(realfnname, arg, args, NULL);
	ret = execvp_gate (NULL, realfnname, file, (char *const *) argv);
	free(argv);
	return(ret);
}



/* #include <unistd.h> */
int execv_gate(
	int (*real_execv_ptr)(const char *path, char *const argv []),
	const char *realfnname,
	const char *path,
	char *const argv [])
{
	(void)real_execv_ptr;	/* not used */

	return execve_gate (NULL, realfnname, path, argv, environ);
}


/* #include <unistd.h> */
int execve_gate(
	int (*real_execve_ptr)(const char *filename, char *const argv [],
		char *const envp[]),
	const char *realfnname,
	const char *filename,
	char *const argv [],
	char *const envp[])
{
	SBOX_MAP_PROLOGUE();
	char *hb_sbox_path = NULL;
	int ret;
	int file;
	char hashbang[SBOX_MAXPATH];
	unsigned int argc;
	const char **newargv;
	char interp_filename[SBOX_MAXPATH];
	char *ptr, **p;
	int k;
	unsigned int i, j, n;
	char c;

	(void)real_execve_ptr;	/* not used */

	sbox_path = scratchbox_path(realfnname, filename);

	/* First we need to find out if the file can be executed at all: */
	if(access_nomap_nolog(sbox_path, X_OK) < 0) {
		/* can't execute it. Possible errno codes from access() 
		 * are all possible from execve(), too, so there is no
		 * need to convert errno.
		*/
		SB_LOG(SB_LOGLEVEL_DEBUG, "%s: no X permission for '%s'",
			realfnname, sbox_path);
		return(-1);
	}

	/* Now we know that the file exists and has execute permission set.
	 *
	 * FIXME: Next this will try to open it and determine if it is a script.
	 * I think that this code should be moved to sb_exec.c and merged
	 * with the binary type detection code that we currently have there
	 * (in function inspect_binary()).
	 *
	 * In any case this *must* not fail if open() fails, because it
	 * is perfectly valid to have files with e.g. "--x--x--x" permissions.
	 * If the binary mode can not be determined, there are several 
	 * possible strategies:
	 * - Currently this assumes that the program is executable by the
	 *   host system (since binary detection in do_exec() will fail to 
	 *   open it, etc).
	 *   of course, host's exec fails badly if it is a target binary.
	 * - another possibility would be to try to set the "r" permission
	 *   (either momentarily or permanently) if this process is running
	 *   as the owner of the file. Obviously, this would present lots
	 *   of "interesting" problems. (one possibility is to enable
	 *   this strategy conditionally, for example if an environment
	 *   variable is set etc). anyway, this has not been implemented.
	 * - third possible solution would be to manually handle this kind
	 *   of exceptions by lua scripts.
	 * Unfortunately there is no perfect solution for this: The kernel
	 * can have a look inside read-protected files, but this runs in
	 * userspace...
	*/
	if ((file = open_nomap_nolog(sbox_path, O_RDONLY)) == -1) {
		/* Failed to open, so we really can't know what it is.
		 * do_exec() will perform default actions, which 
		 * typically means calling host's execve() for it.
		*/
		SB_LOG(SB_LOGLEVEL_WARNING, 
			"%s failed to determine type of '%s' (open failed)",
			realfnname, sbox_path);

		ret = do_exec(realfnname, filename, sbox_path, argv, envp);
		if (sbox_path) free(sbox_path);
		return ret;
	}

	/* Execution permitted and the file can be read. 
	 * See if it begins with #!xxx, i.e. is a script for "xxx"
	*/
	k = read(file, hashbang, SBOX_MAXPATH-2);
	close(file);
	if (k == -1) {
		errno = ENOENT;
		if (sbox_path) free(sbox_path);
		return -1;
	}

	if (hashbang[0] != '#' || hashbang[1] != '!') {
		/* not a script. do_exec() will find out what type of
		 * binary it is. */
		ret = do_exec(realfnname, filename, sbox_path, argv, envp);
		if (sbox_path) free(sbox_path);
		return ret;
	}

	/* if we're here we have a script */

	argc = 0;
	for (p = (char **)argv; *p; p++)
		argc++;

	/* extra element for hashbang argument */
	newargv = alloca((argc + 2) * sizeof (const char *));

	//printf("hashbang: %s\n", hashbang);
	for (i = j = 2; (hashbang[i] == ' ' || hashbang[i] == '\t') && i < SBOX_MAXPATH; i++, j++) {
		//printf("looping\n");
	}

	//printf("hashbanging: i=%u\n",i);
	//hashbang[i] = hashbang[i+1] = 0;

	for (n = 0; i < SBOX_MAXPATH; i++) {
		c = hashbang[i];
		if (hashbang[i] == 0 || hashbang[i] == ' ' || hashbang[i] == '\t' || hashbang[i] == '\n') {
			hashbang[i] = 0;
			if (i > j) {
				if (n == 0) {
					ptr = &hashbang[j];
					//printf("hashbanging ptr, sbox_path: %s, %s\n", ptr, sbox_path);
					strcpy(interp_filename, ptr);
					newargv[n++] = strdup(ptr);
				} else {
					newargv[n++] = strdup(&hashbang[j]);
				}
			}
			j = i + 1;
		}
		if (c == '\n' || c == 0) break;
	}

	hb_sbox_path = scratchbox_path(realfnname, interp_filename);
	//printf("hashbanging: %s, %s\n", interp_filename, hb_sbox_path);
	newargv[n++] = filename; /* the unmapped script path */

	for (i = 1; argv[i] != NULL && i < argc; ) {
		newargv[n++] = argv[i++];
	}

	newargv[n] = NULL;

	SB_LOG(SB_LOGLEVEL_DEBUG, "exec script, interp=%s",
		interp_filename);

	ret = do_exec(realfnname, interp_filename, hb_sbox_path, 
		(char *const *)newargv, envp);
	if (hb_sbox_path) free(hb_sbox_path);
	if (sbox_path) free(sbox_path);
	return ret;
}


/* #include <unistd.h> */
int execvp_gate(
	int (*real_execvp_ptr)(const char *file, char *const argv []),
	const char *realfnname,
	const char *file,
	char *const argv [])
{
	(void)real_execvp_ptr;	/* not used */

	if (*file == '\0') {
		/* We check the simple case first. */
		errno = ENOENT;
		return -1;
	}

	if (strchr (file, '/') != NULL) {
		/* Don't search when it contains a slash.  */
		return execve_gate (NULL, realfnname, file, argv, environ);
	} else {
		int got_eacces = 0;
		const char *p;
		const char *path;
		char *name;
		size_t len;
		size_t pathlen;

		path = getenv ("PATH");
		if (path) path = strdup(path);
		if (path == NULL) {
			/* There is no `PATH' in the environment.
			   The default search path is the current directory
			   followed by the path `confstr' returns for `_CS_PATH'.  */
			char *new_path;
			len = confstr (_CS_PATH, (char *) NULL, 0);
			new_path = (char *) alloca (1 + len);
			new_path[0] = ':';
			(void) confstr (_CS_PATH, new_path + 1, len);
			path = new_path;
		}

		len = strlen (file) + 1;
		pathlen = strlen (path);
		name = alloca (pathlen + len + 1);
		/* Copy the file name at the top.  */
		name = (char *) memcpy (name + pathlen + 1, file, len);
		/* And add the slash.  */
		*--name = '/';

		p = path;
		do {
			char *startp;

			path = p;
			p = sb2_strchrnul (path, ':');

			if (p == path) {
				/* Two adjacent colons, or a colon at the beginning or the end
				   of `PATH' means to search the current directory.  */
				startp = name + 1;
			} else {
				startp = (char *) memcpy (name - (p - path), path, p - path);
			}

			/* Try to execute this name.  If it works, execv will not return.  */
			execve_gate (NULL, realfnname, startp, argv, environ);

			switch (errno) {
				case EACCES:
					/* Record the we got a `Permission denied' error.  If we end
					   up finding no executable we can use, we want to diagnose
					   that we did find one but were denied access.  */
					got_eacces = 1;
				case ENOENT:
				case ESTALE:
				case ENOTDIR:
					/* Those errors indicate the file is missing or not executable
					   by us, in which case we want to just try the next path
					   directory.  */
					break;

				default:
					/* Some other error means we found an executable file, but
					   something went wrong executing it; return the error to our
					   caller.  */
					return -1;
			}
		} while (*p++ != '\0');

		/* We tried every element and none of them worked.  */
		if (got_eacces)
			/* At least one failure was due to permissions, so report that
			   error.  */
			errno = EACCES;
	}

	/* Return the error from the last attempt (probably ENOENT).  */
	return -1;
}


#ifdef HAVE_FTS_H
/* FIXME: why there was #if !defined(HAVE___OPENDIR2) around fts_open() ???? */
FTS * fts_open_gate(FTS * (*real_fts_open_ptr)(char * const *path_argv,
		int options, int (*compar)(const FTSENT **,const FTSENT **)),
	const char *realfnname,
	char * const *path_argv,
	int options,
	int (*compar)(const FTSENT **,const FTSENT **))
{
	SBOX_MAP_PROLOGUE();
	char *path;
	char * const *p;
	char **new_path_argv;
	char **np;
	int n;

	for (n=0, p=path_argv; *p; n++, p++);
	if ((new_path_argv = malloc(n*(sizeof(char *)))) == NULL) {
		return NULL;
	}

	for (n=0, p=path_argv, np=new_path_argv; *p; n++, p++, np++) {
		path = *p;
		sbox_path = scratchbox_path(realfnname, path);
		*np = sbox_path;
	}

	return (*real_fts_open_ptr)(new_path_argv, options, compar);
}
#endif

char * get_current_dir_name_gate(
	char * (*real_get_current_dir_name_ptr)(void),
	const char *realfnname)
{
	SBOX_MAP_PROLOGUE();
	char *cwd;

	if ((cwd = (*real_get_current_dir_name_ptr)()) == NULL) {
		return NULL;
	}
	if (*cwd != '\0') {
		sbox_path = scratchbox_path(realfnname, cwd);
	}
	free(cwd);
	return sbox_path;
}


/* #include <unistd.h> */
char *getcwd_gate (
	char *(*real_getcwd_ptr)(char *buf, size_t size),
	const char *realfnname,
	char *buf,
	size_t size)
{
	char *sbox_path = NULL;
	char *cwd;

	if ((cwd = (*real_getcwd_ptr)(buf, size)) == NULL) {
		return NULL;
	}
	if (*cwd != '\0') {
		sbox_path = scratchbox_path(realfnname, cwd);
	}
	if (sbox_path) {
		if(buf) {
			strncpy(buf, sbox_path, size);
			free(sbox_path);
		} else {
			/* buf==NULL: real getcwd() used malloc() to allocate cwd */
			free(cwd);
			cwd = sbox_path;
		}
	}
	return cwd;
}


char * getwd_gate(
	char *(*real_getwd_ptr)(char *buf),
	const char *realfnname,
	char *buf)
{
	SBOX_MAP_PROLOGUE();
	char *cwd;

	if ((cwd = (*real_getwd_ptr)(buf)) == NULL) {
		return NULL;
	}
	if (*cwd != '\0') {
		sbox_path = scratchbox_path(realfnname, cwd);
	}
	if (sbox_path) {
		if(buf) {
			strcpy(buf, sbox_path);
			free(sbox_path);
		} else {
			/* buf==NULL: next_getwd used malloc() to allocate cwd */
			free(cwd);
			cwd = sbox_path;
		}
	}
	return cwd;
}


int glob_gate(
	int (*real_glob_ptr)(const char *pattern, int flags,
		int (*errfunc) (const char *,int), glob_t *pglob),
	const char *realfnname,
	const char *pattern, /* has been mapped */
	int flags,
	int (*errfunc) (const char *,int),
	glob_t *pglob)
{
	int rc;
	unsigned int i;
	char tmp[SBOX_MAXPATH];

	rc = (*real_glob_ptr)(pattern, flags, errfunc, pglob);
	
	if (rc < 0) return rc;

	for(i = 0; i < pglob->gl_pathc; i++) {
		char	*sbox_path = NULL;

		strcpy(tmp,pglob->gl_pathv[i]);
		sbox_path = scratchbox_path(realfnname, tmp);
		strcpy(pglob->gl_pathv[i], sbox_path);
		if (sbox_path) free(sbox_path);
	}
	return rc;
}


int glob64_gate(
	int (*real_glob64_ptr)(const char *pattern,
		int flags, int (*errfunc) (const char *,int), glob64_t *pglob),
	const char *realfnname,
	const char *pattern, /* has been mapped */
	int flags,
	int (*errfunc) (const char *,int),
	glob64_t *pglob)
{
	int rc;
	unsigned int i;
	char tmp[SBOX_MAXPATH];

	rc = (*real_glob64_ptr)(pattern, flags, errfunc, pglob);

	if (rc < 0) return rc;

	for(i = 0; i < pglob->gl_pathc; i++) {
		char	*sbox_path = NULL;

		strcpy(tmp,pglob->gl_pathv[i]);
		sbox_path = scratchbox_path(realfnname, tmp);
		strcpy(pglob->gl_pathv[i], sbox_path);
		if (sbox_path) free(sbox_path);
	}
	return rc;
}


/* FIXME: The following two functions do not have anything to do with path
 * remapping. Instead these implementations prevent locking of the shadow
 * file, which I find really hard to understand. Please explain
 * why we should have these wrappers, or should these be removed completely?
 * (this same comment is in interface.master, so hopefully somebody
 * will be able to explain this someday!)
*/
/* #include <shadow.h> */
int lckpwdf (void)
{
	return 0;
}
int ulckpwdf (void)
{
	return 0;
}


char *mkdtemp_gate(
	char *(*real_mkdtemp_ptr)(char *template),
	const char *realfnname,
	char *template)
{
	(void)realfnname;	/* not used here */

	if ((*real_mkdtemp_ptr)(template) == NULL) {
		return NULL;
	}
	return template;
}


int uname_gate(
	int (*real_uname_ptr)(struct utsname *buf),
	const char *realfnname,
	struct utsname *buf)
{
	(void)realfnname;	/* not used here */

	if ((*real_uname_ptr)(buf) < 0) {
		return -1;
	}
	/* this may be called before environ is properly setup */
	if (environ) {
		char *uname_machine = getenv("SBOX_UNAME_MACHINE");

		if(uname_machine)
			snprintf(buf->machine, sizeof(buf->machine),
					"%s", uname_machine);
	}
	return 0;
}

/* ---------- */

void *sbox_find_next_symbol(int log_enabled, const char *fn_name)
{
	char	*msg;
	void	*fn_ptr;

	if(log_enabled)
		SB_LOG(SB_LOGLEVEL_DEBUG, "%s: %s", __func__, fn_name);

	fn_ptr = dlsym(RTLD_NEXT, fn_name);
	if ((msg = dlerror()) != NULL) {
		fprintf(stderr, "%s: dlsym(%s): %s\n",
			PACKAGE_NAME, fn_name, msg);
		if(log_enabled)
			SB_LOG(SB_LOGLEVEL_ERROR, "ERROR: %s: dlsym(%s): %s",
				PACKAGE_NAME, fn_name, msg);
		assert(0);
	}
	return(fn_ptr);
}
