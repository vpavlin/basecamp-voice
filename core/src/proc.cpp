#include "proc.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/syscall.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <thread>

extern char** environ;

namespace proc {
namespace {

bool dropped(const char* entry) {
    for (const char* name : {"LD_LIBRARY_PATH=", "LD_PRELOAD=", "QT_PLUGIN_PATH=", "QML_IMPORT_PATH=", "QML2_IMPORT_PATH="})
        if (std::strncmp(entry, name, std::strlen(name)) == 0) return true;
    return false;
}

}  // namespace

pid_t spawn(const std::vector<std::string>& argv, const std::string& logPath,
            const std::vector<std::string>& extraEnv, std::string* error) {
    if (argv.empty()) { *error = "nothing to run"; return -1; }
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e)
        if (!dropped(*e)) env.emplace_back(*e);
    for (const auto& x : extraEnv) env.push_back(x);

    std::vector<char*> av, ev;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    for (const auto& e : env) ev.push_back(const_cast<char*>(e.c_str()));
    ev.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    const char* out = logPath.empty() ? "/dev/null" : logPath.c_str();
    posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Its own process group, so a Ctrl-C meant for Basecamp does not reach it.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid = -1;
    const int rc = argv[0].find('/') == std::string::npos
        ? posix_spawnp(&pid, av[0], &fa, &attr, av.data(), ev.data())
        : posix_spawn(&pid, av[0], &fa, &attr, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        *error = argv[0] + ": " + std::strerror(rc);
        return -1;
    }
    return pid;
}

pid_t spawnTied(const std::vector<std::string>& argv, const std::string& logPath,
                const std::vector<std::string>& extraEnv, std::string* error) {
#if !defined(__linux__)
    return spawn(argv, logPath, extraEnv, error);
#else
    if (argv.empty() || argv[0].find('/') == std::string::npos) { *error = "spawnTied needs an absolute program path"; return -1; }
    // Everything the child needs is prepared before fork.
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e)
        if (!dropped(*e)) env.emplace_back(*e);
    for (const auto& x : extraEnv) env.push_back(x);
    std::vector<char*> av, ev;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    for (const auto& e : env) ev.push_back(const_cast<char*>(e.c_str()));
    ev.push_back(nullptr);
    const char* out = logPath.empty() ? "/dev/null" : logPath.c_str();
    const pid_t parent = ::getpid();

    const pid_t pid = ::fork();
    if (pid < 0) { *error = std::string("fork: ") + std::strerror(errno); return -1; }
    if (pid == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) ::_exit(127);   // the parent already went away
        ::setpgid(0, 0);
#ifdef SYS_close_range
        // Not Basecamp's sockets and files: close all but stdin/out/err
        // (they are reopened below). Ignored on kernels without close_range.
        ::syscall(SYS_close_range, 3u, ~0u, 0u);
#endif
        const int in = ::open("/dev/null", O_RDONLY);
        const int fd = ::open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (in >= 0) ::dup2(in, 0);
        if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); }
        ::execve(av[0], av.data(), ev.data());
        ::_exit(127);
    }
    return pid;
#endif
}

bool alive(pid_t pid) {
    if (pid <= 0) return false;
    // WNOWAIT: look without reaping, so the pid stays ours until stop() or
    // run() collects it and can never be reused under us.
    siginfo_t info{};
    info.si_pid = 0;
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0)
        return info.si_pid == 0;
    return errno == ECHILD ? ::kill(pid, 0) == 0 : true;
}

void stop(pid_t pid, int sig, int waitMs) {
    if (pid <= 0) return;
    ::kill(pid, sig);
    for (int waited = 0; waited < waitMs; waited += 50) {
        const pid_t r = ::waitpid(pid, nullptr, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

int run(const std::vector<std::string>& argv, const std::string& logPath, int timeoutMs, std::string* error) {
    const pid_t pid = spawn(argv, logPath, {}, error);
    if (pid < 0) return -1;
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    *error = argv[0] + " did not finish in time";
    return -1;
}

}  // namespace proc
