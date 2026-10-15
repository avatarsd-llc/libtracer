/**
 * @file
 * @brief Run a command with `membarrier(2)` refused (#2121).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The qsbr reclaim policy, built with `kQsbrAsymmetricFence`, registers for
 * `MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED` and, when the kernel refuses, keeps the fenced
 * bracket entry for good (`qsbr.hpp` §qsbr_asym). A Linux CI runner always accepts that
 * registration, so the fallback ran only on targets without `membarrier`. This launcher
 * installs a seccomp filter that fails every `membarrier` call with EPERM, as a container or
 * a sandbox would, and then executes the command. The filter is inherited across `fork` and
 * `execve`, so everything the command starts sees the refusal too.
 *
 * It needs no privilege: it sets `no_new_privs` first, which is what lets an unprivileged
 * process install a filter. Before it executes the command it checks that the filter took:
 * a `membarrier` query must now fail with EPERM.
 *
 *     cc -O2 -Wall -Wextra -Werror -o refuse_membarrier tools/refuse_membarrier.c
 *     ./refuse_membarrier ctest --test-dir build
 */
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/membarrier.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define REFUSE_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define REFUSE_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "refuse_membarrier: add this architecture's AUDIT_ARCH_* value"
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [args...]\n", argv[0]);
        return 2;
    }
    /* A call made under another ABI than this binary's is let through: its syscall numbers
     * differ, so matching them against this ABI's membarrier number would be wrong. */
    struct sock_filter prog[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, REFUSE_AUDIT_ARCH, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_membarrier, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog fprog = {.len = (unsigned short)(sizeof prog / sizeof prog[0]),
                               .filter = prog};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("refuse_membarrier: PR_SET_NO_NEW_PRIVS");
        return 3;
    }
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
        perror("refuse_membarrier: seccomp");
        return 3;
    }
    errno = 0;
    if (syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0, 0) != -1 || errno != EPERM) {
        fprintf(stderr, "refuse_membarrier: the filter did not take: membarrier still answers\n");
        return 3;
    }
    fprintf(stderr, "refuse_membarrier: membarrier(2) refused with EPERM for %s\n", argv[1]);
    execvp(argv[1], argv + 1);
    fprintf(stderr, "refuse_membarrier: exec %s: %s\n", argv[1], strerror(errno));
    return 127;
}
