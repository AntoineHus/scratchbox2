-- Copyright (C) 2007 Lauri Leukkunen <lle@rahina.org>
-- Licensed under MIT license.

sb1_compat_dir = sbox_target_root .. "/scratchbox1-compat"

sb2_session_dir = os.getenv("SBOX_SESSION_DIR")
if (not sb2_session_dir) then
	sb2_session_dir = "/tmp"
end

mapall_chain = {
	next_chain = nil,
	binary = nil,
	rules = {
		{match = ".*qemu.*", use_orig_path = true},
		{prefix = target_root, use_orig_path = true},

		-- Scratchbox 1 compatibility rules:
		{ prefix = "/targets/", map_to = sb1_compat_dir },
		{ path = "/usr/bin/scratchbox-launcher.sh",
                    map_to = sb1_compat_dir },
                { path = "/etc/osso-af-init/dbus-systembus.sh",
                    map_to = sb1_compat_dir },
		
		-- 
		{prefix = sb2_session_dir, use_orig_path = true},
		{prefix = "/tmp", map_to = sb2_session_dir},

		-- 
		{prefix = "/dev", use_orig_path = true},
		{prefix = "/proc", use_orig_path = true},
		{prefix = "/sys", use_orig_path = true},
		{prefix = os.getenv("HOME") .. "/.scratchbox2",
		 use_orig_path = true},
		{prefix = os.getenv("SBOX_DIR") .. "/share/scratchbox2",
		 use_orig_path = true},

		{prefix = "/etc/resolv.conf", use_orig_path = true},

		{path = "/", use_orig_path = true},
		{prefix = "/", map_to = target_root}
	}
}

export_chains = {
	mapall_chain
}
