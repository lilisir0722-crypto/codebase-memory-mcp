/*
 * ext_bidir.c — Bidirectional subprocess communication + PATH lookup.
 */
#include "extensions/ext_bidir.h"

#ifdef _WIN32
/* TODO: Windows implementation with CreateProcess + named pipes */
#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "foundation/compat.h"

struct cbm_bidir {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
};

static void close_fds_above(int min_fd) {
#if defined(__linux__)
    /* Try close_range (kernel 5.9+), fall back to loop */
    extern int close_range(unsigned int first, unsigned int last, unsigned int flags)
        __attribute__((weak));
    if (close_range && close_range((unsigned)min_fd, ~0U, 0) == 0) return;
#endif
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 256;
    for (int fd = min_fd; fd < (int)max_fd; fd++) {
        (void)close(fd);
    }
}

cbm_bidir_t *cbm_bidir_spawn(const char *const *argv, int stderr_mode) {
    if (!argv || !argv[0]) return NULL;

    int pipe_in[2], pipe_out[2];
    if (cbm_pipe(pipe_in) < 0) return NULL;
    if (cbm_pipe(pipe_out) < 0) {
        close(pipe_in[0]);
        close(pipe_in[1]);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child */
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);

        if (stderr_mode == CBM_BIDIR_STDERR_LOG) {
            int log_fd = open("/tmp/cbm-clangd-stderr.log",
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        close_fds_above(3);

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent */
    close(pipe_in[0]);
    close(pipe_out[1]);

    cbm_bidir_t *b = calloc(1, sizeof(*b));
    if (!b) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    b->pid = pid;
    b->stdin_fd = pipe_in[1];
    b->stdout_fd = pipe_out[0];
    return b;
}

int cbm_bidir_read(cbm_bidir_t *b, void *buf, size_t buf_size) {
    if (!b || b->stdout_fd < 0) return -1;
    ssize_t n;
    do { n = read(b->stdout_fd, buf, buf_size); } while (n < 0 && errno == EINTR);
    return (int)n;
}

int cbm_bidir_read_timeout(cbm_bidir_t *b, void *buf, size_t buf_size, int timeout_ms) {
    if (!b || b->stdout_fd < 0) return -1;
    struct pollfd pfd = {.fd = b->stdout_fd, .events = POLLIN};
    int pr;
    do { pr = poll(&pfd, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) return 0;
    return cbm_bidir_read(b, buf, buf_size);
}

int cbm_bidir_write(cbm_bidir_t *b, const void *buf, size_t len) {
    if (!b || b->stdin_fd < 0) return -1;
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(b->stdin_fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return (int)len;
}

int cbm_bidir_shutdown(cbm_bidir_t *b) {
    if (!b) return -1;
    if (b->stdin_fd >= 0) {
        close(b->stdin_fd);
        b->stdin_fd = -1;
    }
    int st = 0;
    pid_t r;
    do { r = waitpid(b->pid, &st, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void cbm_bidir_free(cbm_bidir_t *b) {
    if (!b) return;
    if (b->stdin_fd >= 0) close(b->stdin_fd);
    if (b->stdout_fd >= 0) close(b->stdout_fd);
    kill(b->pid, SIGKILL);
    waitpid(b->pid, NULL, 0);
    free(b);
}

long cbm_bidir_pid(const cbm_bidir_t *b) {
    return b ? (long)b->pid : -1;
}

char *cbm_which(const char *name) {
    if (!name || !name[0]) return NULL;
    const char *path_env = getenv("PATH");
    if (!path_env) return NULL;

    char *paths = strdup(path_env);
    if (!paths) return NULL;

    char *saveptr = NULL;
    char *dir = strtok_r(paths, ":", &saveptr);
    while (dir) {
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (n > 0 && (size_t)n < sizeof(full) && access(full, X_OK) == 0) {
            free(paths);
            return strdup(full);
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(paths);
    return NULL;
}

#endif /* _WIN32 */
