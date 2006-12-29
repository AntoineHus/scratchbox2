/*
 * Copyright (C) 2006 Lauri Leukkunen <lleukkun@cc.hut.fi>
 *
 * Licensed under LGPL version 2, see top level LICENSE file for details.
 */




/*
 * In the path translation functions we must use syscalls directly to 
 * avoid creating recursive loops of function calls due to wrapping
 */

#define _GNU_SOURCE

#include <sys/syscall.h>
#include <asm/unistd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/param.h>
#include <assert.h>
#include <pthread.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lua_bindings.h"

#include <sb2.h>

#define enable_mapping() mapping_disabled--;
#define disable_mapping() mapping_disabled++;

#define __set_errno(e) errno = e

pidfunction *sb_getpid=getpid;

void bind_set_getpid(pidfunction *func) {
	sb_getpid=func;
}


static int lua_bind_sb_functions(lua_State *l);
char *__sb2_realpath (const char *name, char *resolved);

/* Lua interpreter */
__thread lua_State *l = NULL;

__thread char *rsdir = NULL;
__thread char *main_lua = NULL;
__thread int mapping_disabled = 0;
__thread time_t sb2_timestamp = 0;

__thread pthread_mutex_t lua_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

struct path_entry {
	struct path_entry *prev;
	struct path_entry *next;
	char name[PATH_MAX];
};

static char *decolonize_path(const char *path)
{
	char *cpath, *index, *start;
	char cwd[PATH_MAX];
	struct path_entry list;
	struct path_entry *work;
	struct path_entry *new;
	char *buf = NULL;
	
	if (!path) {
		DBGOUT("PATH IS CRAP\n");
		return NULL;
	}
	
	buf = malloc((PATH_MAX + 1) * sizeof(char));
	memset(buf, '\0', PATH_MAX + 1);

	list.next = NULL;
	list.prev = NULL;
	work = &list;

	if (path[0] != '/') {
		/* not an absolute path */
		memset(cwd, '\0', PATH_MAX);
		if (syscall(__NR_getcwd, cwd, PATH_MAX) < 0) {
			perror("error getting current work dir\n");
			return NULL;
		}
		unsigned int l = (strlen(cwd) + 1 + strlen(path) + 1);
		cpath = malloc((strlen(cwd) + 1 + strlen(path) + 1) * sizeof(char));
		memset(cpath, '\0', l);
		strcpy(cpath, cwd);
		strcat(cpath, "/");
		strcat(cpath, path);
	} else {
		cpath = strdup(path);
	}
	start = cpath + 1; /* ignore leading '/' */
	while (1) {
		index = strstr(start, "/");
		if (!index) {
			/* add the last item */
			new = malloc(sizeof(struct path_entry));
			memset(new->name, '\0', PATH_MAX);
			new->prev = work;
			work->next = new;
			new->next = NULL;
			strcpy(new->name, start);
			work = new;
			break;
		}
		*index = '\0';
		if (strcmp(start, "..") == 0) {
			/* travel up one */
			if (!work->prev) goto proceed;
			work = work->prev;
			free(work->next);
			work->next = NULL;
		} else if (strcmp(start, ".") == 0) {
			/* ignore */
			goto proceed;
		} else {
			/* add an entry to our path_entry list */
			new = malloc(sizeof(struct path_entry));
			memset(new->name, '\0', PATH_MAX);
			new->prev = work;
			work->next = new;
			new->next = NULL;
			strcpy(new->name, start);
			work = new;
		}

proceed:
		*index = '/';
		start = index + 1;
	}

	work = list.next;
	while (work) {
		struct path_entry *tmp;
		strcat(buf, "/");
		strcat(buf, work->name);
		//printf("entry name: %s\n", work->name);
		tmp = work;
		work = work->next;
		free(tmp);
	}
	return buf;
}


#ifndef DISABLE_CACHE

static char *create_sb2cache_path(const char *binary_name, const char *func_name, const char *path)
{
	char *target_dir = getenv("SBOX_TARGET_ROOT");
	char *cache_path;
	unsigned int length;

	length = strlen(target_dir) + strlen(".sb2cache") + strlen(path) + 1 + strlen(binary_name) + 1 + strlen(func_name) + 4 + 1;
	cache_path = malloc(length * sizeof(char));
	memset(cache_path, '\0', length);
	strcpy(cache_path, target_dir);
	strcat(cache_path, ".sb2cache");
	strcat(cache_path, path);
	strcat(cache_path, ".");
	strcat(cache_path, binary_name);
	strcat(cache_path, ".");
	strcat(cache_path, func_name);
	strcat(cache_path, ".map");
	return cache_path;
}

/*
 * return NULL if not found from cache, or if the cache is outdated compared to sb2_timestamp
 */
static char *read_sb2cache(const char *binary_name, const char *func_name, const char *path)
{
	char *link_path = NULL;
	struct stat64 s;
	char *cache_path = create_sb2cache_path(binary_name, func_name, path);

	if (lstat64(cache_path, &s) < 0 ||
		(s.st_mtime < get_sb2_timestamp())) {
		goto exit;
	}

	link_path = malloc((PATH_MAX + 1) * sizeof(char));
	memset(link_path, '\0', PATH_MAX + 1);


	if (syscall(__NR_readlink, cache_path, link_path, PATH_MAX) < 0) {
		free(link_path);
		link_path = NULL;
	}
exit:
	free(cache_path);
	return link_path;
}

time_t get_sb2_timestamp(void)
{
	DIR *d = NULL;
	struct dirent *de = NULL;
	struct stat64 s;
	time_t t;
	char *libsb2 = getenv("SBOX_LIBSB2");

	if (sb2_timestamp) return sb2_timestamp;
	disable_mapping();
	
	t = 0;
	if (stat64(libsb2, &s) < 0) goto exit;
	t = s.st_mtime;

	if (stat64(main_lua, &s) < 0) goto exit;
	if (t < s.st_mtime) t = s.st_mtime;

	if ((d = opendir(rsdir)) == NULL) goto exit;
	while ((de = readdir(d)) != NULL) {
		if (stat64(de->d_name, &s) < 0) continue;
		if (t < s.st_mtime) t = s.st_mtime;
	}
	closedir(d);

exit:
	enable_mapping();
	sb2_timestamp = t;
	return sb2_timestamp;
}

/*
 * if the cache dir doesn't exist, this will create it
 */
static int insert_sb2cache(const char *binary_name, const char *func_name, const char *path, const char *map_to)
{
	char *cache_path;
	char *dcopy;
	char *wrk;
	struct stat64 s;

	disable_mapping();

	cache_path = create_sb2cache_path(binary_name, func_name, path);

	dcopy = strdup(cache_path);

	/* make sure all the directory elements exist, 
	 * this does nasty stuff in place to dcopy */
	wrk = dcopy;
	while (*wrk != '\0') {
		wrk = strstr(wrk + 1, "/");
		
		if (!wrk) break;

		*wrk = '\0';
		//DBGOUT("checking path: %s\n", dcopy);
		if (syscall(__NR_stat64, dcopy, &s) < 0) {
			if (errno == ENOENT || errno == ENOTDIR) {
				/* create the dir */
				if (syscall(__NR_mkdir, dcopy, S_IRWXU) < 0) {
					perror("Unable to create dir in sb2cache\n");
					exit(1);
				}
			} else {
				perror("Big trouble working the sb2cache\n");
				exit(1);
			}
		}

		*wrk = '/';
		wrk++;
	}
	if (lstat64(cache_path, &s) == 0) {
		/* link exists, remove it */
		if (unlink(cache_path) < 0) {
			perror("Error while removing symlink in sb2cache");
			exit(1);
		}
	}
	if (symlink(map_to, cache_path) < 0) {
		perror("Error while creating symlink in sb2cache\n");
		exit(1);
	}
	enable_mapping();
	return 0;
}

#endif /* DISABLE_CACHE */

static int sb_realpath(lua_State *l)
{
	char *path;
	char *resolved_path = NULL;
	int n;

	n = lua_gettop(l);
	if (n != 1) {
		lua_pushstring(l, "sb_realpath(path) - invalid number of parameters");
		return 1;
	}

	path = strdup(lua_tostring(l, 1));

	//printf("sb_realpath: [%s]\n", path);

	/* Here we rely on glibc specific feature of passing NULL as the second
	 * parameter to realpath, thus forcing it to allocate a sufficient buffer.
	 * This might not work at all on other systems.
	 */

	resolved_path = __sb2_realpath(path, NULL);
	if (resolved_path == NULL)  {
		lua_pushstring(l, "no such file");
		return 1;
	}

	//printf("sb_realpath: resolved_path = [%s]\n", resolved_path);

	lua_pushstring(l, resolved_path);
	return 1;
}


/*
 * check if the path is a symlink, if yes then return it resolved,
 * if not, return the path intact
 */

static int sb_followsymlink(lua_State *l)
{
	char *path;
	struct stat64 s;
	char link_path[PATH_MAX + 1];
	int n;

	/* printf("in sb_followsymlink\n"); */
	memset(&s, '\0', sizeof(struct stat64));
	memset(link_path, '\0', PATH_MAX + 1);

	n = lua_gettop(l);
	if (n != 1) {
		lua_pushstring(l, "sb_followsymlink(path) - invalid number of parameters");
		return 1;
	}

	path = strdup(lua_tostring(l, 1));

	//printf("C thinks path is: %s\n", path);
	
	if (syscall(__NR_lstat64, path, &s) < 0) {
		/* didn't work
		 * TODO: error handling 
		 */
		//perror("stat failed\n");
		lua_pushstring(l, path);
		goto getout;
	}
	//printf("about to test for symlink: %i\n", s.st_mode);
	if (S_ISLNK(s.st_mode)) {
		/* we have a symlink, read it and return */
		//printf("WE HAVE A SYMLINK!!!\n");
		syscall(__NR_readlink, path, link_path, PATH_MAX);
		lua_pushstring(l, link_path);

	} else {
		//printf("not a symlink! %s\n", path);
		/* not a symlink, return path */
		lua_pushstring(l, path);
		//printf("after pushing\n");
	}
getout:	
	//printf("about to free!\n");
	free(path);
	return 1;
}

/*
 * This function should ONLY look at things from rsdir
 * any other path leads to loops
 */

static int sb_getdirlisting(lua_State *l)
{
	DIR *d;
	struct dirent *de;
	char *path;
	int count;

	int n = lua_gettop(l);
	
	if (n != 1) {
		lua_pushstring(l, NULL);
		return 1;
	}

	path = strdup(lua_tostring(l, 1));
	if (strncmp(path, rsdir, strlen(rsdir)) != 0) {
		/* invalid path used */
		lua_pushstring(l, NULL);
		return 1;
	}

	disable_mapping();

	if ( (d = opendir(path)) == NULL ) {
		lua_pushstring(l, NULL);
		enable_mapping();
		return 1;
	}
	count = 0;
	lua_newtable(l); /* create a new table on the stack */
	
	while ( (de = readdir(d)) != NULL) { /* get one dirent at a time */
		lua_pushnumber(l, count);
		lua_pushstring(l, de->d_name); /* push the entries to lua stack */
		lua_settable(l, -3);
		count++;
	}
	closedir(d);

	/* make sure lua knows about the table size */
	lua_pushliteral(l, "n");
	lua_pushnumber(l, count - 1);
	lua_rawset(l, -3);
	free(path);
	enable_mapping();
	return 1;
}


char *scratchbox_path(const char *func_name, const char *path)
{	
	char binary_name[PATH_MAX+1];
	char work_dir[PATH_MAX+1];
	char *tmp, *decolon_path;
	char pidlink[17]; /* /proc/2^8/exe */

	if (!path) return NULL;
	if (mapping_disabled || getenv("SBOX_DISABLE_MAPPING")) {
		return strdup(path);
	}

	decolon_path = decolonize_path(path);

	if (strstr(decolon_path, getenv("SBOX_TARGET_ROOT"))) {
		/* short circuit a direct reference to a file inside the sbox 
		 * target dir */
		//DBGOUT("about to short circuit: %s\n", func_name);
		free(decolon_path);
		return strdup(path);
	}

	memset(binary_name, '\0', PATH_MAX+1);
	tmp = getenv("__SB2_BINARYNAME");
	if (tmp) {
		strcpy(binary_name, tmp);
	} else {
		strcpy(binary_name, "DUMMY");
	}

	/* first try from the cache */

#ifndef DISABLE_CACHE
	tmp = read_sb2cache(binary_name, func_name, decolon_path);

	if (tmp) {
		if (strcmp(tmp, decolon_path) == 0) {
			free(decolon_path);
			return strdup(path);
		} else {
			return tmp;
		}
	}
#endif

#if 1
	if (pthread_mutex_trylock(&lua_lock) < 0) {
		pthread_mutex_lock(&lua_lock);
	}
#endif

	if (!rsdir) {
		rsdir = getenv("SBOX_REDIR_SCRIPTS");
		if (!rsdir) {
			rsdir = "/scratchbox/redir_scripts";
		} else {
			rsdir = strdup(rsdir);
		}
		
		main_lua = calloc(strlen(rsdir) + strlen("/main.lua") + 1, sizeof(char));

		strcpy(main_lua, rsdir);
		strcat(main_lua, "/main.lua");
	}
	
	memset(work_dir, '\0', PATH_MAX+1);
	snprintf(pidlink,16,"/proc/%i/exe",sb_getpid());
//	if (syscall(__NR_readlink, pidlink, binary_name, PATH_MAX) < 0) {
//		perror("__NR_readlink() error, check that /proc is mounted!");
//	}
	syscall(__NR_getcwd, work_dir, PATH_MAX);

	/* redir_scripts RECURSIVE CALL BREAK */
	if (strncmp(decolon_path, rsdir, strlen(rsdir)) == 0) {
		//pthread_mutex_unlock(&lua_lock);
		//DBGOUT("cutting recursive call\n");
		return (char *)decolon_path;
	}
	
	if (!l) {
		l = luaL_newstate();
		
		luaL_openlibs(l);
		lua_bind_sb_functions(l); /* register our sb_ functions */
		switch(luaL_loadfile(l, main_lua)) {
		case LUA_ERRFILE:
			fprintf(stderr, "Error loading %s\n", main_lua);
			exit(1);
		case LUA_ERRSYNTAX:
			fprintf(stderr, "Syntax error in %s\n", main_lua);
			exit(1);
		case LUA_ERRMEM:
			fprintf(stderr, "Memory allocation error while loading %s\n", main_lua);
			exit(1);
		default:
			;
		}
		lua_call(l, 0, 0);
	}

	lua_getfield(l, LUA_GLOBALSINDEX, "sbox_translate_path");
	lua_pushstring(l, binary_name);
	lua_pushstring(l, func_name);
	lua_pushstring(l, work_dir);
	lua_pushstring(l, decolon_path);
	lua_call(l, 4, 1); /* four arguments, one result */
	tmp = (char *)lua_tostring(l, -1);
	if (tmp) {
		tmp = strdup(tmp);
	}
	lua_pop(l, 1);

	pthread_mutex_unlock(&lua_lock);
#ifndef DISABLE_CACHE
	insert_sb2cache(binary_name, func_name, decolon_path, tmp);
#endif
	//DBGOUT("returning path: [%s]\n", tmp);
	if (strcmp(tmp, decolon_path) == 0) {
		free(decolon_path);
		free(tmp);
		return strdup(path);
	} else {
		free(decolon_path);
		return tmp;
	}
}


/* mappings from c to lua */
static const luaL_reg reg[] =
{
	{"sb_getdirlisting",		sb_getdirlisting},
	{"sb_followsymlink",		sb_followsymlink},
	{"sb_realpath",			sb_realpath},
	{NULL,				NULL}
};


static int lua_bind_sb_functions(lua_State *l)
{
	luaL_register(l, "sb", reg);
	lua_pushliteral(l,"version");
	lua_pushstring(l, "2.0" );
	lua_settable(l,-3);

	return 0;
}


/*
 * __sb_realpath is based directly on the __realpath in glibc-2.4.
 * The only difference is that this uses syscalls directly and 
 * replaces all __functions with their public names.
 */

/* Return the canonical absolute name of file NAME.  A canonical name
   does not contain any `.', `..' components nor any repeated path
   separators ('/') or symlinks.  All path components must exist.  If
   RESOLVED is null, the result is malloc'd; otherwise, if the
   canonical name is PATH_MAX chars or more, returns null with `errno'
   set to ENAMETOOLONG; if the name fits in fewer than PATH_MAX chars,
   returns the name in RESOLVED.  If the name cannot be resolved and
   RESOLVED is non-NULL, it contains the path of the first component
   that cannot be resolved.  If the path can be resolved, RESOLVED
   holds the same value as the value returned.  */

char *
__sb2_realpath (const char *name, char *resolved)
{
  char *rpath, *dest, *extra_buf = NULL;
  const char *start, *end, *rpath_limit;
  long int path_max;
  int num_links = 0;

  if (name == NULL)
    {
      /* As per Single Unix Specification V2 we must return an error if
	 either parameter is a null pointer.  We extend this to allow
	 the RESOLVED parameter to be NULL in case the we are expected to
	 allocate the room for the return value.  */
      __set_errno (EINVAL);
      return NULL;
    }

  if (name[0] == '\0')
    {
      /* As per Single Unix Specification V2 we must return an error if
	 the name argument points to an empty string.  */
      __set_errno (ENOENT);
      return NULL;
    }

#ifdef PATH_MAX
  path_max = PATH_MAX;
#else
  path_max = pathconf (name, _PC_PATH_MAX);
  if (path_max <= 0)
    path_max = 1024;
#endif

  if (resolved == NULL)
    {
      rpath = malloc (path_max);
      if (rpath == NULL)
	return NULL;
    }
  else
    rpath = resolved;
  rpath_limit = rpath + path_max;

  if (name[0] != '/')
    {
      if (!syscall(__NR_getcwd, rpath, path_max))
	{
	  rpath[0] = '\0';
	  goto error;
	}
      dest = strchr (rpath, '\0');
    }
  else
    {
      rpath[0] = '/';
      dest = rpath + 1;
    }

  for (start = end = name; *start; start = end)
    {
      struct stat64 st;
      int n;

      /* Skip sequence of multiple path-separators.  */
      while (*start == '/')
	++start;

      /* Find end of path component.  */
      for (end = start; *end && *end != '/'; ++end)
	/* Nothing.  */;

      if (end - start == 0)
	break;
      else if (end - start == 1 && start[0] == '.')
	/* nothing */;
      else if (end - start == 2 && start[0] == '.' && start[1] == '.')
	{
	  /* Back up to previous component, ignore if at root already.  */
	  if (dest > rpath + 1)
	    while ((--dest)[-1] != '/');
	}
      else
	{
	  size_t new_size;

	  if (dest[-1] != '/')
	    *dest++ = '/';

	  if (dest + (end - start) >= rpath_limit)
	    {
	      ptrdiff_t dest_offset = dest - rpath;
	      char *new_rpath;

	      if (resolved)
		{
		  __set_errno (ENAMETOOLONG);
		  if (dest > rpath + 1)
		    dest--;
		  *dest = '\0';
		  goto error;
		}
	      new_size = rpath_limit - rpath;
	      if (end - start + 1 > path_max)
		new_size += end - start + 1;
	      else
		new_size += path_max;
	      new_rpath = (char *) realloc (rpath, new_size);
	      if (new_rpath == NULL)
		goto error;
	      rpath = new_rpath;
	      rpath_limit = rpath + new_size;

	      dest = rpath + dest_offset;
	    }

	  dest = mempcpy (dest, start, end - start);
	  *dest = '\0';

	  if (syscall(__NR_lstat64, rpath, &st) < 0)
	    goto error;

	  if (S_ISLNK (st.st_mode))
	    {
	      char *buf = alloca (path_max);
	      size_t len;

	      if (++num_links > MAXSYMLINKS)
		{
		  __set_errno (ELOOP);
		  goto error;
		}

	      n = syscall(__NR_readlink, rpath, buf, path_max);
	      if (n < 0)
		goto error;
	      buf[n] = '\0';

	      if (!extra_buf)
		extra_buf = alloca (path_max);

	      len = strlen (end);
	      if ((long int) (n + len) >= path_max)
		{
		  __set_errno (ENAMETOOLONG);
		  goto error;
		}

	      /* Careful here, end may be a pointer into extra_buf... */
	      memmove (&extra_buf[n], end, len + 1);
	      name = end = memcpy (extra_buf, buf, n);

	      if (buf[0] == '/')
		dest = rpath + 1;	/* It's an absolute symlink */
	      else
		/* Back up to previous component, ignore if at root already: */
		if (dest > rpath + 1)
		  while ((--dest)[-1] != '/');
	    }
	  else if (!S_ISDIR (st.st_mode) && *end != '\0')
	    {
	      __set_errno (ENOTDIR);
	      goto error;
	    }
	}
    }
  if (dest > rpath + 1 && dest[-1] == '/')
    --dest;
  *dest = '\0';

  assert (resolved == NULL || resolved == rpath);
  return rpath;

error:
  assert (resolved == NULL || resolved == rpath);
  if (resolved == NULL)
    free (rpath);
  return NULL;
}

