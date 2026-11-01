#define _GNU_SOURCE

#include "splash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bound memory use even if a driver reports corrupt or unsupported geometry. */
#define GBSPLASH_MAX_DIMENSION 8192U
#define GBSPLASH_MAX_MEMORY (256U * 1024U * 1024U)

static int fail(char *error, size_t size, const char *format, ...) {
  if (error != NULL && size != 0) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
  }
  return -1;
}

int gbsplash_load_image(const char *path, uint8_t *image, size_t image_size,
                       char *error, size_t error_size) {
  struct stat info;
  size_t offset = 0;
  int fd;

  if (path == NULL || image == NULL || image_size != GBSPLASH_IMAGE_BYTES) {
    return fail(error, error_size, "invalid image buffer");
  }
  /* O_NONBLOCK plus the regular-file check avoids hanging on a FIFO/device. */
  fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (fd < 0) {
    return fail(error, error_size, "cannot open image %s: %s", path,
                strerror(errno));
  }
  if (fstat(fd, &info) != 0) {
    int saved_errno = errno;
    close(fd);
    return fail(error, error_size, "cannot stat image: %s",
                strerror(saved_errno));
  }
  if (!S_ISREG(info.st_mode) || info.st_size != (off_t)GBSPLASH_IMAGE_BYTES) {
    close(fd);
    return fail(error, error_size,
                "image must be a regular %u-byte RGB565 file",
                GBSPLASH_IMAGE_BYTES);
  }
  while (offset < image_size) {
    ssize_t count = read(fd, image + offset, image_size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      int saved_errno = errno;
      close(fd);
      return fail(error, error_size, "image read failed: %s",
                  count == 0 ? "unexpected end of file" : strerror(saved_errno));
    }
    offset += (size_t)count;
  }
  for (;;) {
    uint8_t extra;
    ssize_t count = read(fd, &extra, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count != 0) {
      int saved_errno = errno;
      close(fd);
      return fail(error, error_size, "image changed while reading: %s",
                  count > 0 ? "trailing bytes" : strerror(saved_errno));
    }
    break;
  }
  close(fd);
  return 0;
}

static int channel_mask(const struct fb_bitfield *channel, uint32_t bpp,
                        int optional, uint32_t *mask) {
  if (optional && channel->length == 0) {
    *mask = 0;
    return 0;
  }
  /* 1..16-bit channels cover RGB565, RGB555, RGB888 and RGB101010. */
  if (channel->length == 0 || channel->length > 16 || channel->msb_right != 0 ||
      channel->offset >= bpp || channel->length > bpp - channel->offset) {
    return -1;
  }
  *mask = ((UINT32_C(1) << channel->length) - 1U) << channel->offset;
  return 0;
}

int gbsplash_validate_framebuffer(const struct fb_fix_screeninfo *fixed,
                                const struct fb_var_screeninfo *variable,
                                char *error, size_t error_size) {
  uint32_t masks[4];
  uint64_t row_end, last_byte;
  uint32_t bytes;
  unsigned int i, j;

  if (fixed == NULL || variable == NULL) {
    return fail(error, error_size, "missing framebuffer description");
  }
  if (fixed->type != FB_TYPE_PACKED_PIXELS ||
      fixed->visual != FB_VISUAL_TRUECOLOR || variable->grayscale != 0 ||
      variable->nonstd != 0 ||
      (variable->bits_per_pixel != 16 && variable->bits_per_pixel != 32)) {
    return fail(error, error_size,
                "unsupported framebuffer format (requires packed 16/32-bit truecolor)");
  }
  if ((variable->vmode & FB_VMODE_YWRAP) != 0 ||
      variable->xres == 0 || variable->yres == 0 ||
      variable->xres > GBSPLASH_MAX_DIMENSION ||
      variable->yres > GBSPLASH_MAX_DIMENSION ||
      variable->xres > variable->xres_virtual ||
      variable->yres > variable->yres_virtual ||
      variable->xoffset > variable->xres_virtual - variable->xres ||
      variable->yoffset > variable->yres_virtual - variable->yres) {
    return fail(error, error_size, "invalid framebuffer viewport");
  }
  bytes = variable->bits_per_pixel / 8U;
  row_end = ((uint64_t)variable->xoffset + variable->xres) * bytes;
  /* The validated viewport sums fit in uint32_t; products fit in uint64_t. */
  last_byte = ((uint64_t)variable->yoffset + variable->yres - 1U) *
              fixed->line_length + row_end;
  if (fixed->line_length == 0 || row_end > fixed->line_length ||
      (uint64_t)variable->xres_virtual * bytes > fixed->line_length ||
      fixed->smem_len == 0 || fixed->smem_len > GBSPLASH_MAX_MEMORY ||
      last_byte > fixed->smem_len || last_byte > SIZE_MAX) {
    return fail(error, error_size, "framebuffer stride or memory bounds are invalid");
  }
  if (channel_mask(&variable->red, variable->bits_per_pixel, 0, &masks[0]) ||
      channel_mask(&variable->green, variable->bits_per_pixel, 0, &masks[1]) ||
      channel_mask(&variable->blue, variable->bits_per_pixel, 0, &masks[2]) ||
      channel_mask(&variable->transp, variable->bits_per_pixel, 1, &masks[3])) {
    return fail(error, error_size, "unsupported framebuffer channel bitfields");
  }
  for (i = 0; i < 4; ++i) {
    for (j = i + 1; j < 4; ++j) {
      if ((masks[i] & masks[j]) != 0) {
        return fail(error, error_size, "overlapping framebuffer channel bitfields");
      }
    }
  }
  return 0;
}

static uint32_t component(uint32_t value, uint32_t source_max,
                          const struct fb_bitfield *target) {
  if (target->length == 0) return 0;
  const uint32_t target_max = (UINT32_C(1) << target->length) - 1U;
  return ((value * target_max + source_max / 2U) / source_max) << target->offset;
}

static uint32_t pixel_value(uint16_t rgb565,
                            const struct fb_var_screeninfo *variable) {
  return component((rgb565 >> 11U) & 31U, 31U, &variable->red) |
         component((rgb565 >> 5U) & 63U, 63U, &variable->green) |
         component(rgb565 & 31U, 31U, &variable->blue) |
         component(1U, 1U, &variable->transp);
}

int gbsplash_render(const uint8_t *image, size_t image_size,
                   uint8_t *memory, size_t memory_size,
                   const struct fb_fix_screeninfo *fixed,
                   const struct fb_var_screeninfo *variable,
                   char *error, size_t error_size) {
  uint32_t side, origin_x, origin_y, bytes, y, x;

  if (image == NULL || image_size != GBSPLASH_IMAGE_BYTES || memory == NULL) {
    return fail(error, error_size, "invalid image or framebuffer buffer");
  }
  if (gbsplash_validate_framebuffer(fixed, variable, error, error_size) != 0) {
    return -1;
  }
  if (memory_size < fixed->smem_len) {
    return fail(error, error_size, "framebuffer mapping is smaller than advertised memory");
  }

  side = variable->xres < variable->yres ? variable->xres : variable->yres;
  origin_x = (variable->xres - side) / 2U;
  origin_y = (variable->yres - side) / 2U;
  bytes = variable->bits_per_pixel / 8U;
  for (y = 0; y < variable->yres; ++y) {
    uint8_t *row = memory + ((size_t)y + variable->yoffset) * fixed->line_length +
                   (size_t)variable->xoffset * bytes;
    for (x = 0; x < variable->xres; ++x) {
      uint16_t source = 0;
      uint32_t value;
      if (x >= origin_x && x < origin_x + side &&
          y >= origin_y && y < origin_y + side) {
        const uint32_t sx = (x - origin_x) * GBSPLASH_WIDTH / side;
        const uint32_t sy = (y - origin_y) * GBSPLASH_HEIGHT / side;
        const size_t index = ((size_t)sy * GBSPLASH_WIDTH + sx) * 2U;
        source = (uint16_t)(image[index] | ((uint16_t)image[index + 1U] << 8U));
      }
      value = pixel_value(source, variable);
      if (bytes == 2) {
        const uint16_t packed = (uint16_t)value;
        memcpy(row + (size_t)x * bytes, &packed, sizeof(packed));
      } else {
        memcpy(row + (size_t)x * bytes, &value, sizeof(value));
      }
    }
  }
  return 0;
}

int gbsplash_is_spi_panel_path(const char *path) {
  const char *match;
  if (path == NULL) return 0;
  match = path;
  while ((match = strstr(match, "spi0.0")) != NULL) {
    if ((match == path || match[-1] == '/') &&
        (match[6] == '/' || match[6] == '\0')) {
      return 1;
    }
    ++match;
  }
  return 0;
}
