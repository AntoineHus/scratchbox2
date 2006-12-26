#ifndef __SB2_H
#define __SB2_H

#define DBGOUT(fmt...) fprintf(stderr, fmt)

extern int (*next_execve) (const char *filename, char *const argv [], char *const envp[]);

int ld_so_run_app(char *file, char **argv, char *const *envp);
int run_app(char *file, char **argv, char *const *envp);
int run_cputransparency(char *file, char **argv, char *const *envp);

int run_sbrsh(char *sbrsh_bin, char *target_root, char *file, char **argv, char *const *envp);
int run_qemu(char *qemu_bin, char *target_root, char *file, char **argv, char *const *envp);


#endif
