/*
 * Copyright (C) 2006,2007 Lauri Leukkunen <lle@rahina.org>
 *
 * Licensed under LGPL version 2.1, see top level LICENSE file for details.
 */

#ifndef MAPPING_H
#define MAPPING_H

#include <sys/types.h>


#define enable_mapping(a) ((a)->mapping_disabled--)
#define disable_mapping(a) ((a)->mapping_disabled++)

extern char *scratchbox_path(const char *func_name, const char *path,
	int *ro_flagp, int dont_resolve_final_symlink);
extern char *scratchbox_path3(const char *binary_name, const char *func_name,
	const char *path, int *ro_flagp, int dont_resolve_final_symlink);
char *scratchbox_path_for_exec(const char *func_name, const char *path,
	int *ro_flagp, int dont_resolve_final_symlink);

extern int sb_execve_preprocess(char **file, char ***argv, char ***envp);
extern char *emumode_map(const char *path);
extern void sb_push_string_to_lua_stack(char *str);
extern int sb_execve_postprocess(char *exec_type,
	char **mapped_file, char **filename, const char *binary_name,
	char ***argv, char ***envp);
extern char *sb_query_exec_policy(const char *field_name,
	const char *binary_name, const char *real_binary_name);

#endif
