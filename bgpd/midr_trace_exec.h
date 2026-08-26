// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Controlled process executor for MIDR traceroute jobs.
 */

#ifndef _FRR_MIDR_TRACE_EXEC_H
#define _FRR_MIDR_TRACE_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "prefix.h"

#ifdef __cplusplus
extern "C" {
#endif

enum midr_trace_exec_stream {
	MIDR_TRACE_EXEC_STDOUT,
	MIDR_TRACE_EXEC_STDERR,
};

enum midr_trace_exec_reap_rc {
	MIDR_TRACE_EXEC_REAP_ALIVE,
	MIDR_TRACE_EXEC_REAP_DONE,
	MIDR_TRACE_EXEC_REAP_ERROR,
};

struct midr_trace_exec {
	pid_t pid;
	pid_t pgid;
	int stdout_fd;
	int stderr_fd;
	bool spawned;
	bool registered;
	bool child_reaped;
	bool wait_status_valid;
	bool reap_consistency_error;
	int wait_status;
	void *owner;
	void (*reaped_cb)(void *owner);
};

void midr_trace_exec_init(struct midr_trace_exec *exec);
bool midr_trace_exec_supported(void);

int midr_trace_exec_start(struct midr_trace_exec *exec,
			  const struct prefix *target, void *owner,
			  void (*reaped_cb)(void *owner));
int midr_trace_exec_stream_fd(const struct midr_trace_exec *exec,
			      enum midr_trace_exec_stream stream);
ssize_t midr_trace_exec_read(struct midr_trace_exec *exec,
			     enum midr_trace_exec_stream stream, void *buf,
			     size_t buflen);
void midr_trace_exec_close_stream(struct midr_trace_exec *exec,
				  enum midr_trace_exec_stream stream);
void midr_trace_exec_close_streams(struct midr_trace_exec *exec);

int midr_trace_exec_signal(struct midr_trace_exec *exec, int signo);
enum midr_trace_exec_reap_rc
midr_trace_exec_reap_one(struct midr_trace_exec *exec);

/*
 * FRR signal-table entry point.  It scans only registered MIDR PIDs and
 * invokes reaped callbacks after the full waitpid(WNOHANG) scan.
 */
void midr_trace_exec_sigchld(void);

void midr_trace_exec_reset(struct midr_trace_exec *exec);

#ifdef __cplusplus
}
#endif

#endif /* _FRR_MIDR_TRACE_EXEC_H */
