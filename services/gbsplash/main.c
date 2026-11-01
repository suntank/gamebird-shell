#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "splash.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_IMAGE "/usr/share/gamebird/boot/splash.rgb565"
#define DEFAULT_TIMEOUT_MS 1500U
#define MAX_TIMEOUT_MS 10000U
#define POLL_INTERVAL_MS 20U

static volatile sig_atomic_t stop_signal = 0;

static void request_stop(int signal_number) {
  stop_signal = signal_number;
}

static int cancelled(char *error, size_t size) {
  if (stop_signal == 0) return 0;
  snprintf(error, size, "interrupted by signal %d", (int)stop_signal);
  return 1;
}

struct framebuffer {
  int fd;
  char path[PATH_MAX];
  struct fb_fix_screeninfo fixed;
  struct fb_var_screeninfo variable;
};

struct console_state {
  int fd;
  int old_mode;
  int changed;
};

static void usage(FILE *output) {
  fprintf(output,
          "Usage: gbsplash [--image PATH] [--timeout-ms N] [--fbdev PATH]\n"
          "       gbsplash --check-image PATH\n"
          "\n"
          "Draw one 240x240 little-endian RGB565 image and leave it displayed.\n"
          "Default image: " DEFAULT_IMAGE "\n"
          "Default device: detected 240x240 SPI0.0 framebuffer.\n"
          "--timeout-ms bounds device discovery (default 1500, maximum 10000).\n"
          "An explicit --fbdev permits aspect-fit rendering on other framebuffers.\n");
}

static int monotonic_ms(uint64_t *milliseconds) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  *milliseconds = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
  return 0;
}

/* Return 1 for a usable device, 0 for a device not yet present, -1 on error. */
static int open_framebuffer(const char *path, int automatic,
                            struct framebuffer *fb, char *error, size_t size) {
  struct stat info;
  int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENODEV || errno == ENXIO || errno == EAGAIN) {
      return 0;
    }
    snprintf(error, size, "cannot open framebuffer %s: %s", path, strerror(errno));
    return -1;
  }
  if (fstat(fd, &info) != 0 || !S_ISCHR(info.st_mode)) {
    snprintf(error, size, "%s is not a framebuffer character device", path);
    close(fd);
    return -1;
  }
  if (automatic) {
    char device_link[PATH_MAX], resolved[PATH_MAX];
    snprintf(device_link, sizeof(device_link), "/sys/dev/char/%u:%u/device",
             (unsigned int)major(info.st_rdev), (unsigned int)minor(info.st_rdev));
    /* Verify the opened node too, so a stale /dev/fbN link cannot select HDMI. */
    if (realpath(device_link, resolved) == NULL ||
        !gbsplash_is_spi_panel_path(resolved)) {
      snprintf(error, size, "%s does not resolve to the SPI0.0 panel", path);
      close(fd);
      return -1;
    }
  }
  memset(&fb->fixed, 0, sizeof(fb->fixed));
  memset(&fb->variable, 0, sizeof(fb->variable));
  if (ioctl(fd, FBIOGET_FSCREENINFO, &fb->fixed) != 0 ||
      ioctl(fd, FBIOGET_VSCREENINFO, &fb->variable) != 0) {
    int saved_errno = errno;
    close(fd);
    if (saved_errno == ENODEV || saved_errno == ENXIO || saved_errno == EAGAIN) {
      return 0;
    }
    snprintf(error, size, "cannot query framebuffer %s: %s", path,
             strerror(saved_errno));
    return -1;
  }
  if (automatic && (fb->variable.xres != GBSPLASH_WIDTH ||
                    fb->variable.yres != GBSPLASH_HEIGHT)) {
    snprintf(error, size, "SPI framebuffer %s is %ux%u; expected 240x240", path,
             fb->variable.xres, fb->variable.yres);
    close(fd);
    return -1;
  }
  if (gbsplash_validate_framebuffer(&fb->fixed, &fb->variable, error, size) != 0) {
    close(fd);
    return -1;
  }
  if (strlen(path) >= sizeof(fb->path)) {
    snprintf(error, size, "framebuffer path is too long");
    close(fd);
    return -1;
  }
  strcpy(fb->path, path);
  fb->fd = fd;
  return 1;
}

static int discover_framebuffer(struct framebuffer *fb, char *error, size_t size) {
  glob_t matches;
  size_t i;
  int result = 0;
  int glob_result;

  memset(&matches, 0, sizeof(matches));
  glob_result = glob("/sys/class/graphics/fb[0-9]*", GLOB_NOSORT, NULL, &matches);
  if (glob_result == GLOB_NOMATCH) {
    globfree(&matches);
    return 0;
  }
  if (glob_result != 0) {
    snprintf(error, size, "cannot enumerate framebuffer devices");
    globfree(&matches);
    return -1;
  }
  for (i = 0; i < matches.gl_pathc; ++i) {
    char device_link[PATH_MAX], resolved[PATH_MAX], device[PATH_MAX];
    const char *name = strrchr(matches.gl_pathv[i], '/');
    const char *digit;
    int attempt;
    if (name == NULL) continue;
    ++name;
    for (digit = name + 2; *digit >= '0' && *digit <= '9'; ++digit) {}
    if (digit == name + 2 || *digit != '\0') continue;
    if (snprintf(device_link, sizeof(device_link), "%s/device", matches.gl_pathv[i])
        >= (int)sizeof(device_link)) continue;
    if (realpath(device_link, resolved) == NULL ||
        !gbsplash_is_spi_panel_path(resolved)) continue;
    if (snprintf(device, sizeof(device), "/dev/%s", name) >= (int)sizeof(device)) {
      continue;
    }
    attempt = open_framebuffer(device, 1, fb, error, size);
    if (attempt == 1) {
      globfree(&matches);
      return 1;
    }
    if (attempt < 0) result = -1;
  }
  globfree(&matches);
  return result;
}

static struct console_state guard_console(void) {
  /* tty0 names the active virtual console; tty1 covers normal appliance boot. */
  const char *paths[] = {"/dev/tty0", "/dev/tty1"};
  struct console_state state = {.fd = -1, .old_mode = KD_TEXT, .changed = 0};
  size_t i;
  for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    int fd = open(paths[i], O_RDWR | O_CLOEXEC | O_NOCTTY);
    int previous;
    if (fd < 0) continue;
    if (ioctl(fd, KDGETMODE, &previous) == 0 &&
        (previous == KD_GRAPHICS || ioctl(fd, KDSETMODE, KD_GRAPHICS) == 0)) {
      state.fd = fd;
      state.old_mode = previous;
      state.changed = previous != KD_GRAPHICS;
      return state;
    }
    close(fd);
  }
  /* Some initramfs configurations lack a VT; drawing can still be useful. */
  fprintf(stderr, "gbsplash: virtual-console guard unavailable\n");
  return state;
}

static void release_console(struct console_state *state, int success) {
  if (state->fd < 0) return;
  if (!success && state->changed) {
    if (ioctl(state->fd, KDSETMODE, state->old_mode) != 0) {
      fprintf(stderr, "gbsplash: cannot restore virtual console: %s\n", strerror(errno));
    }
  }
  /* Successful exit deliberately leaves KD_GRAPHICS for the shell to inherit. */
  close(state->fd);
  state->fd = -1;
}

/*
 * fb_deferred_io_fsync flushes the work scheduled by mmap writes. Drivers which
 * do not implement fsync get an ordinary framebuffer write, whose fb_write path
 * reports damage directly. Never replace that with a guessed sleep duration.
 */
static int flush_framebuffer(const struct framebuffer *fb, const uint8_t *memory,
                             char *error, size_t size) {
  int result;
  if (cancelled(error, size)) return -1;
  do { result = fsync(fb->fd); }
  while (result != 0 && errno == EINTR && stop_signal == 0);
  if (cancelled(error, size)) return -1;
  if (result == 0) return 0;
  if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) {
    snprintf(error, size, "framebuffer flush failed: %s", strerror(errno));
    return -1;
  }
  {
    const size_t bytes = fb->variable.bits_per_pixel / 8U;
    const size_t offset = (size_t)fb->variable.yoffset * fb->fixed.line_length +
                           (size_t)fb->variable.xoffset * bytes;
    const size_t length = (size_t)(fb->variable.yres - 1U) * fb->fixed.line_length +
                           (size_t)fb->variable.xres * bytes;
    uint8_t *copy = malloc(length);
    size_t written = 0;
    if (copy == NULL) {
      snprintf(error, size, "cannot allocate framebuffer transfer buffer");
      return -1;
    }
    /* Preserve row padding and offscreen pixels that lie inside this span. */
    memcpy(copy, memory + offset, length);
    while (written < length) {
      if (cancelled(error, size)) {
        free(copy);
        return -1;
      }
      ssize_t count = pwrite(fb->fd, copy + written, length - written,
                             (off_t)(offset + written));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        int saved_errno = errno;
        free(copy);
        snprintf(error, size, "framebuffer transfer failed: %s",
                 count == 0 ? "short write" : strerror(saved_errno));
        return -1;
      }
      written += (size_t)count;
    }
    free(copy);
  }
  if (cancelled(error, size)) return -1;
  return 0;
}

int main(int argc, char **argv) {
  const char *image_path = DEFAULT_IMAGE, *device_path = NULL, *check_path = NULL;
  uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
  uint8_t *image = NULL, *memory = MAP_FAILED;
  struct framebuffer fb = {.fd = -1};
  struct console_state console = {.fd = -1, .old_mode = KD_TEXT, .changed = 0};
  char error[512] = "";
  int i, exit_code = 1, have_render_option = 0;
  uint64_t deadline;
  struct sigaction stop_action;

  memset(&stop_action, 0, sizeof(stop_action));
  stop_action.sa_handler = request_stop;
  sigemptyset(&stop_action.sa_mask);
  /* No SA_RESTART: an interruptible device operation must reach cleanup. */
  if (sigaction(SIGTERM, &stop_action, NULL) != 0 ||
      sigaction(SIGINT, &stop_action, NULL) != 0) {
    fprintf(stderr, "gbsplash: cannot install stop handler: %s\n", strerror(errno));
    return 1;
  }

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (i + 1 >= argc) {
      usage(stderr);
      return 2;
    }
    if (strcmp(argv[i], "--check-image") == 0) {
      if (check_path != NULL) { usage(stderr); return 2; }
      check_path = argv[++i];
    } else if (strcmp(argv[i], "--image") == 0) {
      image_path = argv[++i];
      have_render_option = 1;
    } else if (strcmp(argv[i], "--fbdev") == 0) {
      device_path = argv[++i];
      have_render_option = 1;
    } else if (strcmp(argv[i], "--timeout-ms") == 0) {
      char *end;
      unsigned long value;
      const char *argument = argv[++i];
      errno = 0;
      value = strtoul(argument, &end, 10);
      if (argument[0] < '0' || argument[0] > '9' || *end != '\0' ||
          errno != 0 || value > MAX_TIMEOUT_MS) {
        fprintf(stderr, "gbsplash: --timeout-ms must be between 0 and 10000\n");
        return 2;
      }
      timeout_ms = (uint32_t)value;
      have_render_option = 1;
    } else {
      usage(stderr);
      return 2;
    }
  }
  if (check_path != NULL && have_render_option) {
    fprintf(stderr, "gbsplash: --check-image cannot be combined with rendering options\n");
    return 2;
  }
  image = malloc(GBSPLASH_IMAGE_BYTES);
  if (image == NULL) {
    fprintf(stderr, "gbsplash: cannot allocate image buffer\n");
    return 1;
  }
  if (gbsplash_load_image(check_path != NULL ? check_path : image_path,
                          image, GBSPLASH_IMAGE_BYTES, error, sizeof(error)) != 0) {
    goto done;
  }
  if (check_path != NULL) {
    printf("gbsplash: image OK (240x240 RGB565 LE, %u bytes)\n", GBSPLASH_IMAGE_BYTES);
    exit_code = 0;
    goto done;
  }

  if (monotonic_ms(&deadline) != 0) {
    snprintf(error, sizeof(error), "cannot read monotonic clock: %s", strerror(errno));
    goto done;
  }
  deadline += timeout_ms;
  for (;;) {
    uint64_t now;
    if (cancelled(error, sizeof(error))) goto done;
    int found = device_path != NULL
        ? open_framebuffer(device_path, 0, &fb, error, sizeof(error))
        : discover_framebuffer(&fb, error, sizeof(error));
    if (found > 0) break;
    if (found < 0) goto done;
    if (monotonic_ms(&now) != 0) {
      snprintf(error, sizeof(error), "cannot read monotonic clock: %s", strerror(errno));
      goto done;
    }
    if (now >= deadline) {
      snprintf(error, sizeof(error), "no %sframebuffer within %u ms",
               device_path != NULL ? "requested " : "240x240 SPI0.0 ", timeout_ms);
      exit_code = 3;
      goto done;
    }
    {
      uint64_t remaining_ms = deadline - now;
      struct timespec pause = {.tv_sec = 0,
          .tv_nsec = (long)((remaining_ms < POLL_INTERVAL_MS ? remaining_ms :
                             POLL_INTERVAL_MS) * 1000000U)};
      /* An interrupted wait retries discovery and checks its deadline again. */
      nanosleep(&pause, NULL);
    }
  }

  memory = mmap(NULL, fb.fixed.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
  if (memory == MAP_FAILED) {
    snprintf(error, sizeof(error), "cannot map framebuffer: %s", strerror(errno));
    goto done;
  }
  if (cancelled(error, sizeof(error))) goto done;
  console = guard_console();
  if (gbsplash_render(image, GBSPLASH_IMAGE_BYTES, memory, fb.fixed.smem_len,
                       &fb.fixed, &fb.variable, error, sizeof(error)) != 0 ||
      flush_framebuffer(&fb, memory, error, sizeof(error)) != 0) {
    goto done;
  }
  {
    uint64_t written_ms;
    if (monotonic_ms(&written_ms) == 0) {
      fprintf(stderr, "gbsplash: frame written t=%.3fs fb=%s size=%ux%u bpp=%u\n",
              (double)written_ms / 1000.0, fb.path, fb.variable.xres,
              fb.variable.yres, fb.variable.bits_per_pixel);
    } else {
      fprintf(stderr, "gbsplash: frame written fb=%s (timing unavailable)\n", fb.path);
    }
  }
  exit_code = 0;

done:
  if (cancelled(error, sizeof(error))) exit_code = 128 + (int)stop_signal;
  if (exit_code != 0 && error[0] != '\0') fprintf(stderr, "gbsplash: %s\n", error);
  release_console(&console, exit_code == 0);
  if (memory != MAP_FAILED) munmap(memory, fb.fixed.smem_len);
  if (fb.fd >= 0) close(fb.fd);
  free(image);
  return exit_code;
}
