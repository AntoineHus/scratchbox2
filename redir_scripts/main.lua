-- Scratchbox2 universal redirector dynamic path translation scripts
-- Copyright (C) 2006 Lauri Leukkunen

--print "hello!\n"

tools_root = os.getenv("SBOX_TOOLS_ROOT")
if (not tools_root) then
	tools_root = "/scratchbox/sarge"
end

target_root = os.getenv("SBOX_TARGET_ROOT")
if (not target_root) then
	target_root = "/"
end

compiler_root = os.getenv("SBOX_COMPILER_ROOT")
if (not compiler_root) then
	compiler_root = "/usr"
end

verbose = os.getenv("SBOX_MAPPING_VERBOSE")


-- SBOX_REDIR_SCRIPTS environment variable controls where
-- we look for the scriptlets defining the path mappings

rsdir = os.getenv("SBOX_REDIR_SCRIPTS")
if (rsdir == nil) then
	rsdir = "/scratchbox/redir_scripts"
end

chains = {}

-- sb.sb_getdirlisting is provided by lua_bindings.c
-- it returns a table listing all files in a directory
t = sb.sb_getdirlisting(rsdir .. "/preload")

if (t ~= nil) then
	local i = 0
	local r = 0
	table.sort(t)
	-- load the individual parts ($SBOX_REDIR_SCRIPTS/parts/*.lua)
	for n = 1,table.maxn(t) do
		if (string.match(t[n], "%a*%.lua$")) then
			-- print("loading part: " .. t[n])
			filename = rsdir .. "/preload/" .. t[n]
			f, err = loadfile(filename)
			if (f == nil) then
				error("\nError while loading " .. filename .. ": \n" .. err .. "\n")
			else
				f() -- execute the loaded chunk
				-- export_chains variable contains now the chains
				-- from the chunk
				for i = 1,table.maxn(export_chains) do
					--print("loading chain:" .. export_chains[i].binary)
					-- fill in the default values
					if (not export_chains[i].binary) then
						export_chains[i].binary = ".*"
					end
					-- loop through the rules
					for r = 1, table.maxn(export_chains[i].rules) do
						if (not export_chains[i].rules[r].func_name) then
							export_chains[i].rules[r].func_name = ".*"
						end
						if (not export_chains[i].rules[r].path) then
							-- this is an error, report and exit
							print("path not specified for a rule in " .. filename)
							os.exit(1)
						end
					end
					table.insert(chains, export_chains[i])
				end
			end
		end
	end
end


function basename(path)
	if (path == "/") then
		return "/"
	else
		return string.match(path, "[^/]*$")
	end
end

function dirname(path)
	if (path == "/") then
		return "/"
	end
	dir = string.match(path, ".*/")
	if (dir == nil) then
		return "."
	end

	if (dir == "/") then return dir end

	-- chop off the trailing /
	if (string.sub(dir, string.len(dir)) == "/") then
		dir = string.sub(dir, 1, string.len(dir) - 1)
	end
	return dir 
end


function sbox_map_to(binary_name, func_name, work_dir, rp, path, rule)
	local ret = nil
	if (rule.map_to) then
		if (string.sub(rule.map_to, 1, 1) == "=") then
			--print(string.format("rp: %s\ntarget_root: %s", rp, target_root))
			return target_root .. string.sub(rule.map_to, 2) .. path
		elseif (string.sub(rule.map_to, 1, 1) == "+") then
			ret = compiler_root .. string.sub(rule.map_to, 2) .. "/" .. basename(path)
			return ret
		elseif (string.sub(rule.map_to, 1, 1) == "-") then
			ret = string.match(path, rule.path .. "(.*)")
			ret = string.sub(rule.map_to, 2) .. ret
			return ret
		else
			return rule.map_to .. path
		end
	end

	return path
end


function find_rule(chain, func, path)
	local i = 0
	local wrk = chain
	while (wrk) do
		-- travel the chains
		for i = 1, table.maxn(wrk.rules) do
			-- loop the rules in a chain
			if (string.match(func, wrk.rules[i].func_name) and
				string.match(path, wrk.rules[i].path)) then
				return wrk.rules[i]
			end
		end
		wrk = wrk.next
	end
	return nil
end

-- sbox_translate_path is the function called from libsb2.so
-- preload library and the FUSE system for each path that needs 
-- translating

function sbox_translate_path(binary_name, func_name, work_dir, path)
	--print(string.format("[%s]:", binary_name))
	--print(string.format("debug: [%s][%s][%s][%s]", binary_name, func_name, work_dir, path))

	local ret = path
	local rp = path
	local rule = nil

	-- loop through the chains, first match is used
	for n=1,table.maxn(chains) do
		-- print(string.format("looping through rules: %s, %s, %s", rules[n].binary, rules[n].func_name, rules[n].path))
		if (string.match(binary_name, chains[n].binary)) then
			rule = find_rule(chains[n], func_name, rp)
			if (not rule) then
				-- error, not even a default rule found
				print(string.format("Unable to find a match at all: [%s][%s][%s]", binary_name, func_name, path))
				return path
			end
			if (rule.custom_map_func ~= nil) then
				return rule.custom_map_func(binary_name, func_name, work_dir, rp, path, rules[n])
			else
				ret = sbox_map_to(binary_name, func_name, work_dir, rp, path, rule)
				if (verbose) then
					print(string.format("[%i]%s: %s(%s) -> [%s]", n, binary_name, func_name, path, ret))
				end
				return ret
			end
		end
	end

	-- we should never ever get here, if we still do, map
	return target_root .. rp
end

