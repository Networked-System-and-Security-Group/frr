// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Controlled process executor for MIDR traceroute jobs.
 */

#include <zebra.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef HAVE_SPAWN_H
#include <spawn.h>
#endif
#include <sys/stat.h>
#include <sys/wait.h>

#include "log.h"

#include "bgpd/midr_trace_exec.h"
#include "bgpd/midr_trace_scheduler.h"

#ifndef MIDR_TRACEROUTE_PATH
#define MIDR_TRACEROUTE_PATH "/usr/bin/traceroute"
#endif

#define MIDR_TRACE_EXEC_MAX MIDR_TRACE_CONCURRENCY_HARD_MAX

#if defined(HAVE_SPAWN_H) && defined(HAVE_POSIX_SPAWN)                    \
	&& defined(POSIX_SPAWN_SETPGROUP) && defined(O_CLOEXEC)
#define MIDR_TRACE_HAVE_SPAWN_TYPES 1
#else
#define MIDR_TRACE_HAVE_SPAWN_TYPES 0
#endif

static struct midr_trace_exec *midr_trace_exec_registry[MIDR_TRACE_EXEC_MAX];

static bool midr_trace_exec_compile_supported(void)
{
#if defined(HAVE_SPAWN_H) && defined(HAVE_POSIX_SPAWN)                  \
	&& defined(HAVE_PIPE2) && defined(POSIX_SPAWN_SETPGROUP)         \
	&& (defined(HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP)       \
	    || defined(POSIX_SPAWN_CLOEXEC_DEFAULT))                     \
	&& defined(F_DUPFD_CLOEXEC) && defined(O_CLOEXEC)
	return true;
#else
	return false;
#endif
}

static bool midr_trace_exec_directory_valid(const char *path)
{
	char prefix[PATH_MAX];
	struct stat st;
	char *slash;

	if (!path || path[0] != '/'
	    || strlcpy(prefix, path, sizeof(prefix)) >= sizeof(prefix))
		return false;

	if (stat("/", &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0
	    || (st.st_mode & (S_IWGRP | S_IWOTH)))
		return false;

	/*
	 * Every directory that can rename a path component must be root-owned
	 * and not group/world writable.  Root is trusted; an unprivileged bgpd
	 * account must not be able to replace the checked executable between
	 * validation and posix_spawn().
	 */
	slash = strchr(prefix + 1, '/');
	while (slash) {
		*slash = '\0';
		if (stat(prefix, &st) != 0 || !S_ISDIR(st.st_mode)
		    || st.st_uid != 0
		    || (st.st_mode & (S_IWGRP | S_IWOTH)))
			return false;
		*slash = '/';
		slash = strchr(slash + 1, '/');
	}
	return true;
}

static bool midr_trace_exec_resolve_path(char *resolved,
					 size_t resolved_len)
{
	char canonical[PATH_MAX];
	struct stat st;

	if (!resolved || !resolved_len || MIDR_TRACEROUTE_PATH[0] != '/')
		return false;
	if (!realpath(MIDR_TRACEROUTE_PATH, canonical))
		return false;
	if (!midr_trace_exec_directory_valid(MIDR_TRACEROUTE_PATH)
	    || !midr_trace_exec_directory_valid(canonical))
		return false;
	if (stat(canonical, &st) != 0 || !S_ISREG(st.st_mode)
	    || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH))
	    || access(canonical, X_OK) != 0)
		return false;
	if (strlcpy(resolved, canonical, resolved_len) >= resolved_len)
		return false;
	return true;
}

bool midr_trace_exec_supported(void)
{
	char resolved[PATH_MAX];

	return midr_trace_exec_compile_supported()
	       && midr_trace_exec_resolve_path(resolved, sizeof(resolved));
}

void midr_trace_exec_init(struct midr_trace_exec *exec)
{
	if (!exec)
		return;

	memset(exec, 0, sizeof(*exec));
	exec->pid = -1;
	exec->pgid = -1;
	exec->stdout_fd = -1;
	exec->stderr_fd = -1;
}

#if MIDR_TRACE_HAVE_SPAWN_TYPES
static int midr_trace_exec_registry_slot(void)
{
	unsigned int i;

	for (i = 0; i < array_size(midr_trace_exec_registry); i++)
		if (!midr_trace_exec_registry[i])
			return (int)i;
	return -1;
}
#endif

static void midr_trace_exec_unregister(struct midr_trace_exec *exec)
{
	unsigned int i;

	if (!exec || !exec->registered)
		return;

	for (i = 0; i < array_size(midr_trace_exec_registry); i++) {
		if (midr_trace_exec_registry[i] != exec)
			continue;
		midr_trace_exec_registry[i] = NULL;
		break;
	}
	exec->registered = false;
}

#if MIDR_TRACE_HAVE_SPAWN_TYPES
static int midr_trace_exec_fd_above_stdio(int *fdp)
{
	if (*fdp > STDERR_FILENO)
		return 0;

#ifndef F_DUPFD_CLOEXEC
	errno = ENOTSUP;
	return -1;
#else
	int replacement =
		fcntl(*fdp, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);

	if (replacement < 0)
		return -1;

	close(*fdp);
	*fdp = replacement;
	return 0;
#endif
}
#endif

static void midr_trace_exec_close_fd(int *fdp)
{
	if (*fdp >= 0)
		close(*fdp);
	*fdp = -1;
}

#if MIDR_TRACE_HAVE_SPAWN_TYPES
static int midr_trace_exec_make_pipe(int fds[2])
{
#ifndef HAVE_PIPE2
	(void)fds;
	errno = ENOTSUP;
	return -1;
#else
	int flags;

	if (pipe2(fds, O_CLOEXEC) < 0)
		return -1;

	if (midr_trace_exec_fd_above_stdio(&fds[0]) < 0
	    || midr_trace_exec_fd_above_stdio(&fds[1]) < 0) {
		midr_trace_exec_close_fd(&fds[0]);
		midr_trace_exec_close_fd(&fds[1]);
		return -1;
	}

	flags = fcntl(fds[0], F_GETFL);
	if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
		midr_trace_exec_close_fd(&fds[0]);
		midr_trace_exec_close_fd(&fds[1]);
		return -1;
	}
	return 0;
#endif
}
#endif

#if MIDR_TRACE_HAVE_SPAWN_TYPES
static int midr_trace_exec_add_file_actions(
	posix_spawn_file_actions_t *actions, int nullfd, const int stdout_pipe[2],
	const int stderr_pipe[2])
{
	int rc;

	rc = posix_spawn_file_actions_adddup2(actions, nullfd, STDIN_FILENO);
	if (rc)
		return rc;
	rc = posix_spawn_file_actions_adddup2(actions, stdout_pipe[1],
					      STDOUT_FILENO);
	if (rc)
		return rc;
	rc = posix_spawn_file_actions_adddup2(actions, stderr_pipe[1],
					      STDERR_FILENO);
	if (rc)
		return rc;

	rc = posix_spawn_file_actions_addclose(actions, nullfd);
	if (rc)
		return rc;
	rc = posix_spawn_file_actions_addclose(actions, stdout_pipe[0]);
	if (rc)
		return rc;
	rc = posix_spawn_file_actions_addclose(actions, stdout_pipe[1]);
	if (rc)
		return rc;
	rc = posix_spawn_file_actions_addclose(actions, stderr_pipe[0]);
	if (rc)
		return rc;
	rc = posix_spawn_file_actions_addclose(actions, stderr_pipe[1]);
	if (rc)
		return rc;

#ifdef HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
	rc = posix_spawn_file_actions_addclosefrom_np(actions,
						      STDERR_FILENO + 1);
	if (rc)
		return rc;
#endif
	return 0;
}

static int midr_trace_exec_attr_init(posix_spawnattr_t *attr,
				     const sigset_t *child_mask)
{
	sigset_t defaults;
	short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK
		      | POSIX_SPAWN_SETSIGDEF;
	int rc;

#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
#ifndef HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
#endif

	rc = posix_spawnattr_init(attr);
	if (rc)
		return rc;
	rc = posix_spawnattr_setpgroup(attr, 0);
	if (rc)
		goto fail;
	rc = posix_spawnattr_setsigmask(attr, child_mask);
	if (rc)
		goto fail;

	sigemptyset(&defaults);
	sigaddset(&defaults, SIGCHLD);
	sigaddset(&defaults, SIGHUP);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGPIPE);
	sigaddset(&defaults, SIGQUIT);
	sigaddset(&defaults, SIGTERM);
	rc = posix_spawnattr_setsigdefault(attr, &defaults);
	if (rc)
		goto fail;
	rc = posix_spawnattr_setflags(attr, flags);
	if (rc)
		goto fail;
	return 0;

fail:
	posix_spawnattr_destroy(attr);
	return rc;
}

static bool midr_trace_exec_addrstr(const struct prefix *target, char *buf,
				    size_t buflen)
{
	const void *addr;

	if (!target)
		return false;
	if (target->family == AF_INET)
		addr = &target->u.prefix4;
	else if (target->family == AF_INET6)
		addr = &target->u.prefix6;
	else
		return false;
	return inet_ntop(target->family, addr, buf, buflen) != NULL;
}

int midr_trace_exec_start(struct midr_trace_exec *exec,
			  const struct prefix *target, void *owner,
			  void (*reaped_cb)(void *owner))
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	sigset_t block_mask;
	sigset_t old_mask;
	sigset_t child_mask;
	int stdout_pipe[2] = {-1, -1};
	int stderr_pipe[2] = {-1, -1};
	int nullfd = -1;
	int registry_slot;
	bool actions_initialized = false;
	bool attr_initialized = false;
	bool mask_blocked = false;
	char address[INET6_ADDRSTRLEN];
	char executable[PATH_MAX];
	char *argv4[] = {executable, (char *)"-n",
			 (char *)"-m", (char *)"30", (char *)"-q",
			 (char *)"1", (char *)"-w", (char *)"1",
			 address, NULL};
	char *argv6[] = {executable, (char *)"-6",
			 (char *)"-n", (char *)"-m", (char *)"30",
			 (char *)"-q", (char *)"1", (char *)"-w",
			 (char *)"1", address, NULL};
	char *envp[] = {(char *)"LC_ALL=C", NULL};
	char **argv;
	pid_t pid = -1;
	int rc = 0;

	if (!exec || !owner || !reaped_cb || exec->spawned
	    || !midr_trace_exec_addrstr(target, address, sizeof(address)))
		return EINVAL;
	if (!midr_trace_exec_compile_supported()
	    || !midr_trace_exec_resolve_path(executable,
					      sizeof(executable)))
		return ENOTSUP;

	registry_slot = midr_trace_exec_registry_slot();
	if (registry_slot < 0)
		return EAGAIN;
	if (midr_trace_exec_make_pipe(stdout_pipe) < 0
	    || midr_trace_exec_make_pipe(stderr_pipe) < 0) {
		rc = errno;
		goto out;
	}

	nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (nullfd < 0) {
		rc = errno;
		goto out;
	}
	if (midr_trace_exec_fd_above_stdio(&nullfd) < 0) {
		rc = errno;
		goto out;
	}

	rc = posix_spawn_file_actions_init(&actions);
	if (rc)
		goto out;
	actions_initialized = true;
	rc = midr_trace_exec_add_file_actions(&actions, nullfd, stdout_pipe,
					      stderr_pipe);
	if (rc)
		goto out;

	sigemptyset(&block_mask);
	sigaddset(&block_mask, SIGCHLD);
	rc = pthread_sigmask(SIG_BLOCK, &block_mask, &old_mask);
	if (rc)
		goto out;
	mask_blocked = true;

	/*
	 * FRR may keep its handled signals blocked for event-loop delivery.
	 * The child must not inherit that mask or SIGTERM grace would be
	 * ineffective.
	 */
	sigemptyset(&child_mask);
	rc = midr_trace_exec_attr_init(&attr, &child_mask);
	if (rc)
		goto out;
	attr_initialized = true;

	argv = target->family == AF_INET6 ? argv6 : argv4;
	rc = posix_spawn(&pid, executable, &actions, &attr, argv,
			 envp);
	if (rc)
		goto out;

	exec->pid = pid;
	exec->pgid = pid;
	exec->stdout_fd = stdout_pipe[0];
	exec->stderr_fd = stderr_pipe[0];
	exec->spawned = true;
	exec->owner = owner;
	exec->reaped_cb = reaped_cb;
	exec->registered = true;
	midr_trace_exec_registry[registry_slot] = exec;
	stdout_pipe[0] = -1;
	stderr_pipe[0] = -1;

out:
	midr_trace_exec_close_fd(&stdout_pipe[0]);
	midr_trace_exec_close_fd(&stdout_pipe[1]);
	midr_trace_exec_close_fd(&stderr_pipe[0]);
	midr_trace_exec_close_fd(&stderr_pipe[1]);
	midr_trace_exec_close_fd(&nullfd);
	if (attr_initialized)
		posix_spawnattr_destroy(&attr);
	if (actions_initialized)
		posix_spawn_file_actions_destroy(&actions);
	if (mask_blocked) {
		int mask_rc = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);

		if (mask_rc)
			zlog_err("MIDR traceroute: failed to restore signal mask: %s",
				 safe_strerror(mask_rc));
	}
	return rc;
}
#else
int midr_trace_exec_start(struct midr_trace_exec *exec,
			  const struct prefix *target, void *owner,
			  void (*reaped_cb)(void *owner))
{
	(void)exec;
	(void)target;
	(void)owner;
	(void)reaped_cb;
	return ENOTSUP;
}
#endif

int midr_trace_exec_stream_fd(const struct midr_trace_exec *exec,
			      enum midr_trace_exec_stream stream)
{
	if (!exec)
		return -1;
	return stream == MIDR_TRACE_EXEC_STDERR ? exec->stderr_fd
					       : exec->stdout_fd;
}

ssize_t midr_trace_exec_read(struct midr_trace_exec *exec,
			     enum midr_trace_exec_stream stream, void *buf,
			     size_t buflen)
{
	int fd = midr_trace_exec_stream_fd(exec, stream);

	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	return read(fd, buf, buflen);
}

void midr_trace_exec_close_stream(struct midr_trace_exec *exec,
				  enum midr_trace_exec_stream stream)
{
	if (!exec)
		return;
	if (stream == MIDR_TRACE_EXEC_STDERR)
		midr_trace_exec_close_fd(&exec->stderr_fd);
	else
		midr_trace_exec_close_fd(&exec->stdout_fd);
}

void midr_trace_exec_close_streams(struct midr_trace_exec *exec)
{
	if (!exec)
		return;
	midr_trace_exec_close_stream(exec, MIDR_TRACE_EXEC_STDOUT);
	midr_trace_exec_close_stream(exec, MIDR_TRACE_EXEC_STDERR);
}

int midr_trace_exec_signal(struct midr_trace_exec *exec, int signo)
{
	int group_error;

	if (!exec || !exec->spawned || exec->child_reaped || exec->pgid <= 0)
		return ESRCH;
	if (kill(-exec->pgid, signo) == 0)
		return 0;

	group_error = errno;
	/*
	 * The child is created as its process-group leader, but retain a
	 * tracked-PID fallback for an unexpected group invariant failure.  This
	 * never broadens signaling beyond the PID registered by this executor.
	 */
	if (exec->pid > 0 && kill(exec->pid, signo) == 0)
		return 0;
	if (group_error != ESRCH)
		return group_error;
	return errno;
}

static enum midr_trace_exec_reap_rc
midr_trace_exec_reap_nonblocking(struct midr_trace_exec *exec)
{
	pid_t rc;
	int status;

	if (!exec || !exec->spawned)
		return MIDR_TRACE_EXEC_REAP_ERROR;
	if (exec->child_reaped)
		return MIDR_TRACE_EXEC_REAP_DONE;

	do {
		rc = waitpid(exec->pid, &status, WNOHANG);
	} while (rc < 0 && errno == EINTR);

	if (rc == 0)
		return MIDR_TRACE_EXEC_REAP_ALIVE;
	if (rc == exec->pid) {
		exec->wait_status = status;
		exec->wait_status_valid = true;
		exec->child_reaped = true;
		return MIDR_TRACE_EXEC_REAP_DONE;
	}
	if (rc < 0 && errno == ECHILD) {
		exec->reap_consistency_error = true;
		exec->child_reaped = true;
		return MIDR_TRACE_EXEC_REAP_DONE;
	}
	return MIDR_TRACE_EXEC_REAP_ERROR;
}

enum midr_trace_exec_reap_rc
midr_trace_exec_reap_one(struct midr_trace_exec *exec)
{
	return midr_trace_exec_reap_nonblocking(exec);
}

void midr_trace_exec_sigchld(void)
{
	struct midr_trace_exec *snapshot[MIDR_TRACE_EXEC_MAX];
	enum midr_trace_exec_reap_rc results[MIDR_TRACE_EXEC_MAX];
	int errors[MIDR_TRACE_EXEC_MAX] = {};
	size_t count = 0;
	size_t i;

	for (i = 0; i < array_size(midr_trace_exec_registry); i++) {
		struct midr_trace_exec *exec = midr_trace_exec_registry[i];

		if (!exec || exec->child_reaped)
			continue;
		snapshot[count++] = exec;
	}

	for (i = 0; i < count; i++) {
		results[i] = midr_trace_exec_reap_one(snapshot[i]);
		if (results[i] == MIDR_TRACE_EXEC_REAP_ERROR)
			errors[i] = errno;
	}

	for (i = 0; i < count; i++) {
		struct midr_trace_exec *exec = snapshot[i];

		if (results[i] == MIDR_TRACE_EXEC_REAP_DONE) {
			if (exec->reap_consistency_error)
				zlog_err("MIDR traceroute: PID %jd was not waitable",
					 (intmax_t)exec->pid);
			if (exec->reaped_cb)
				exec->reaped_cb(exec->owner);
		} else if (results[i] == MIDR_TRACE_EXEC_REAP_ERROR) {
			zlog_err("MIDR traceroute: waitpid(%jd) failed: %s",
				 (intmax_t)exec->pid,
				 safe_strerror(errors[i]));
		}
	}
}

void midr_trace_exec_reset(struct midr_trace_exec *exec)
{
	if (!exec)
		return;
	midr_trace_exec_unregister(exec);
	midr_trace_exec_close_streams(exec);
	midr_trace_exec_init(exec);
}
