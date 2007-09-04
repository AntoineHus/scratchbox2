/*
 * Copyright (C) 2006,2007 Lauri Leukkunen <lle@rahina.org>
 *
 * Licensed under LGPL version 2.1, see top level LICENSE file for details.
 *
 * In the path translation functions we must call disable_mapping();
 * to avoid creating recursive loops of function calls due to wrapping.
 */

#define _GNU_SOURCE

#include <asm/unistd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _GNU_SOURCE
#undef _GNU_SOURCE
#include <string.h>
#include <libgen.h>
#define _GNU_SOURCE
#else
#include <string.h>
#include <libgen.h>
#endif

#include <limits.h>
#include <sys/param.h>
#include <sys/file.h>
#include <assert.h>
#include <pthread.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <mapping.h>
#include <sb2.h>

#define WRITE_LOG(fmt...) \
	{char *__logfile = getenv("SBOX_MAPPING_LOGFILE"); \
	int __logfd; FILE *__logfs;\
	if (__logfile) { \
		if ((__logfd = open(__logfile, O_APPEND | O_RDWR | O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)) > 0) { \
			__logfs = fdopen(__logfd, "a"); \
			fprintf(__logfs, fmt); \
			fclose(__logfs); \
		} \
	}}


#define enable_mapping() mapping_disabled--
#define disable_mapping() mapping_disabled++

#define __set_errno(e) errno = e

pidfunction *sb_getpid=getpid;

void bind_set_getpid(pidfunction *func) {
	sb_getpid=func;
}


void mapping_log_write(char *msg);
static int lua_bind_sb_functions(lua_State *l);

/* Lua interpreter */
__thread lua_State *l = NULL;

__thread char *rsdir = NULL;
__thread char *main_lua = NULL;
__thread int mapping_disabled = 0;
__thread time_t sb2_timestamp = 0;

__thread pthread_mutex_t lua_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

pthread_mutex_t mapping_cache_lock = PTHREAD_MUTEX_INITIALIZER;

struct path_entry {
	struct path_entry *prev;
	struct path_entry *next;
	char name[PATH_MAX];
};

char *decolonize_path(const char *path)
{
	char *cpath, *index, *start;
	char cwd[PATH_MAX];
	struct path_entry list;
	struct path_entry *work;
	struct path_entry *new;
	char *buf = NULL;

	if (!path) {
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
		if (!getcwd(cwd, PATH_MAX)) {
			perror("error getting current work dir\n");
			return NULL;
		}
		unsigned int l = (strlen(cwd) + 1 + strlen(path) + 1);
		cpath = malloc((strlen(cwd) + 1
					+ strlen(path) + 1) * sizeof(char));
		if (!cpath)
			abort();

		memset(cpath, '\0', l);
		strcpy(cpath, cwd);
		strcat(cpath, "/");
		strcat(cpath, path);
	} else {
		if (!(cpath = strdup(path)))
			abort();
	}

	start = cpath + 1;          /* ignore leading '/' */
	while (1) {
		unsigned int last = 0;

		index = strstr(start, "/");
		if (!index) {
			last = 1;
		} else {
			*index = '\0';
		}

		if (index == start) {
			goto proceed;       /* skip over empty strings 
					       resulting from // */
		}

		if (strcmp(start, "..") == 0) {
			/* travel up one */
			if (!work->prev)
				goto proceed;
			work = work->prev;
			free(work->next);
			work->next = NULL;
		} else if (strcmp(start, ".") == 0) {
			/* ignore */
			goto proceed;
		} else {
			/* add an entry to our path_entry list */
			if (!(new = malloc(sizeof(struct path_entry))))
				abort();
			memset(new->name, '\0', PATH_MAX);
			new->prev = work;
			work->next = new;
			new->next = NULL;
			strcpy(new->name, start);
			work = new;
		}

proceed:
		if (last)
			break;
		*index = '/';
		start = index + 1;
	}

	work = list.next;
	while (work) {
		struct path_entry *tmp;
		strcat(buf, "/");
		strcat(buf, work->name);
		tmp = work;
		work = work->next;
		free(tmp);
	}
	return buf;
}


#ifndef DISABLE_CACHE

static char *create_sb2cache_path(const char *mapping_mode, 
			const char *binary_name, 
			const char *func_name, 
			const char *path)
{
	char *target_dir = getenv("SBOX_MAPPING_CACHE");
	char *cache_path;
	unsigned int length;

	length = strlen(mapping_mode) + 1 + strlen(target_dir) + strlen(path) 
		+ 1 + strlen(binary_name) + 1 + strlen(func_name) + 4 + 1;

	cache_path = malloc(length * sizeof(char));
	memset(cache_path, '\0', length);
	strcpy(cache_path, target_dir);
	strcat(cache_path, path);
	strcat(cache_path, ".");
	strcat(cache_path, mapping_mode);
	strcat(cache_path, ".");
	strcat(cache_path, binary_name);
	strcat(cache_path, ".");
	strcat(cache_path, func_name);
	strcat(cache_path, ".map");
	return cache_path;
}

/*
 * Return NULL if not found from cache, or if the cache is outdated 
 * compared to sb2_timestamp.
 */
static char *read_sb2cache(const char *mapping_mode, 
			const char *binary_name, 
			const char *func_name, 
			const char *path)
{
	char *link_path = NULL;
	struct stat64 s;

	if (getenv("SBOX_DISABLE_MAPPING_CACHE")) return NULL;

	char *cache_path = create_sb2cache_path(mapping_mode, binary_name, func_name, path);

	if (lstat64(cache_path, &s) < 0 ||
		(s.st_mtime < get_sb2_timestamp())) {
		goto exit;
	}

	link_path = malloc((PATH_MAX + 1) * sizeof(char));
	memset(link_path, '\0', PATH_MAX + 1);


	if (readlink(cache_path, link_path, PATH_MAX) < 0) {
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

	/* TODO: check the redir_scripts/preload contents as well */

exit:
	enable_mapping();
	sb2_timestamp = t;
	return sb2_timestamp;
}

/*
 * If the cache dir doesn't exist, this will create it.
 */
static int insert_sb2cache(const char *mapping_mode,
			const char *binary_name, 
			const char *func_name, 
			const char *path, 
			const char *map_to)
{
	char *cache_path;
	char *dcopy;
	char *wrk;
	struct stat64 s;
	int lockfd;

	disable_mapping();

	/* get a lock on the cache
	 * first get a lock within this process
	 */
	pthread_mutex_lock(&mapping_cache_lock);
	lockfd = open(getenv("SBOX_MAPPING_CACHE"), O_RDONLY);
	while (flock(lockfd, LOCK_EX) < 0) {
		if (errno != EINTR) {
			perror("Unable to acquire cache lock");
			pthread_mutex_unlock(&mapping_cache_lock);
			close(lockfd);
			return -1;
		}
	}

	cache_path = create_sb2cache_path(mapping_mode, binary_name, 
					func_name, path);

	dcopy = strdup(cache_path);

	/* make sure all the directory elements exist, 
	 * this does nasty stuff in place to dcopy */
	wrk = dcopy;
	while (*wrk != '\0') {
		wrk = strstr(wrk + 1, "/");
		
		if (!wrk) break;

		*wrk = '\0';
		//DBGOUT("checking path: %s\n", dcopy);
#ifdef __x86_64__
		if (stat(dcopy, &s) < 0) {
#else
		if (stat64(dcopy, &s) < 0) {
#endif
			if (errno == ENOENT || errno == ENOTDIR) {
				/* create the dir */
				if (mkdir(dcopy, S_IRWXU) < 0) {
					perror("Unable to create dir in sb2cache\n");
					flock(lockfd, LOCK_UN);
					close(lockfd);
					pthread_mutex_unlock(&mapping_cache_lock);
					exit(1);
				}
			} else {
				perror("Big trouble working the sb2cache\n");
				flock(lockfd, LOCK_UN);
				close(lockfd);
				pthread_mutex_unlock(&mapping_cache_lock);
				exit(1);
			}
		}

		*wrk = '/';
		wrk++;
	}
	if (lstat64(cache_path, &s) == 0) {
		/* link exists, remove it */
		if (unlink(cache_path) < 0) {
			DBGOUT("unable to remove: %s\n", cache_path);
			perror("Error while removing symlink in sb2cache");
			flock(lockfd, LOCK_UN);
			close(lockfd);
			pthread_mutex_unlock(&mapping_cache_lock);
			exit(1);
		}
	}
	if (symlink(map_to, cache_path) < 0) {
		perror("Error while creating symlink in sb2cache\n");
		DBGOUT("Failed on: (%s, %s)\n", map_to, cache_path);
		flock(lockfd, LOCK_UN);
		close(lockfd);
		pthread_mutex_unlock(&mapping_cache_lock);
		exit(1);
	}
	flock(lockfd, LOCK_UN);
	close(lockfd);
	pthread_mutex_unlock(&mapping_cache_lock);
	enable_mapping();
	return 0;
}

#endif /* DISABLE_CACHE */


static int sb_decolonize_path(lua_State *l)
{
	int n;
	char *path;
	char *resolved_path;

	n = lua_gettop(l);
	if (n != 1) {
		lua_pushstring(l, NULL);
		return 1;
	}

	path = strdup(lua_tostring(l, 1));

	resolved_path = decolonize_path(path);
	lua_pushstring(l, resolved_path);
	free(resolved_path);
	free(path);
	return 1;
}

static int sb_readlink(lua_State *l)
{
	int n;
	char *path;
	char resolved_path[PATH_MAX + 1];

	n = lua_gettop(l);
	if (n != 1) {
		lua_pushstring(l, NULL);
		return 1;
	}

	memset(resolved_path, '\0', PATH_MAX + 1);

	path = strdup(lua_tostring(l, 1));
	if (readlink(path, resolved_path, PATH_MAX) < 0) {
		free(path);
		lua_pushstring(l, NULL);
		return 1;
	} else {
		free(path);
		lua_pushstring(l, resolved_path);
		return 1;
	}
}

/*
 * Check if file exists
 */
static int sb_file_exists(lua_State *l)
{
	struct stat s;
	char *path;
	int n;
	
	memset(&s, '\0', sizeof(struct stat64));

	n = lua_gettop(l);
	if (n != 1) {
		lua_pushstring(l, "sb_file_exists(path) - invalid number of parameters");
		return 1;
	}
	
	path = strdup(lua_tostring(l, 1));

	if (stat(path, &s) < 0) {
		/* failure, we assume it's because the target
		 * doesn't exist
		 */
		lua_pushnumber(l, 0);
		free(path);
		return 1;
	}
	
	free(path);
	lua_pushnumber(l, 1);
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

#ifdef __x86_64__
	if (lstat(path, &s) < 0) {
#else
	if (lstat64(path, &s) < 0) {
#endif
		/* didn't work
		 * TODO: error handling 
		 */
		//perror("stat failed\n");
		lua_pushstring(l, path);
		goto getout;
	}
	
	if (S_ISLNK(s.st_mode)) {
		/* we have a symlink, read it and return */
		readlink(path, link_path, PATH_MAX);
		lua_pushstring(l, link_path);
	} else {
		//printf("not a symlink! %s\n", path);
		/* not a symlink, return path */
		lua_pushstring(l, path);
	}

getout:	
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
	count = 1; /* Lua indexes tables from 1 */
	lua_newtable(l); /* create a new table on the stack */
	
	while ( (de = readdir(d)) != NULL) { /* get one dirent at a time */
		lua_pushnumber(l, count);
		lua_pushstring(l, de->d_name); /* push the entries to lua stack */
		lua_settable(l, -3);
		count++;
	}
	closedir(d);

	free(path);
	enable_mapping();
	return 1;
}

char *scratchbox_path(const char *func_name, const char *path)
{
	char binary_name[PATH_MAX+1];
	char *tmp;

	memset(binary_name, '\0', PATH_MAX+1);
	tmp = getenv("__SB2_BINARYNAME");
	if (tmp) {
		strcpy(binary_name, tmp);
	} else {
		strcpy(binary_name, "UNKNOWN");
	}
	return scratchbox_path2(binary_name, func_name, path);
}

char *scratchbox_path2(const char *binary_name,
		const char *func_name,
		const char *path)
{	
	char work_dir[PATH_MAX + 1];
	char *tmp = NULL, *decolon_path = NULL, *mapping_mode = NULL;
	char pidlink[17]; /* /proc/2^8/exe */

	if (!(mapping_mode = getenv("SBOX_MAPMODE"))) {
		mapping_mode = "simple";
	}

	if (!path) {
		WRITE_LOG("ERROR: scratchbox_path2: path == NULL: [%s][%s]\n", binary_name, func_name);
		return NULL;
	}

	if (mapping_disabled || getenv("SBOX_DISABLE_MAPPING")) {
		return strdup(path);
	}

	disable_mapping();
	decolon_path = decolonize_path(path);

	/* first try from the cache */

#ifndef DISABLE_CACHE
	tmp = NULL;
	if (rsdir && main_lua) tmp = read_sb2cache(mapping_mode,
						binary_name, 
						func_name, decolon_path);

	if (tmp) {
		if (strcmp(tmp, decolon_path) == 0) {
			free(decolon_path);
			enable_mapping();
			return strdup(path);
		} else {
			enable_mapping();
			return tmp;
		}
	}
#endif

	if (pthread_mutex_trylock(&lua_lock) < 0) {
		pthread_mutex_lock(&lua_lock);
	}

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
	getcwd(work_dir, PATH_MAX);

	/* redir_scripts RECURSIVE CALL BREAK */
	if (strncmp(decolon_path, rsdir, strlen(rsdir)) == 0) {
		pthread_mutex_unlock(&lua_lock);
		enable_mapping();
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
	lua_pushstring(l, mapping_mode);
	lua_pushstring(l, binary_name);
	lua_pushstring(l, func_name);
	lua_pushstring(l, work_dir);
	lua_pushstring(l, decolon_path);
	lua_call(l, 5, 1); /* five arguments, one result */

	tmp = (char *)lua_tostring(l, -1);
	if (tmp) {
		tmp = strdup(tmp);
	}

	lua_pop(l, 1);

	pthread_mutex_unlock(&lua_lock);

#ifndef DISABLE_CACHE
	insert_sb2cache(mapping_mode, binary_name, func_name, decolon_path, tmp);
#endif
	
	if (strcmp(tmp, decolon_path) == 0) {
		free(decolon_path);
		free(tmp);
		enable_mapping();
		return strdup(path);
	} else {
		free(decolon_path);
		enable_mapping();
		return tmp;
	}
}


/* mappings from c to lua */
static const luaL_reg reg[] =
{
	{"sb_getdirlisting",		sb_getdirlisting},
	{"sb_followsymlink",		sb_followsymlink},
	{"sb_readlink",			sb_readlink},
	{"sb_decolonize_path",		sb_decolonize_path},
	{"sb_file_exists",		sb_file_exists},
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
