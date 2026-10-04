#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

// Child processes, started with posix_spawn (fork is unsafe in a
// multithreaded module) and a cleaned environment: Basecamp's AppImage sets
// LD_LIBRARY_PATH for itself, which must not leak into a recorder or into
// llama-server.
namespace proc {

// argv[0] is looked up in PATH unless it contains a '/'. stdout and stderr go
// to logPath (appended), or nowhere when it is empty. extraEnv: "NAME=value".
pid_t spawn(const std::vector<std::string>& argv, const std::string& logPath,
            const std::vector<std::string>& extraEnv, std::string* error);

// Like spawn, but the child gets SIGTERM when the spawning THREAD exits (Linux
// PR_SET_PDEATHSIG), so a server cannot outlive a Basecamp that was killed.
// Call it from a thread that lives as long as the child should. Uses fork:
// the child only makes async-signal-safe calls before exec.
pid_t spawnTied(const std::vector<std::string>& argv, const std::string& logPath,
                const std::vector<std::string>& extraEnv, std::string* error);

bool alive(pid_t pid);

// Sends `sig`, waits up to waitMs for the exit, then SIGKILLs.
void stop(pid_t pid, int sig, int waitMs);

// Runs to completion (or SIGKILL after timeoutMs). Returns the exit code, or
// -1 when it could not run or was killed.
int run(const std::vector<std::string>& argv, const std::string& logPath, int timeoutMs, std::string* error);

}  // namespace proc
