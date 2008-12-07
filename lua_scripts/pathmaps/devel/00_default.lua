-- Copyright (C) 2007 Lauri Leukkunen <lle@rahina.org>
-- Copyright (c) 2008 Nokia Corporation.
-- Licensed under MIT license.
--
-- "devel" mode, to be used for software development & building when
-- the "simple" mode is not enough.


-- Rule file interface version, mandatory.
--
rule_file_interface_version = "16"
----------------------------------

tools = tools_root
if (not tools) then
	tools = "/"
end

sb2_share_dir = sbox_user_home_dir.."/.scratchbox2/"..sbox_target.."/share"

-- =========== Exec policies:  ===========
--
-- For tools: If tools_root is set and libsb2 has been installed there,
-- then dynamic libraries can be used from tools_root (otherwise we'll
-- be using the libs from the host OS)

devel_mode_tools_ld_so = nil		-- default = not needed
devel_mode_tools_ld_library_path = nil	-- default = not needed

if ((tools_root ~= nil) and conf_tools_sb2_installed) then
	if (conf_tools_ld_so ~= nil) then
		-- Ok to use dynamic libraries from tools!
		devel_mode_tools_ld_so = conf_tools_ld_so
		devel_mode_tools_ld_library_path = conf_tools_ld_so_library_path
	end
end

exec_policy_tools = {
	name = "Tools",
	native_app_ld_so = devel_mode_tools_ld_so,
	native_app_ld_so_supports_argv0 = conf_tools_ld_so_supports_argv0,
	native_app_ld_library_path = devel_mode_tools_ld_library_path
}

-- For target binaries:
-- First, note that "foreign" binaries are easy to handle, no problem there.
-- But if CPU transparency method has not been set, then host CPU == target CPU:
-- we have "target's native" and "host's native" binaries, that would look 
-- identical (and valid!) to the kernel. But they need to use different 
-- loaders and dynamic libraries! The solution is that we use the location
-- (as determined by the mapping engine) to decide the execution policy.

devel_mode_target_ld_so = nil		-- default = not needed
devel_mode_target_ld_library_path = nil	-- default = not needed

if (conf_target_sb2_installed) then
	if (conf_target_ld_so ~= nil) then
		-- use dynamic libraries from target, 
		-- when executing native binaries!
		devel_mode_target_ld_so = conf_target_ld_so
		devel_mode_target_ld_library_path = conf_target_ld_so_library_path

		-- FIXME: This exec policy should process (map components of)
		-- the current value of LD_LIBRARY_PATH, and add the results
		-- to devel_mode_target_ld_library_path just before exec.
		-- This has not been done yet.
	end
end

exec_policy_target = {
	name = "Rootstrap",
	native_app_ld_so = devel_mode_target_ld_so,
	native_app_ld_so_supports_argv0 = conf_target_ld_so_supports_argv0,
	native_app_ld_library_path = devel_mode_target_ld_library_path
}

--
-- For real host binaries:

exec_policy_host_os = {
	name = "Host",
	log_level = "debug",
	log_message = "executing in host OS mode"
}

-- =========== Actions for conditional rules ===========

test_first_target_then_tools_default_is_target = {
	{ if_exists_then_map_to = target_root, readonly = true },
	{ if_exists_then_map_to = tools, readonly = true },
	{ map_to = target_root, readonly = true }
}

test_first_tools_default_is_target = {
	{ if_exists_then_map_to = tools, readonly = true },
	{ map_to = target_root, readonly = true }
}

-- =========== Mapping rule chains ===========

simple_chain = {
	next_chain = nil,
	binary = nil,
	rules = {
		-- -----------------------------------------------
		-- 1. General SB2 environment:

		{prefix = "/usr/bin/sb2-",
		 use_orig_path = true, readonly = true},
		{prefix = "/usr/share/scratchbox2/host_usr",
		 replace_by = "/usr", readonly = true},
		{prefix = "/usr/share/scratchbox2",
		 use_orig_path = true, readonly = true},

		-- -----------------------------------------------
		-- 2. Development environment special destinations:

		{prefix = "/sb2/wrappers",
		 replace_by = session_dir .. "/wrappers." .. active_mapmode,
		 readonly = true},

		{prefix = "/sb2/scripts",
		 replace_by = sbox_dir.."/share/scratchbox2/scripts",
		 readonly = true},

		-- tools_root should not be mapped twice.
		{prefix = tools, use_orig_path = true, readonly = true},

		-- -----------------------------------------------
		-- 5. Maemo SDK+

		{prefix = "/opt/maemo",
		 use_orig_path = true, readonly = true},

		-- -----------------------------------------------
		-- 10. Home directories

		{prefix = sbox_user_home_dir, use_orig_path = true},

		-- "user" is a special username at least on the Maemo platform:
		-- (but note that if the real user name is "user",
		-- our previous rule handled that and this rule won't be used)
		{prefix = "/home/user", map_to = target_root},

		-- Home directories = not mapped, R/W access
		{prefix = "/home", use_orig_path = true},

		-- -----------------------------------------------
		-- 20. /bin/* and /usr/bin/*:
		-- tools that need special processing:

		{path = "/bin/sh",
		 replace_by = tools .. "/bin/bash", readonly = true},
		{prefix = "/usr/bin/host-",
		 use_orig_path = true, readonly = true},

		-- -----------------------------------------------
		-- 30. /lib/*

		{prefix = "/lib", map_to = target_root, readonly = true},

		-- -----------------------------------------------
		-- 40. /usr/lib/*
		-- Most of /usr/lib should come from target_root, but
		-- there are exceptions: Some tools have private subdirectories
		-- there.

		{prefix = "/usr/lib/gcc", map_to = tools, readonly = true},
		{prefix = "/usr/lib/perl", map_to = tools, readonly = true},
		{prefix = "/usr/lib/dpkg", map_to = tools, readonly = true},
		{prefix = "/usr/lib/apt", map_to = tools, readonly = true},
		{prefix = "/usr/lib/cdbs", map_to = tools, readonly = true},
		{prefix = "/usr/lib/libfakeroot", map_to = tools, readonly = true},
		{prefix = "/usr/lib/man-db", map_to = tools, readonly = true},

		-- /usr/lib/python* from tools_root
		{prefix = "/usr/lib/python", map_to = tools, readonly = true},

		{prefix = "/usr/lib", map_to = target_root, readonly = true},

		-- -----------------------------------------------
		-- 45. /usr/share/aclocal*
		-- This is more than a bit complex, we must mix files from
		-- both places:
		-- Prefer files in tools_root, but if not there, try
		-- to get it from target_root. New files will be created
		-- to target_root.
		-- directory itself comes from target_root; there are several
		-- lib*-dev packages that add files to the aclocal directories
		-- (because of this, opendir() must open the diretory from
		-- target_root)
		--
		-- First, existing directories => target_root:
		{path = "/usr/share/aclocal",
		 actions = test_first_target_then_tools_default_is_target},
		{path = "/usr/share/aclocal-1.4",
		 actions = test_first_target_then_tools_default_is_target},
		{path = "/usr/share/aclocal-1.7",
		 actions = test_first_target_then_tools_default_is_target},
		{path = "/usr/share/aclocal-1.8",
		 actions = test_first_target_then_tools_default_is_target},
		{path = "/usr/share/aclocal-1.9",
		 actions = test_first_target_then_tools_default_is_target},
		{path = "/usr/share/aclocal-1.10",
		 actions = test_first_target_then_tools_default_is_target},

		-- Next, exceptions to these rules:
		-- 1) gnome-common presents policy problems, typically we
		--    have it in both places but want to take it from the
		--    rootstrap:
		{prefix = "/usr/share/aclocal/gnome-common",
		 actions = test_first_target_then_tools_default_is_target},
		{prefix = "/usr/share/aclocal/gnome-compiler",
		 actions = test_first_target_then_tools_default_is_target},

		-- Next, use /usr/share/aclocal* from tools_root if target
		-- exists, but default is target_root
		{prefix = "/usr/share/aclocal",
		 actions = test_first_tools_default_is_target},

		-- -----------------------------------------------
		-- 46. /usr/share/* (other than /usr/share/aclocal*)
		--
		-- The default is to map /usr/share to tools_root,
		-- but there are lots of exceptions. That directory
		-- is used for so many purposes nowadays..

		-- (see the comment about gnome-common files in .../aclocal):
		{prefix = "/usr/share/gnome-common",
		 actions = test_first_target_then_tools_default_is_target},

		{prefix = "/usr/share/glib-2.0", map_to = target_root,
		 readonly = true},
		{prefix = "/usr/share/dbus-1", map_to = target_root,
		 readonly = true},

		-- zoneinfo belongs to "libc6" package:
		{prefix = "/usr/share/zoneinfo", map_to = target_root,
		 readonly = true},

		-- files from package 'microb-engine-dev':
		{path = "/usr/share/idl", map_to = target_root,
		 readonly = true},
		{prefix = "/usr/share/idl/microb-engine", map_to = target_root,
		 readonly = true},

		-- /usr/share/hildon* (this is a real prefix):
		-- (was added to map hildon-theme-layout-4)
		{prefix = "/usr/share/hildon", map_to = target_root,
		 readonly = true},

		-- for libwww0:
		{prefix = "/usr/share/w3c-libwww", map_to = target_root,
		 readonly = true},

		-- for libwww-dev:
		{prefix = "/usr/share/libwww", map_to = target_root,
		 readonly = true},

		-- for libsofia-sip-ua-dev:
		{prefix = "/usr/share/sofia-sip", map_to = target_root,
		 readonly = true},

		{prefix = "/usr/share/osso", map_to = target_root,
		 readonly = true},
		{prefix = "/usr/share/doc", map_to = target_root,
		 readonly = true},

		-- for liblzo-dev:
		{prefix = "/usr/share/lzo", map_to = target_root,
		 readonly = true},

		-- for modest-providers-data:
		{prefix = "/usr/share/modest", map_to = target_root,
		 readonly = true},

		-- default rules:
		{path = "/usr/share", map_to = tools, readonly = true},
		{prefix = "/usr/share/", map_to = tools, readonly = true},

		-- -----------------------------------------------
		-- 50. /usr/src/*
		{path = "/usr/src", map_to = target_root, readonly = true},
		{prefix = "/usr/src/", map_to = target_root, readonly = true},

		-- -----------------------------------------------
		-- 55. X11 (/usr/X11R6/*)

		{prefix = "/usr/X11R6/lib", map_to = target_root,
		 readonly = true},
		{prefix = "/usr/X11R6/include", map_to = target_root,
		 readonly = true},

		-- -----------------------------------------------
		-- 60. /usr/include/*

		{prefix = "/usr/include", map_to = target_root,
		 readonly = true},

		-- -----------------------------------------------
		-- 70. /etc/*
		--
		{prefix = "/etc/gconf/2", map_to = target_root,
		 readonly = true},
		{prefix = "/etc/dbus-1", map_to = target_root,
		 readonly = true},
		{prefix = "/etc/osso-af-init", map_to = target_root,
		 readonly = true},
		{prefix = "/etc/gtk-2.0", map_to = target_root,
		 readonly = true},

		{prefix = "/etc/apt", map_to = target_root, readonly = true},

		-- Files that must not be mapped:
		{prefix = "/etc/resolv.conf",
		 use_orig_path = true, readonly = true},
		{path = "/etc/passwd",
		 use_orig_path = true, readonly = true},
		{path = "/etc/shadow",
		 use_orig_path = true, readonly = true},

		-- default rules:
		{path = "/etc", map_to = tools, readonly = true},
		{prefix = "/etc/", map_to = tools, readonly = true},

		-- -----------------------------------------------
		-- 80. /var/*

		{prefix = "/var/run", map_to = session_dir},

		-- files from package "xkbutils":
		{prefix = "/var/lib/xkb", map_to = target_root,
		 readonly = true},

		-- apt & dpkg:
		{prefix = "/var/lib/apt", map_to = target_root,
		 readonly = true},
		{prefix = "/var/cache/apt", map_to = target_root,
		 readonly = true},
		{prefix = "/var/lib/dpkg", map_to = target_root,
		 readonly = true},
		{prefix = "/var/cache/dpkg", map_to = target_root,
		 readonly = true},
		{prefix = "/var/cache/debconf", map_to = target_root,
		 readonly = true},

		{prefix = "/var/lib/dbus", map_to = target_root,
		 readonly = true},

		{prefix = "/var/log", map_to = target_root,
		 readonly = true},

		-- -----------------------------------------------
		-- 85. /tmp
		{prefix = session_dir, use_orig_path = true},
		{prefix = "/tmp", map_to = session_dir},

		-- -----------------------------------------------
		-- 90. Top-level directories that must not be mapped:
		{prefix = "/dev", use_orig_path = true},
		{prefix = "/proc", use_orig_path = true},
		{prefix = "/sys",
		 use_orig_path = true, readonly = true},

		-- -----------------------------------------------
		-- 95. Some virtual paths:
		{prefix = "/host_usr", map_to = target_root, readonly = true},

		-- unmodified view of the rootstrap, can be used as destination
		-- directory when installing stuff to the rootstrap
		-- This provides is R/W access to the target_root!
		{prefix = "/target_root", replace_by = target_root},

		-- -----------------------------------------------
		-- 98. Scratchbox 1 emulation rules
		-- (some packages have hard-coded paths to the SB1 enviroment;
		-- replace those by the correct locations in our environment)
		-- (these are marked "virtual"; these won't be reversed)

		-- "libtool" for arm
		{prefix = "/scratchbox/compilers/cs2005q3.2-glibc2.5-arm/arch_tools/share/libtool",
		 replace_by = sb2_share_dir .. "/libtool",
		 log_level = "warning",
		 readonly = true, virtual_path = true},

		-- "libtool" for i386
		{prefix = "/scratchbox/compilers/cs2005q3.2-glibc-i386/arch_tools/share",
		 replace_by = tools .. "/usr/share",
		 log_level = "warning",
		 readonly = true, virtual_path = true},

		{prefix = "/scratchbox/tools/bin",
		 replace_by = tools .. "/usr/bin",
		 log_level = "warning",
		 readonly = true, virtual_path = true},

		{prefix = "/scratchbox/tools/autotools/automake-1.7/share/automake-1.7",
		 replace_by = tools .. "/usr/share/automake-1.7",
		 log_level = "warning",
		 readonly = true, virtual_path = true},

		-- otherwise, don't map /scratchbox, some people still
		-- keep their projects there.
		{prefix = "/scratchbox", use_orig_path = true},

		-- -----------------------------------------------
		-- 100. DEFAULT RULES:
		-- the root directory must not be mapped:
		{path = "/", use_orig_path = true},

		-- "standard" directories are mapped to tools_root,
		-- but everything else defaults to the host system
		-- (so that things like /mnt, /media and /opt are
		-- used from the host)
		{prefix = "/bin", map_to = tools, readonly = true},
		{prefix = "/etc", map_to = tools, readonly = true},
		{prefix = "/sbin", map_to = tools, readonly = true},
		{prefix = "/usr", map_to = tools, readonly = true},
		{prefix = "/var", map_to = tools, readonly = true},

		-- Default = Host, R/W access
		{prefix = "/", use_orig_path = true}
	}
}

qemu_chain = {
	next_chain = nil,
	binary = basename(sbox_cputransparency_method),
	rules = {
		{prefix = "/lib", map_to = target_root, readonly = true},
		{prefix = "/usr/lib", map_to = target_root, readonly = true},
		{prefix = "/usr/local/lib", map_to = target_root,
		 readonly = true},

		-- Home directories = not mapped, R/W access
		{prefix = "/home", use_orig_path = true},

		{prefix = session_dir, use_orig_path = true},
		{prefix = "/tmp", map_to = session_dir},

		{prefix = "/dev", use_orig_path = true},
		{prefix = "/proc", use_orig_path = true},
		{prefix = "/sys", use_orig_path = true},

		{prefix = "/etc/resolv.conf",
		 use_orig_path = true, readonly = true},
		{path = "/etc/passwd",
		 use_orig_path = true, readonly = true},
		{path = "/etc/shadow",
		 use_orig_path = true, readonly = true},

		-- tools_root should not be mapped twice.
		{prefix = tools, use_orig_path = true, readonly = true},

		-- unmodified view of the rootstrap, can be used as destination
		-- directory when installing stuff to the rootstrap
		{prefix = "/target_root", replace_by = target_root},

		{path = "/", use_orig_path = true},

		-- "standard" directories are mapped to tools_root,
		-- but everything else defaults to the host system
		-- (so that things like /mnt, /media and /opt are
		-- used from the host)
		{prefix = "/bin", map_to = tools, readonly = true},
		{prefix = "/etc", map_to = tools, readonly = true},
		{prefix = "/sbin", map_to = tools, readonly = true},
		{prefix = "/usr", map_to = tools, readonly = true},
		{prefix = "/var", map_to = tools, readonly = true},

		-- Default = Host, R/W access
		{prefix = "/", use_orig_path = true}
	}
}

-- Path mapping rules.
export_chains = {
	qemu_chain,
	simple_chain
}

-- Exec policy rules.
-- These are used only if the mapping rule did not provide an exec policy.
--
-- Note that the real path (mapped path) is used
--  when looking up rules from here!
devel_exec_policies = {
	next_chain = nil,
	binary = nil,
	rules = {

		-- ~/bin proobably contains programs for the host OS:
                {prefix = sbox_user_home_dir.."/bin", exec_policy = exec_policy_host_os},

                -- Other places under the home directory are expected
                -- to contain target binaries:
                {prefix = sbox_user_home_dir, exec_policy = exec_policy_target},

		-- Target binaries:
		{prefix = target_root, exec_policy = exec_policy_target},

		-- Tools:
		{prefix = tools, exec_policy = exec_policy_tools},

		-- -----------------------------------------------
		-- DEFAULT RULE (must exist):
		{prefix = "/", exec_policy = exec_policy_host_os}
	}
}

exec_policy_chains = {
	devel_exec_policies
}

