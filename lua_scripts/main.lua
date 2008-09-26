-- Scratchbox2 Lua main file
-- Copyright (C) 2006, 2007 Lauri Leukkunen
-- Licensed under MIT license.

-- This file is loaded by the libsb2.so preload library, from the 
-- constructor to initialize sb2's "Lua-side"

debug = os.getenv("SBOX_MAPPING_DEBUG")
debug_messages_enabled = sb.debug_messages_enabled()

-- This version string is used to check that the lua scripts offer 
-- what the C files expect, and v.v.
-- Increment the serial number (first number) and update the initials
-- and date whenever the interface beween Lua and C is changed.
--
-- NOTE: the corresponding identifier for C is in include/sb2.h
sb2_lua_c_interface_version = "28,lta-2008-09-23"

function do_file(filename)
	if (debug_messages_enabled) then
		sb.log("debug", string.format("Loading '%s'", filename))
	end
	f, err = loadfile(filename)
	if (f == nil) then
		error("\nError while loading " .. filename .. ": \n" 
			.. err .. "\n")
		-- "error()" never returns
	else
		f() -- execute the loaded chunk
	end
end

session_dir = os.getenv("SBOX_SESSION_DIR")

-- Load session-specific settings
do_file(session_dir .. "/sb2-session.conf")
do_file(session_dir .. "/exec_config.lua")

-- Load mapping- and exec functions
do_file(session_dir .. "/lua_scripts/mapping.lua")
do_file(session_dir .. "/lua_scripts/argvenvp.lua")

-- sb2 is ready for operation!
