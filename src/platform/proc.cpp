#include "platform/proc.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gb::platform {

ProcessResult RunProcessBlocking(const std::vector<std::string>& argv,
                                 const ProcessOptions& options) {
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

  int errors[2];
  if (pipe2(errors, O_CLOEXEC) < 0) {
    result.error = std::string("launch pipe failed: ") + std::strerror(errno);
    return result;
  }
  int log = -1;
  if (!options.log_path.empty()) {
    log = open(options.log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log < 0) {
      result.error = std::string("cannot open game log: ") + std::strerror(errno);
      close(errors[0]); close(errors[1]);
      return result;
    }
  }
  const pid_t pid = fork();
  if (pid < 0) {
    result.error = std::string("fork failed: ") + std::strerror(errno);
    close(errors[0]); close(errors[1]);
    if (log >= 0) close(log);
    return result;
  }
  if (pid == 0) {
    close(errors[0]);
    const auto fail = [&](const int stage) {
      const int failure[] = {stage, errno};
      const auto ignored = write(errors[1], failure, sizeof(failure));
      (void)ignored;
      _exit(127);
    };
    if (!options.working_directory.empty() && chdir(options.working_directory.c_str()) < 0) fail(1);
    if (log >= 0) {
      if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) fail(2);
      close(log);
    }
    for (const auto& [key, value] : options.environment)
      if (setenv(key.c_str(), value.c_str(), 1) < 0) fail(3);
    execvp(child_argv[0], child_argv.data());
    fail(4);
  }
  close(errors[1]);
  if (log >= 0) close(log);
  int failure[2] = {};
  ssize_t count;
  do { count = read(errors[0], failure, sizeof(failure)); } while (count < 0 && errno == EINTR);
  close(errors[0]);
  if (count > 0) {
    const char* stage = failure[0] == 1 ? "cannot enter game directory" :
                        failure[0] == 2 ? "cannot redirect game log" :
                        failure[0] == 3 ? "cannot set game environment" : "cannot start executable";
    result.error = std::string(stage) + ": " + std::strerror(failure[1]);
  } else if (count < 0) {
    result.error = "cannot read launch result";
  } else {
    result.launched = true;
  }

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
