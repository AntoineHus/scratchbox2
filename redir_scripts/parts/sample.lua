-- Copyright (C) 2006 Lauri Leukkunen <lle@rahina.org>
-- Licensed under so called MIT license.

-- print "hello from sample.lua!\n"

-- All these values are treated as Lua patterns, 
-- except the map_to and custom_map_func fields.
-- In map_to these have special meaning:
--
-- "="			map to tools_root .. "/" .. path
-- "=/some/path"	map to tools_root .. "/some/path" .. "/" .. path
-- nil			no mapping, use straight
--
-- Any other value is prepended to path (map_to .. "/" .. path).
-- 
-- The rules are exported from this file to the main.lua engine
-- by putting them all into export_rules table variable.
-- They will be evaluated in the order they are listed there.
-- Also the files matching parts/*.lua are sorted alphabetically
-- and used in that order.
--
-- custom_map_func allows you to specify your own path mapping logic.
-- The function takes these parameters: 
-- function(binary_name, func_name, work_dir, path, rule) 
-- and is expected to return the mapped path. rule argument contains
-- the rule which triggered the function invocation.

sample_rule1 = {
	binary = "python",
	func_name = "exec",
	func_param = nil,
	path = "^/usr/bin",
	map_to = "=/usr/local",
	custom_map_func = nil
}

sample_rule2 = {
	binary = "*",
	func_name = "exec",
	func_param = nil,
	path = "^/usr/bin",
	map_to = "=",
	custom_map_func = nil
}

export_rules = {
	sample_rule1,
	sample_rule2
}

