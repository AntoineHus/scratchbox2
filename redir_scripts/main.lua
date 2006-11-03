-- Scratchbox2 universal redirector dynamic path translation scripts
-- Copyright (C) 2006 Lauri Leukkunen

-- print "hello!\n"

tools_root = os.getenv("SBOX_TOOLS_ROOT")
if (tools_root == nil) then
	tools_root = "/scratchbox/sarge"
end

-- SBOX_REDIR_SCRIPTS environment variable controls where
-- we look for the scriptlets defining the path mappings

rsdir = os.getenv("SBOX_REDIR_SCRIPTS")
if (rsdir == nil) then
	rsdir = "/scratchbox/redir_scripts"
end

rules = {}

-- sb.sb_getdirlisting is provided by lua_bindings.c
-- it returns a table listing all files in a directory
t = sb.sb_getdirlisting(rsdir .. "/parts")

if (t ~= nil) then
	table.sort(t)
	-- load the individual parts ($SBOX_REDIR_SCRIPTS/parts/*.lua)
	for n = 0,table.maxn(t) do
		if (string.match(t[n], "%a*%.lua$")) then
			-- print("loading part: " .. t[n])
			filename = rsdir .. "/parts/" .. t[n]
			f, err = loadfile(filename)
			if (f == nil) then
				error("\nError while loading " .. filename .. ": \n" .. err .. "\n")
			else
				f() -- execute the loaded chunk
				-- export_rules variable contains now the rules
				-- from the chunk
				for i = 1,table.maxn(export_rules) do
					-- print("loading rule:" .. export_rules[i].binary)
					table.insert(rules, export_rules[i])
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
	ret = ""

	if (rule.map_to ~= nil) then
		if (string.sub(rule.map_to, 1, 1) == "=") then
			ret = tools_root .. string.sub(rule.map_to, 2)
		else
			ret = tools_root .. rule.map_to
		end
	end

	-- print("mapping to: " .. ret .. path)
	return ret .. path
end


-- sbox_translate_path is the function called from libsb2.so
-- preload library and the FUSE system for each path that needs 
-- translating

function sbox_translate_path(binary_name, func_name, work_dir, path)

	-- print(string.format("debug: [%s][%s][%s][%s]", binary_name, func_name, work_dir, path))

	ret = path
	rp = sb.sb_realpath(path)

	if (rp == "no such file") then
		rp = path
	end

	-- loop through the rules, first match is used
	for n=1,table.maxn(rules) do
		-- print(string.format("looping through rules: %s, %s, %s", rules[n].binary, rules[n].func_name, rules[n].path))
		if (string.match(binary_name, rules[n].binary) and
			string.match(func_name, rules[n].func_name) and
			string.match(rp, rules[n].path)) then
			if (rules[n].custom_map_func ~= nil) then
				return rules[n].custom_map_func(binary_name, func_name, work_dir, rp, path, rules[n])
			else
				return sbox_map_to(binary_name, func_name, work_dir, rp, path, rules[n])
			end
		end
	end

	-- fail safe, if none matched, map
	return tools_root .. rp
end

