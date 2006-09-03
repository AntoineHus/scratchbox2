/*
 * Copyright (c) 2004, 2005 Nokia
 * Author: Timo Savola <tsavola@movial.fi>
 *
 * Licensed under GPL, see COPYING for details.
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include "sb_config.hh"
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <list>
#include <iterator>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>

using namespace std;

/*
 * Compilers
 */

struct Compiler {
	string name;
	list<string> prefix_list;
	string subst_prefix;
	string dir;
	string specs_file;
	string ld_args;

	bool ok() const
	{
		return !name.empty() && !prefix_list.empty() && !subst_prefix.empty() && !dir.empty();
	}

	bool has_prefix(const string &prefix) const
	{
		const list<string>::const_iterator end = prefix_list.end();
		return find(prefix_list.begin(), end, prefix) != end;
	}
};

static Compiler default_gcc;
static Compiler cross_gcc;
static Compiler host_gcc;

static void init_compilers()
{
	map<string,string> config;
	if (!sb::read_config(config))
		throw sb::error("unable to open scratchbox.config");

	cross_gcc.name         = config["SBOX_CROSS_GCC_NAME"];
	cross_gcc.prefix_list  = sb::split(config["SBOX_CROSS_GCC_PREFIX_LIST"]);
	cross_gcc.subst_prefix = config["SBOX_CROSS_GCC_SUBST_PREFIX"];
	cross_gcc.specs_file   = config["SBOX_CROSS_GCC_SPECS_FILE"];
	cross_gcc.dir          = config["SBOX_CROSS_GCC_DIR"];
	cross_gcc.ld_args      = config["SBOX_CROSS_GCC_LD_ARGS"];

	host_gcc.name          = config["SBOX_HOST_GCC_NAME"];
	host_gcc.prefix_list   = sb::split(config["SBOX_HOST_GCC_PREFIX_LIST"]);
	host_gcc.subst_prefix  = config["SBOX_HOST_GCC_SUBST_PREFIX"];
	host_gcc.specs_file    = config["SBOX_HOST_GCC_SPECS_FILE"];
	host_gcc.dir           = config["SBOX_HOST_GCC_DIR"];
	host_gcc.ld_args       = config["SBOX_HOST_GCC_LD_ARGS"];

	if (!cross_gcc.ok() && !host_gcc.ok())
		throw sb::error("No proper compiler configurations found");

	const string &default_prefix = config["SBOX_DEFAULT_GCC_PREFIX"];
	if (cross_gcc.has_prefix(default_prefix))
		default_gcc = cross_gcc;
	else if (host_gcc.has_prefix(default_prefix))
		default_gcc = host_gcc;
	else
		default_gcc.prefix_list.push_back(default_prefix);
}

/*
 * Programs
 */

struct Name {
	const char *name;
	const char *var;
};

typedef const Name * Group;

struct Program {
	const Name &name;
	const Compiler &compiler;
	Group group;

	Program(const Name &n, const Compiler &c)
		: name(n), compiler(c), group(0)
	{
	}
};

static const Name compilers[] = {
	{ "cc",        "CC",        },
	{ "gcc",       "CC",        },
	{ "gcc-3.3",   "CC",        },
	{ "c++",       "CXX",       },
	{ "g++",       "CXX",       },
	{ "g++-3.3",   "CXX",       },
	{ "cpp",       "CPP",       },
	{ "f77",       "F77",       },
	{ "g77",       "F77",       },
	{ 0 }
};

static const Name linkers[] = {
	{ "ld",        "LD",        },
	{ 0 }
};

static const Name others[] = {
	{ "addr2line", "ADDR2LINE", },
	{ "ar",        "AR",        },
	{ "as",        "AS",        },
	{ "c++filt",   "CXXFILT",   },
	{ "gccbug",    "GCCBUG",    },
	{ "gcov",      "GCOV",      },
	{ "nm",        "NM",        },
	{ "objcopy",   "OBJCOPY",   },
	{ "objdump",   "OBJDUMP",   },
	{ "ranlib",    "RANLIB",    },
	{ "readelf",   "READELF",   },
	{ "size",      "SIZE",      },
	{ "strings",   "STRINGS",   },
	{ "strip",     "STRIP",     },
	{ 0 }
};

static Program *detect_program(const char *const name, const Name &listedname,
			       const Compiler &compiler)
{
	const list<string> &ls = compiler.prefix_list;

	for (list<string>::const_iterator i = ls.begin(); i != ls.end(); ++i) {
		string full = *i;
		full += listedname.name;

		if (full == name)
			return new Program(listedname, compiler);
	}

	return 0;
}

static Program *detect_program(const char *const name, const Group group)
{
	Program *prog = 0;

	for (size_t i = 0; group[i].name; ++i) {
		if (strcmp(group[i].name, name) == 0) {
			if (!default_gcc.ok())
				throw sb::error("default compiler prefix does not match"
						" any compiler configuration");

			prog = new Program(group[i], default_gcc);
			break;
		}

		if ((prog = detect_program(name, group[i], cross_gcc)) ||
		    (prog = detect_program(name, group[i], host_gcc )))
			break;
	}

	if (prog)
		prog->group = group;

	return prog;
}

static const Program &detect_program(const char *const absname)
{
	// GNU basename never modifies its argument
	const char *const name = basename(absname);

	const Program *prog;
	if ((prog = detect_program(name, compilers)) ||
	    (prog = detect_program(name, linkers  )) ||
	    (prog = detect_program(name, others   )))
		return *prog;

	throw sb::error(absname, " cannot be recognized\n"
		"Maybe you are trying to run a compiler of a wrong architecture?");
}

/*
 * Execution
 */

#define CCACHE_PATH "/scratchbox/tools/bin/ccache"

static list<string> split(const string &str)
{
	list<string> l;

	string::const_iterator i = str.begin();
	const string::const_iterator end = str.end();

	while (i != end) {
		while (isspace(*i))
			++i;

		string::const_iterator j = i;
		while (j != end && !isspace(*j))
			++j;

		l.push_back(string(i, j));

		i = j;
	}

	return l;
}

static void extra_args(list<string> &args, const string str)
{
	insert_iterator<list<string> > inserter(args, args.begin());

	const list<string> extras = split(str);
	for (list<string>::const_iterator i = extras.begin(); i != extras.end(); ++i)
		*inserter = *i;
}

static void block_args(list<string> &args, const string &str)
{
	const list<string>::iterator beg = args.begin();
	list<string>::iterator end = args.end();

	const list<string> blocked = split(str);
	for (list<string>::const_iterator i = blocked.begin(); i != blocked.end(); ++i)
		end = remove(beg, end, *i);

	args = list<string>(beg, end);
}

static char *getenv(const char *const strong, const char *const fair, const char *const weak)
{
	char *str = 0;

	if (strong)
		str = getenv(strong);

	if (!str) {
		if (fair)
			str = getenv(fair);

		if (!str && weak)
			str = getenv(weak);
	}

	return str;
}

static const char *build_var(const char *const type, const Program &prog)
{
	string str = "SBOX_";
	str += type;
	str += '_';
	str += prog.name.var;
	str += "_ARGS";

	// leak some memory...
	return strdup(str.c_str());
}

static void exec_program(const Program &prog, const int old_argc, char **const old_argv)
{
	string new_path = prog.compiler.dir;
	const char *const old_path = getenv("PATH");
	if (old_path)
		(new_path += ':') += old_path;
	if (setenv("PATH", new_path.c_str(), 1) < 0)
		throw sb::error(strerror(errno));

	string progpath = prog.compiler.dir;
	((progpath += "/") += prog.compiler.subst_prefix) += prog.name.name;

	list<string> args;
	for (int i = 1; i < old_argc; ++i)
		args.push_back(old_argv[i]);

	if (prog.group == compilers && !prog.compiler.specs_file.empty())
		args.push_front(string("-specs=") += prog.compiler.specs_file);

	if (prog.group == linkers && !prog.compiler.ld_args.empty())
		extra_args(args, prog.compiler.ld_args);

	const char *extra_group = 0;
	const char *block_group = 0;
	if (prog.group == compilers) {
		extra_group = "SBOX_EXTRA_COMPILER_ARGS";
		block_group = "SBOX_BLOCK_COMPILER_ARGS";
	}

	if (char *s = getenv(build_var("EXTRA", prog), extra_group, "SBOX_EXTRA_ARGS"))
		extra_args(args, s);

	if (char *s = getenv(build_var("BLOCK", prog), block_group, "SBOX_BLOCK_ARGS"))
		block_args(args, s);

	const char *path = progpath.c_str();
	char **const new_argv = sb::build_argv(path, args);

	if (prog.group == compilers) {
		const char *s = getenv("SBOX_USE_CCACHE");
		if (s && strcmp(s, "yes") == 0)
			path = CCACHE_PATH;
	}

#if 0
	cerr << "path: \"" << path << "\"; argv:";
	for (int i = 0; new_argv[i]; ++i)
		cerr << " \"" << new_argv[i] << "\"";
	cerr << endl;
#endif

	execv(path, new_argv);
	throw sb::error(path, ": ", strerror(errno));
}

/*
 * Blocked Execution
 */

static void block_program(const Program &prog)
{
	string var = "SBOX_BLOCK_";
	var += prog.name.var;

	const char *str = getenv(var.c_str());
	if (str && str[0] != '\0' && strcmp(str, "no") != 0) {
		int num = atoi(str);
		if (num == 0 && strcmp(str, "0") != 0 && strcmp(str, "yes") != 0)
			throw sb::error(var, ": invalid value");

		// exit with desired error code instead of executing the program
		exit(num);
	}
}

/*
 * Main
 */

int main(const int argc, char **const argv)
{
	try {
		init_compilers();
		const Program &prog = detect_program(argv[0]);
		block_program(prog);
		exec_program(prog, argc, argv);

	} catch (const exception &e) {
		cerr << "sb_gcc_wrapper (" << argv[0] << "): " << e.what() << endl;
	}

	return 1;
}
