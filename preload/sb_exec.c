/*
 * Copyright (C) 2006,2007 Lauri Leukkunen <lle@rahina.org>
 *
 * Licensed under LGPL version 2.1
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <libgen.h>

#include <sys/utsname.h>
#include <sys/user.h>
#include <sys/mman.h>

#include <config.h>
#include <sb2.h>
#include <mapping.h>
#include <elf.h>

#include "libsb2.h"
#include "exported.h"

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(array) (sizeof (array) / sizeof ((array)[0]))
#endif

#if __BYTE_ORDER == __BIG_ENDIAN
# define HOST_ELF_DATA ELFDATA2MSB
#elif __BYTE_ORDER == __LITTLE_ENDIAN
# define HOST_ELF_DATA ELFDATA2LSB
#else
# error Invalid __BYTE_ORDER
#endif

#ifdef __i386__
# define HOST_ELF_MACHINE EM_386
#elif defined(__x86_64__)
# define HOST_ELF_MACHINE EM_X86_64
#elif defined(__ia64__)
# define HOST_ELF_MACHINE EM_IA_64
#elif defined(__powerpc__)
# define HOST_ELF_MACHINE EM_PPC
#else
# error Unsupported host CPU architecture
#endif

#ifndef PAGE_MASK
# define PAGE_MASK sysconf(_SC_PAGE_SIZE)
#endif

#ifdef __x86_64__
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
#else
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
#endif

struct target_info {
	char name[8];
	uint16_t machine;
	unsigned int default_byteorder;
	int multi_byteorder;
};

static const struct target_info target_table[] = {
	{ "arm",	EM_ARM,		ELFDATA2LSB,	1 },
	{ "mips",	EM_MIPS,	ELFDATA2MSB,	1 },
	{ "ppc",	EM_PPC,		ELFDATA2MSB,	0 },
	{ "sh",		EM_SH, 		ELFDATA2LSB,	1 },
};

static int elf_hdr_match(Elf_Ehdr *ehdr, uint16_t match, int ei_data);

static uint16_t byte_swap(uint16_t a);

static uint16_t byte_swap(uint16_t a)
{
	uint16_t b;
	uint8_t *r;
	uint8_t *t;
	t = (uint8_t *)&a;
	r = (uint8_t *)&b;
	*r++ = *(t + 1);
	*r = *t;
	return b;
}

enum binary_type {
	BIN_NONE, 
	BIN_UNKNOWN,
	BIN_INVALID,
	BIN_HOST_STATIC,
	BIN_HOST_DYNAMIC,
	BIN_TARGET,
	BIN_HASHBANG,
};


static int elem_count(char *const *elems)
{
	int count = 0;
	char **p = (char **)elems;
	while (*p) {
		p++; count++;
	}
	return count;
}

int token_count(char *str);
char **split_to_tokens(char *str);

int token_count(char *str)
{
	char *p;
	int count = 0;

	for (p = str; *p; ) {
		if (!isspace(*p)) {
			count++;
			while (*p && !isspace(*p))
				p++;
		} else {
			while (*p && isspace(*p))
				p++;
		}
	}

	return count;
}

char **split_to_tokens(char *str)
{
	int c, i, len;
	char **tokens, *start, *end;

	c = token_count(str);
	tokens = calloc(c + 1, sizeof(char *));
	i = 0;
	for (start = str; *start; start++) {
		if (isspace(*start))
			continue;
		end = start;
		while (*end && !isspace(*end))
			end++;
		len = end - start;
		tokens[i] = malloc(sizeof(char) * (len + 1));
		strncpy(tokens[i], start, len);
		tokens[i][len] = '\0';
		start = end - 1;
		i++;
	}
	tokens[i] = NULL;
	return tokens;
}

static int run_qemu(const char *qemu_bin, char *const *qemu_args,
		const char *file, char *const *argv, char *const *envp);
static int run_sbrsh(const char *sbrsh_bin, char *const *sbrsh_args,
		const char *target_root, const char *orig_file,
		char *const *argv, char *const *envp);

/* file is mangled, unmapped_file is not */
static int run_cputransparency(const char *file, const char *unmapped_file,
			char *const *argv, char *const *envp)
{
	char *cputransp_method, *cputransp_bin, *target_root;
	char **cputransp_tokens, **cputransp_args, **p;
	char *basec, *bname;
	int token_count, i;

	cputransp_method = getenv("SBOX_CPUTRANSPARENCY_METHOD");
	if (!cputransp_method) {
		fprintf(stderr, "SBOX_CPUTRANSPARENCY_METHOD not set, "
				"unable to execute the target binary\n");
		return -1;
	}

	cputransp_tokens = split_to_tokens(cputransp_method);
	token_count = elem_count(cputransp_tokens);
	if (token_count < 1) {
		free(cputransp_tokens);
		fprintf(stderr, "Invalid SBOX_CPUTRANSPARENCY_METHOD set\n");
		return -1;
	}

	cputransp_args = calloc(token_count, sizeof(char *));
	for (i = 1, p = cputransp_args; i < token_count; i++, p++) {
		*p = strdup(cputransp_tokens[i]);
	}

	*p = NULL;
	
	cputransp_bin = strdup(cputransp_tokens[0]);

	target_root = getenv("SBOX_TARGET_ROOT");
	if (!target_root) {
		fprintf(stderr, "SBOX_TARGET_ROOT not set, "
				"unable to execute the target binary\n");
		return -1;
	}

	basec = strdup(cputransp_bin);
	bname = basename(basec);

	if (strstr(bname, "qemu")) {
		free(basec);
		return run_qemu(cputransp_bin, cputransp_args,
				unmapped_file, argv, envp);
	} else if (strstr(bname, "sbrsh")) {
		free(basec);
		return run_sbrsh(cputransp_bin, cputransp_args,
				target_root, file, argv, envp);
	}

	free(basec);
	fprintf(stderr, "run_cputransparency() error: "
			"Unknown cputransparency method: [%s]\n",
			cputransp_bin);
	return -1;
}

static int is_subdir(const char *root, const char *subdir)
{
	size_t rootlen;

	if (strstr(subdir, root) != subdir)
		return 0;

	rootlen = strlen(root);
	return subdir[rootlen] == '/' || subdir[rootlen] == '\0';
}

static int run_sbrsh(const char *sbrsh_bin, char *const *sbrsh_args,
		const char *target_root, const char *orig_file,
		char *const *argv, char *const *envp)
{
	char *config, *file, *dir, **my_argv, **p;
	int len, i = 0;

	SB_LOG(SB_LOGLEVEL_INFO, "Exec:sbrsh (%s,%s,%s)",
		sbrsh_bin, target_root, orig_file);

	config = getenv("SBRSH_CONFIG");
	if (config && strlen(config) == 0)
		config = NULL;

	len = strlen(target_root);
	if (len > 0 && target_root[len - 1] == '/')
		--len;

	file = strdup(orig_file);
	if (file[0] == '/') {
		if (is_subdir(target_root, file)) {
			file += len;
		} else if (is_subdir(getenv("HOME"), file)) {
			/* no change */
		} else {
			fprintf(stderr, "Binary must be under target (%s) or"
			        " home when using sbrsh\n", target_root);
			errno = ENOTDIR;
			free(file);
			return -1;
		}
	}

	dir = get_current_dir_name();
	if (is_subdir(target_root, dir)) {
		dir += len;
	} else if (is_subdir(getenv("HOME"), dir)) {
		/* no change */
	} else {
		fprintf(stderr, "Warning: Executing binary with bogus working"
		        " directory (/tmp) because sbrsh can only see %s and"
		        " %s\n", target_root, getenv("HOME"));
		dir = "/tmp";
	}

	my_argv = calloc(elem_count(sbrsh_args) + 6 + elem_count(argv) + 1,
			sizeof (char *));

	my_argv[i++] = strdup(sbrsh_bin);
	for (p = (char **)sbrsh_args; *p; p++)
		my_argv[i++] = strdup(*p);

	if (config) {
		my_argv[i++] = "--config";
		my_argv[i++] = config;
	}
	my_argv[i++] = "--directory";
	my_argv[i++] = dir;
	my_argv[i++] = file;

	for (p = (char **)&argv[1]; *p; p++)
		my_argv[i++] = strdup(*p);

	for (p = (char **) envp; *p; ++p) {
		char *start, *end;

		if (strncmp("LD_PRELOAD=", *p, strlen("LD_PRELOAD=")) != 0)
			continue;

		start = strstr(*p, LIBSB2);
		if (!start)
			break;

		end = start + strlen(LIBSB2);

		while (start[-1] != '=' && !isspace(start[-1]))
			start--;

		while (*end != '\0' && isspace(*end))
			end++;

		memmove(start, end, strlen(end) + 1);
	}

	return sb_next_execve(sbrsh_bin, my_argv, envp);
}

static int run_qemu(const char *qemu_bin, char *const *qemu_args,
		const char *file, char *const *argv, char *const *envp)
{
	char **my_argv, **p;
	int i = 0;

	SB_LOG(SB_LOGLEVEL_INFO, "Exec:qemu (%s,%s)",
		qemu_bin, file);

	my_argv = (char **)calloc(elem_count(qemu_args) + elem_count(argv)
				+ 5 + 1, sizeof(char *));

	my_argv[i++] = strdup(qemu_bin);

	for (p = (char **)qemu_args; *p; p++) {
		my_argv[i++] = strdup(*p);
		printf(*p);
	}

	my_argv[i++] = "-drop-ld-preload";
	my_argv[i++] = "-L";
	my_argv[i++] = "/";
	my_argv[i++] = strdup(file); /* we're passing the unmapped file
				      * here, it works because qemu will
				      * do open() on it, and that gets
				      * mapped again.
				      */
	for (p = (char **)&argv[1]; *p; p++) {
		my_argv[i++] = strdup(*p);
	}

	my_argv[i] = NULL;

	return sb_next_execve(qemu_bin, my_argv, envp);
}

static int run_app(const char *file, char *const *argv, char *const *envp)
{
	sb_next_execve(file, argv, envp);

	fprintf(stderr, "libsb2.so failed running (%s): %s\n", file,
			strerror(errno));
	return -12;
}

static int ld_so_run_app(const char *file, char *const *argv, char *const *envp)
{
	char *binaryname, **my_argv;
	char *host_libs, *ld_so;
	char **p;
	char *tmp;
	char ld_so_buf[PATH_MAX + 1];
	char ld_so_basename[PATH_MAX + 1];
	int argc;
	int i = 0;
	
	tmp = getenv("SBOX_REDIR_LD_LIBRARY_PATH");

	if (!tmp) {
		fprintf(stderr, "Total failure to execute tools"
				"SBOX_REDIR_LD_LIBRARY_PATH not specified\n");
		exit(1);
	} else {
		host_libs = strdup(tmp);
	}

	tmp = getenv("SBOX_REDIR_LD_SO");
	if (!tmp) {
		fprintf(stderr, "Total failure to execute tools"
				"SBOX_REDIR_LD_SO not specified\n");
		exit(1);
	} else {
		ld_so = strdup(tmp);
	}

	memset(ld_so_buf, '\0', PATH_MAX + 1);
	memset(ld_so_basename, '\0', PATH_MAX + 1);

	if (readlink_nomap(ld_so, ld_so_buf, PATH_MAX) < 0) {
		if (errno == EINVAL) {
			/* it's not a symbolic link, so use it directly */
			strcpy(ld_so_basename, basename(ld_so));

		} else {
			/* something strange, bail out */
			perror("readlink(ld_so) failed badly. aborting\n");
			return -1;
		}
	} else {
		strcpy(ld_so_basename, basename(ld_so_buf));
	}

	binaryname = basename(strdup(file));
	
	/* if the file to be run is the dynamic loader itself, 
	 * run it straight
	 */

	if (strcmp(binaryname, ld_so_basename) == 0) {
		sb_next_execve(file, argv, envp);
		perror("failed to directly run the dynamic linker!\n");
		return -1;
	}

	argc = elem_count(argv);

	my_argv = (char **)calloc(4 + argc - 1 + 1, sizeof (char *));
	i = 0;
	my_argv[i++] = strdup(ld_so);
	my_argv[i++] = strdup("--library-path");
	my_argv[i++] = host_libs;
	my_argv[i++] = strdup(file);

	for (p = (char **)argv + 1; *p; p++)
		my_argv[i++] = strdup(*p);

	my_argv[i] = NULL;

	sb_next_execve(strdup(ld_so), my_argv, envp);

	fprintf(stderr, "sb2 ld_so_run_app(%s): %s\n", file, strerror(errno));
	return -11;
}

static int elf_hdr_match(Elf_Ehdr *ehdr, uint16_t match, int ei_data)
{
	int swap;

	if (ehdr->e_ident[EI_DATA] != ei_data)
		return 0;

#ifdef WORDS_BIGENDIAN
	swap = (ei_data == ELFDATA2LSB);
#else
	swap = (ei_data == ELFDATA2MSB);
#endif

	if (swap && ehdr->e_machine == byte_swap(match))
		return 1;

	if (!swap && ehdr->e_machine == match)
		return 1;

	return 0;
}

static enum binary_type inspect_binary(const char *filename)
{
	enum binary_type retval;
	int fd, phnum, j;
	struct stat status;
	char *region, *target_cpu;
	unsigned int ph_base, ph_frag, ei_data;
	uint16_t e_machine;
#ifdef __x86_64__
	int64_t reloc0;
#else
	int reloc0;
#endif
	Elf_Ehdr *ehdr;
	Elf_Phdr *phdr;

	retval = BIN_NONE;
	if (access_nomap_nolog(filename, X_OK) < 0) {
		int saved_errno = errno;
		char *sb1_bug_emulation_mode = getenv("SBOX_EMULATE_SB1_BUGS");
		if (sb1_bug_emulation_mode && 
		    strchr(sb1_bug_emulation_mode,'x')) {
			/* the old scratchbox didn't have the x-bit check, so 
			 * having R-bit set was enough to exec a file. That is 
			 * of course wrong, but unfortunately there are now 
			 * lots of packages out there that have various scripts 
			 * with wrong permissions.
			 * We'll provide a compatibility mode, so that broken 
			 * packages can be built.
			 *
			 * an 'x' in the env.var. means that we should not 
			 * worry about x-bits when checking exec parmissions
			*/
			if (access_nomap_nolog(filename, R_OK) < 0) {
				saved_errno = errno;
				SB_LOG(SB_LOGLEVEL_DEBUG, "no X or R "
					"permission for '%s'", filename);
				retval = BIN_INVALID;
				errno = saved_errno;
				goto _out;
			}
			SB_LOG(SB_LOGLEVEL_WARNING, 
				"X permission missing, but exec enabled by"
				" SB1 bug emulation mode ('%s')", filename);
		} else {
			/* The normal case:
			 * can't execute it. Possible errno codes from access() 
			 * are all possible from execve(), too, so there is no
			 * need to convert errno.
			*/
			SB_LOG(SB_LOGLEVEL_DEBUG, "no X permission for '%s'",
				filename);
			retval = BIN_INVALID;
			errno = saved_errno;
			goto _out;
		}
	}

	fd = open_nomap_nolog(filename, O_RDONLY, 0);
	if (fd < 0) {
		retval = BIN_HOST_DYNAMIC; /* can't peek in to look, assume dynamic */
		goto _out;
	}

	retval = BIN_UNKNOWN;

	if (fstat(fd, &status) < 0) {
		goto _out_close;
	}

	if ((status.st_mode & S_ISUID)) {
		SB_LOG(SB_LOGLEVEL_WARNING,
			"SUID bit set for '%s' (SB2 may be disabled)",
			filename);
	}

	if ((status.st_mode & S_ISGID)) {
		SB_LOG(SB_LOGLEVEL_WARNING,
			"SGID bit set for '%s' (SB2 may be disabled)",
			filename);
	}

	if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) {
		goto _out_close;
	}

	region = mmap(0, status.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (!region) {
		goto _out_close;
	}

	/* check for hashbang */

	if (region[0] == '#' && region[1] == '!') {
		retval = BIN_HASHBANG;
		goto _out_munmap;
	}

	ehdr = (Elf_Ehdr *) region;

	if (elf_hdr_match(ehdr, HOST_ELF_MACHINE, HOST_ELF_DATA)) {
		retval = BIN_HOST_STATIC;

		phnum = ehdr->e_phnum;
		reloc0 = ~0;
		ph_base = ehdr->e_phoff & PAGE_MASK;
		ph_frag = ehdr->e_phoff - ph_base;

		phdr = (Elf_Phdr *) (region + ph_base + ph_frag);

		for (j = phnum; --j >= 0; ++phdr)
			if (PT_LOAD == phdr->p_type && ~0 == reloc0)
				reloc0 = phdr->p_vaddr - phdr->p_offset;

		phdr -= phnum;

		for (j = phnum; --j >= 0; ++phdr) {
			if (PT_DYNAMIC != phdr->p_type)
				continue;

			retval = BIN_HOST_DYNAMIC;
		}
	} else {
		target_cpu = getenv("SBOX_CPU");
		if (!target_cpu)
			target_cpu = "arm";

		ei_data = ELFDATANONE;
		e_machine = EM_NONE;

		for (j = 0; (size_t) j < ARRAY_SIZE(target_table); j++) {
			const struct target_info *ti = &target_table[j];

			if (strncmp(target_cpu, ti->name, strlen(ti->name)))
				continue;

			ei_data = ti->default_byteorder;
			e_machine = ti->machine;

			if (ti->multi_byteorder &&
			    strlen(target_cpu) >= strlen(ti->name) + 2) {
				size_t len = strlen(target_cpu);
				const char *tail = target_cpu + len - 2;

				if (strcmp(tail, "eb") == 0)
					ei_data = ELFDATA2MSB;
				else if (strcmp(tail, "el") == 0)
					ei_data = ELFDATA2LSB;
			}

			break;
		}

		if (elf_hdr_match(ehdr, e_machine, ei_data))
			retval = BIN_TARGET;
	}

_out_munmap:
	munmap(region, status.st_size);
_out_close:
	close(fd);
_out:
	return retval;
}

static int run_hashbang(
	const char *mapped_file,
	const char *orig_file,
	char *const *argv,
	char *const *envp)
{
	int argc, fd, c, i, j, n, ret;
	char ch;
	char *ptr, *mapped_interpreter;
	char **new_argv;
	char hashbang[SBOX_MAXPATH]; /* only 60 needed on linux, just be safe */
	char interpreter[SBOX_MAXPATH];

	if ((fd = open_nomap(mapped_file, O_RDONLY)) < 0) {
		/* unexpected error, just run it */
		return run_app(mapped_file, argv, envp);
	}

	if ((c = read(fd, &hashbang[0], SBOX_MAXPATH - 1)) < 2) {
		/* again unexpected error, close fd and run it */
		close(fd);
		return run_app(mapped_file, argv, envp);
	}

	argc = elem_count(argv);

	/* extra element for hashbang argument */
	new_argv = calloc(argc + 3, sizeof(char *));

	/* skip any initial whitespace following "#!" */
	for (i = 2; (hashbang[i] == ' ' 
			|| hashbang[i] == '\t') && i < c; i++)
		;

	for (n = 0, j = i; i < c; i++) {
		ch = hashbang[i];
		if (hashbang[i] == 0
			|| hashbang[i] == ' '
			|| hashbang[i] == '\t'
			|| hashbang[i] == '\n') {
			hashbang[i] = 0;
			if (i > j) {
				if (n == 0) {
					ptr = &hashbang[j];
					strcpy(interpreter, ptr);
					new_argv[n++] = strdup(interpreter);
				} else {
					new_argv[n++] = strdup(&hashbang[j]);
					/* this was the one and only
					 * allowed argument for the
					 * interpreter
					 */
					break;
				}
			}
			j = i + 1;
		}
		if (ch == '\n' || ch == 0) break;
	}

	mapped_interpreter = scratchbox_path("execve", interpreter, 
		NULL/*RO-flag addr.*/, 0/*dont_resolve_final_symlink*/);
	SB_LOG(SB_LOGLEVEL_DEBUG, "run_hashbang(): interpreter=%s,"
			"mapped_interpreter=%s", interpreter,
			mapped_interpreter);
	new_argv[n++] = strdup(orig_file); /* the unmapped script path */

	for (i = 1; argv[i] != NULL && i < argc; ) {
		new_argv[n++] = argv[i++];
	}

	new_argv[n] = NULL;

	/* feed this through do_exec to let it deal with
	 * cpu transparency etc.
	 */
	ret = do_exec("run_hashbang", mapped_interpreter, new_argv, envp);

	if (mapped_interpreter) free(mapped_interpreter);
	return ret;
}

static char ***duplicate_argv(char *const *argv)
{
	int	argc = elem_count(argv);
	char	**p;
	int	i;
	char	***my_argv;

	my_argv = malloc(sizeof(char **));
	*my_argv = (char **)calloc(argc + 1, sizeof(char *));
	for (i = 0, p = (char **)argv; *p; p++) {
		(*my_argv)[i++] = strdup(*p);
	}
	(*my_argv)[i] = NULL;

	return(my_argv);
}

static char ***prepare_envp_for_do_exec(char *binaryname, char *const *envp)
{
	char	**p;
	int	envc = 0;
	char	***my_envp;
	int	has_ld_preload = 0;
	int	i;
	char	*tmp;

	/* if we have LD_PRELOAD env var set, make sure the new my_envp
	 * has it as well
	 */

	/* count the environment variables and arguments, also check
	 * for LD_PRELOAD
	 */
	for (p=(char **)envp; *p; p++, envc++) {
		if (strncmp("LD_PRELOAD=", *p, strlen("LD_PRELOAD=")) == 0)
			has_ld_preload = 1;
	}

	my_envp = malloc(sizeof(char **));

	if (has_ld_preload || !getenv("LD_PRELOAD")) {
		*my_envp = (char **)calloc(envc + 2, sizeof(char *));
	} else {
		*my_envp = (char **)calloc(envc + 3, sizeof(char *));
	}

	/* __SB2_BINARYNAME is used to communicate the binary name
	 * to the new process so that it's available even before
	 * its main function is called
	 */
	i = strlen(binaryname) + strlen("__SB2_BINARYNAME=") + 1;
	tmp = malloc(i * sizeof(char));
	strcpy(tmp, "__SB2_BINARYNAME=");
	strcat(tmp, binaryname);

	for (i = 0, p=(char **)envp; *p; p++) {
		if (strncmp(*p, "__SB2_BINARYNAME=",
				strlen("__SB2_BINARYNAME=")) == 0) {
			/* this is current process' name, skip it */
			continue;
		}

		(*my_envp)[i++] = strdup(*p);
	}
	(*my_envp)[i++] = tmp; /* add the new process' name */

	/* If our environ has LD_PRELOAD, but the given envp doesn't,
	 * add it.
	 */
	if (!has_ld_preload && getenv("LD_PRELOAD")) {
		tmp = malloc(strlen("LD_PRELOAD=")
				+ strlen(getenv("LD_PRELOAD")) + 1);
		if (!tmp)
			exit(1);
		strcpy(tmp, "LD_PRELOAD=");
		strcat(tmp, getenv("LD_PRELOAD"));
		(*my_envp)[i++] = strdup(tmp);
		free(tmp);
	}
	(*my_envp)[i] = NULL;

	return(my_envp);
}

/* compare vectors of strings and log if there are any changes.
 * TO BE IMPROVED: a more clever algorithm would be nice, something
 * that would log only the modified parts... now this displays 
 * everything if something was changed, which easily creates 
 * a *lot* of noise if SB_LOGLEVEL_NOISE is enabled.
*/
static void compare_and_log_strvec_changes(const char *vecname, 
	char *const *orig_strv, char *const *new_strv) 
{
	char *const *ptr2old = orig_strv;
	char *const *ptr2new = new_strv;
	int	strv_modified = 0;

	while (*ptr2old && *ptr2new) {
		if (strcmp(*ptr2old, *ptr2new)) {
			strv_modified = 1;
			break;
		}
		ptr2old++, ptr2new++;
	}
	if (!strv_modified && (*ptr2old || *ptr2new)) {
		strv_modified = 1;
	}
	if (strv_modified) {
		SB_LOG(SB_LOGLEVEL_DEBUG, "%s[] was modified", vecname); 
		if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE)) {
			int	i;
			for (i = 0, ptr2new = new_strv; 
			    *ptr2new;
			    i++, ptr2new++) {
				SB_LOG(SB_LOGLEVEL_NOISE, 
					"[%d]='%s'", i,
					*ptr2new);
			}
		}
	} else {
		SB_LOG(SB_LOGLEVEL_NOISE, 
			"%s[] was not modified", vecname); 
	}
}

static int strvec_contains(char *const *strvec, const char *id)
{
	char *const *ptr2vec = strvec;

	while (*ptr2vec) {
		if (!strcmp(*ptr2vec, id)) {
			return(1);
		}
		ptr2vec++;
	}
	return(0);
}

int do_exec(const char *exec_fn_name, const char *file,
		char *const *argv, char *const *envp)
{
	char ***my_envp, ***my_argv, **my_file;
	char ***my_envp_copy = NULL; /* used only for debug log */
	char *binaryname, *tmp, *mapped_file;
	int err = 0;
	enum binary_type type;

	(void)exec_fn_name; /* not yet used */

	if (getenv("SBOX_DISABLE_MAPPING")) {
		/* just run it, don't worry, be happy! */
		return sb_next_execve(file, argv, envp);
	}
	
	tmp = strdup(file);
	binaryname = strdup(basename(tmp));
	free(tmp);
	
	my_file = malloc(sizeof(char *));
	*my_file = strdup(file);

	my_envp = prepare_envp_for_do_exec(binaryname, envp);
	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_DEBUG)) {
		/* create a copy of intended environment for logging,
		 * before sb_execve_preprocess() gets control */ 
		my_envp_copy = prepare_envp_for_do_exec(binaryname, envp);
	}

	my_argv = duplicate_argv(argv);

	if ((err = sb_execve_preprocess(my_file, my_argv, my_envp)) != 0) {
		SB_LOG(SB_LOGLEVEL_ERROR, "argvenvp processing error %i", err);
	}

	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_DEBUG)) {
		/* find out and log if sb_execve_preprocess() did something */
		compare_and_log_strvec_changes("argv", argv, *my_argv);
		compare_and_log_strvec_changes("envp", *my_envp_copy, *my_envp);
	}

	/* test if mapping is enabled during the exec()..
	 * (host-* tools disable it)
	*/
	if (strvec_contains(*my_envp, "SBOX_DISABLE_MAPPING=1")) {
		SB_LOG(SB_LOGLEVEL_DEBUG,
			"do_exec(): mapping disabled, *my_file = %s",
			*my_file);
		mapped_file = strdup(*my_file);
	} else {
		/* now we have to do path mapping for *my_file to find exactly
		 * what is the path we're supposed to deal with
		 */

		mapped_file = scratchbox_path("do_exec", *my_file,
			NULL/*RO-flag addr.*/, 0/*dont_resolve_final_symlink*/);
		SB_LOG(SB_LOGLEVEL_DEBUG,
			"do_exec(): *my_file = %s, mapped_file = %s",
			*my_file, mapped_file);
	}

	/* inspect the completely mangled filename */
	type = inspect_binary(mapped_file);

	switch (type) {
		case BIN_HASHBANG:
			SB_LOG(SB_LOGLEVEL_DEBUG, "Exec/hashbang %s", mapped_file);
			return run_hashbang(mapped_file, *my_file,
					*my_argv, *my_envp);
		case BIN_HOST_DYNAMIC:
			SB_LOG(SB_LOGLEVEL_DEBUG, "Exec/host-dynamic %s",
					mapped_file);
			tmp = getenv("SBOX_REDIR_LD_SO");
			if (tmp && strlen(tmp) > 0)
				return ld_so_run_app(mapped_file, *my_argv, *my_envp);
			else
				return run_app(mapped_file, *my_argv, *my_envp);

		case BIN_HOST_STATIC:
			SB_LOG(SB_LOGLEVEL_WARNING, "Executing statically "
					"linked native binary %s",
					mapped_file);
			return run_app(mapped_file, *my_argv, *my_envp);
		case BIN_TARGET:
			SB_LOG(SB_LOGLEVEL_DEBUG, "Exec/target %s",
					mapped_file);

			return run_cputransparency(mapped_file, *my_file,
					*my_argv, *my_envp);

		case BIN_INVALID: /* = can't be executed, no X permission */
	 		/* don't even try to exec, errno has been set.*/
			return (-1);

		case BIN_NONE:
		case BIN_UNKNOWN:
			SB_LOG(SB_LOGLEVEL_ERROR,
				"Unidentified executable detected (%s)",
				mapped_file);
			break;
	}

	return sb_next_execve(mapped_file, *my_argv, *my_envp);
}

/* ---------- */
int sb2show__execve_mods__(
	char *file,
	char *const *orig_argv, char *const *orig_envp,
	char **new_file, char ***new_argv, char ***new_envp)
{
	char *binaryname, *tmp;
	int err = 0;
	char ***my_envp, ***my_argv, **my_file;

	SB_LOG(SB_LOGLEVEL_DEBUG, "%s '%s'", __func__, orig_argv[0]);

	tmp = strdup(file);
	binaryname = strdup(basename(tmp));
	free(tmp);
	
	my_file = malloc(sizeof(char *));
	*my_file = strdup(file);

	my_envp = prepare_envp_for_do_exec(binaryname, orig_envp);
	my_argv = duplicate_argv(orig_argv);

	if ((err = sb_execve_preprocess(my_file, my_argv, my_envp)) != 0) {
		SB_LOG(SB_LOGLEVEL_ERROR, "argvenvp processing error %i", err);
		
		*new_file = NULL;
		*new_argv = NULL;
		*new_envp = NULL;
	} else {
		*new_file = *my_file;
		*new_argv = *my_argv;
		*new_envp = *my_envp;
	}

	return(0);
}

