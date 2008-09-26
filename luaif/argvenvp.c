/*
 * Copyright (C) 2006,2007 Lauri Leukkunen <lle@rahina.org>
 *
 * Licensed under LGPL version 2.1, see top level LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>

#include <sb2.h>
#include <mapping.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

static char *argvenvp_mode;

/* Convert a vector of strings to a lua table, leaves that table to
 * lua's stack.
*/
static void strvec_to_lua_table(struct lua_instance *luaif, char **args)
{
	char	**p;
	int	i;

	lua_newtable(luaif->lua);
	for (p = args, i = 1; *p; p++, i++) {
		lua_pushnumber(luaif->lua, i);
		lua_pushstring(luaif->lua, *p);
		lua_settable(luaif->lua, -3);
	}
}

static void strvec_free(char **args)
{
	char **p;

	for (p = args; *p; p++) {
		free(*p);
	}
	free(args);
}


/* convert a lua table (table of strings) to a string vector,
 * the vector will be dynamically allocated.
*/
static void lua_string_table_to_strvec(struct lua_instance *luaif,
	int lua_stack_offs, char ***args, int new_argc)
{
	int	i;

	*args = calloc(new_argc + 1, sizeof(char *));

	for (i = 0; i < new_argc; i++) {
		lua_rawgeti(luaif->lua, lua_stack_offs, i + 1);
		(*args)[i] = strdup(lua_tostring(luaif->lua, -1));
		lua_pop(luaif->lua, 1); /* return stack state to what it
					 * was before lua_rawgeti() */
	}
	(*args)[i] = NULL;
}

void sb_push_string_to_lua_stack(char *str) 
{
	struct lua_instance *luaif = get_lua();

	if (luaif) lua_pushstring(luaif->lua, str);
}

/* Exec preprocessor:
 * (previously known as "sb_execve_mod")
*/
int sb_execve_preprocess(char **file, char ***argv, char ***envp)
{
	struct lua_instance *luaif = get_lua();
	char **p;
	int i, res, new_argc, new_envc;

	if (!luaif) return(0);

	if (!(argvenvp_mode = getenv("SBOX_ARGVENVP_MODE"))) {
		argvenvp_mode = "simple";
	}

	if (!argv || !envp) {
		SB_LOG(SB_LOGLEVEL_ERROR,
			"ERROR: sb_argvenvp: (argv || envp) == NULL");
		return -1;
	}

	if (getenv("SBOX_DISABLE_ARGVENVP")) {
		SB_LOG(SB_LOGLEVEL_DEBUG, "sb_argvenvp disabled(E):");
		return 0;
	}

	SB_LOG(SB_LOGLEVEL_NOISE,
		"sb_execve_preprocess: gettop=%d", lua_gettop(luaif->lua));

	lua_getfield(luaif->lua, LUA_GLOBALSINDEX, "sbox_execve_preprocess");
	lua_pushstring(luaif->lua, *file);
	free(*file);

	strvec_to_lua_table(luaif, *argv);
	strvec_free(*argv);

	strvec_to_lua_table(luaif, *envp);
	strvec_free(*envp);

	/* args:    binaryname, argv, envp
	 * returns: err, file, argc, argv, envc, envp */
	lua_call(luaif->lua, 3, 6);
	
	res = lua_tointeger(luaif->lua, -6);
	*file = strdup(lua_tostring(luaif->lua, -5));
	new_argc = lua_tointeger(luaif->lua, -4);
	new_envc = lua_tointeger(luaif->lua, -2);

	lua_string_table_to_strvec(luaif, -3, argv, new_argc);
	lua_string_table_to_strvec(luaif, -1, envp, new_envc);

	/* remove sbox_execve_preprocess' return values from the stack.  */
	lua_pop(luaif->lua, 6);

	SB_LOG(SB_LOGLEVEL_NOISE,
		"sb_execve_preprocess: at exit, gettop=%d", lua_gettop(luaif->lua));
	return res;
}
