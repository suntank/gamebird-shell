#include "platform/proc.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gb::platform {

ProcessResult RunProcessBlocking(const std::vector<std::string>& argv) {
  ProcessResult result;

  if (argv.empty()) {
    result.error = "empty argv";
    return result;
  }

  std::vector<char*> child_argv;
  child_argv.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    child_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  child_argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    result.error = std::string("fork failed: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    execvp(child_argv[0], child_argv.data());
    _exit(127);
  }

  result.launched = true;

  int status = 0;
  while (true) {
    const pid_t wait_rc = waitpid(pid, &status, 0);
    if (wait_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = std::string("waitpid failed: ") + std::strerror(errno);
      return result;
    }
    break;
  }

  if (WIFEXITED(status)) {
    result.exited_normally = true;
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signaled = true;
    result.signal = WTERMSIG(status);
    result.exit_code = 128 + result.signal;
  }

  return result;
}

ProcessCaptureResult RunProcessCapture(const std::vector<std::string>& argv) {
  ProcessCaptureResult capture;

  if (argv.empty()) {
    capture.process.error = "empty argv";
    return capture;
  }

  int pipes[2] = {-1, -1};
  if (pipe2(pipes, O_CLOEXEC) < 0) {
    capture.process.error = std::string("pipe2 failed: ") + std::strerror(errno);
    return capture;
  }

  std::vector<char*> child_argv;
  child_argv.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    child_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  child_argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    const int e = errno;
    close(pipes[0]);
    close(pipes[1]);
    capture.process.error = std::string("fork failed: ") + std::strerror(e);
    return capture;
  }

  if (pid == 0) {
    close(pipes[0]);
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    execvp(child_argv[0], child_argv.data());
    _exit(127);
  }

  capture.process.launched = true;
  close(pipes[1]);

  char buf[4096];
  while (true) {
    const ssize_t n = read(pipes[0], buf, sizeof(buf));
    if (n > 0) {
      capture.output.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  close(pipes[0]);

  int status = 0;
  while (true) {
    const pid_t wait_rc = waitpid(pid, &status, 0);
    if (wait_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      capture.process.error =
          std::string("waitpid failed: ") + std::strerror(errno);
      return capture;
    }
    break;
  }

  if (WIFEXITED(status)) {
    capture.process.exited_normally = true;
    capture.process.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    capture.process.signaled = true;
    capture.process.signal = WTERMSIG(status);
    capture.process.exit_code = 128 + capture.process.signal;
  }

  return capture;
}

}  // namespace gb::platform
