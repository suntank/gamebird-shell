#define _GNU_SOURCE

#include "splash.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t image[GBSPLASH_IMAGE_BYTES];
static char error[256];

static void source_pixel(unsigned int x, unsigned int y, uint16_t value) {
  size_t position = ((size_t)y * GBSPLASH_WIDTH + x) * 2U;
  image[position] = (uint8_t)value;
  image[position + 1U] = (uint8_t)(value >> 8U);
}

static void describe(struct fb_fix_screeninfo *fixed,
                     struct fb_var_screeninfo *variable,
                     unsigned int width, unsigned int height, unsigned int bpp) {
  memset(fixed, 0, sizeof(*fixed));
  memset(variable, 0, sizeof(*variable));
  fixed->type = FB_TYPE_PACKED_PIXELS;
  fixed->visual = FB_VISUAL_TRUECOLOR;
  fixed->line_length = width * (bpp / 8U);
  fixed->smem_len = fixed->line_length * height;
  variable->xres = variable->xres_virtual = width;
  variable->yres = variable->yres_virtual = height;
  variable->bits_per_pixel = bpp;
  if (bpp == 16) {
    variable->red = (struct fb_bitfield){.offset = 11, .length = 5};
    variable->green = (struct fb_bitfield){.offset = 5, .length = 6};
    variable->blue = (struct fb_bitfield){.offset = 0, .length = 5};
  } else {
    variable->red = (struct fb_bitfield){.offset = 16, .length = 8};
    variable->green = (struct fb_bitfield){.offset = 8, .length = 8};
    variable->blue = (struct fb_bitfield){.offset = 0, .length = 8};
  }
}

static uint16_t word16(const uint8_t *data) {
  uint16_t value;
  memcpy(&value, data, sizeof(value));
  return value;
}

static uint32_t word32(const uint8_t *data) {
  uint32_t value;
  memcpy(&value, data, sizeof(value));
  return value;
}

static void conversion_and_orientation(void) {
  struct fb_fix_screeninfo fixed;
  struct fb_var_screeninfo variable;
  uint8_t *frame = malloc(GBSPLASH_WIDTH * GBSPLASH_HEIGHT * 4U);
  assert(frame != NULL);
  memset(image, 0, sizeof(image));
  source_pixel(0, 0, 0xf800);
  source_pixel(1, 0, 0x07e0);
  source_pixel(2, 0, 0x001f);
  source_pixel(3, 0, 0xffff);
  source_pixel(239, 239, 0x8410);
  describe(&fixed, &variable, 240, 240, 16);
  assert(gbsplash_render(image, sizeof(image), frame, fixed.smem_len,
                         &fixed, &variable, error, sizeof(error)) == 0);
  assert(word16(frame) == 0xf800);
  assert(word16(frame + 2) == 0x07e0);
  assert(word16(frame + 4) == 0x001f);
  assert(word16(frame + 6) == 0xffff);
  assert(word16(frame + GBSPLASH_IMAGE_BYTES - 2U) == 0x8410);

  describe(&fixed, &variable, 240, 240, 32);
  variable.transp = (struct fb_bitfield){.offset = 24, .length = 8};
  assert(gbsplash_render(image, sizeof(image), frame, fixed.smem_len,
                         &fixed, &variable, error, sizeof(error)) == 0);
  assert(word32(frame) == 0xffff0000U);
  assert(word32(frame + 4) == 0xff00ff00U);
  assert(word32(frame + 8) == 0xff0000ffU);
  assert(word32(frame + 12) == 0xffffffffU);
  assert(word32(frame + 16) == 0xff000000U);

  variable.red.offset = 0;
  variable.blue.offset = 16;
  variable.transp.length = 0;
  assert(gbsplash_render(image, sizeof(image), frame, fixed.smem_len,
                         &fixed, &variable, error, sizeof(error)) == 0);
  assert(word32(frame) == 0x000000ffU);
  assert(word32(frame + 8) == 0x00ff0000U);

  describe(&fixed, &variable, 240, 240, 16);
  variable.red.offset = 10;
  variable.green.length = 5;
  variable.transp = (struct fb_bitfield){.offset = 15, .length = 1};
  assert(gbsplash_render(image, sizeof(image), frame, fixed.smem_len,
                         &fixed, &variable, error, sizeof(error)) == 0);
  assert(word16(frame) == 0xfc00U);
  assert(word16(frame + 2) == 0x83e0U);
  assert(word16(frame + 4) == 0x801fU);
  free(frame);
}

static void viewport_padding_and_scaling(void) {
  struct fb_fix_screeninfo fixed;
  struct fb_var_screeninfo variable;
  uint8_t memory[64], expected[64];
  const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
  memset(image, 0, sizeof(image));
  source_pixel(0, 0, colors[0]);
  source_pixel(120, 0, colors[1]);
  source_pixel(0, 120, colors[2]);
  source_pixel(120, 120, colors[3]);
  describe(&fixed, &variable, 2, 2, 16);
  variable.xres_virtual = variable.yres_virtual = 4;
  variable.xoffset = variable.yoffset = 1;
  fixed.line_length = 12;
  fixed.smem_len = 48;
  memset(memory, 0xa5, sizeof(memory));
  memcpy(expected, memory, sizeof(memory));
  memcpy(expected + 14, &colors[0], 2);
  memcpy(expected + 16, &colors[1], 2);
  memcpy(expected + 26, &colors[2], 2);
  memcpy(expected + 28, &colors[3], 2);
  assert(gbsplash_render(image, sizeof(image), memory, sizeof(memory),
                         &fixed, &variable, error, sizeof(error)) == 0);
  assert(memcmp(memory, expected, sizeof(memory)) == 0);

  describe(&fixed, &variable, 4, 2, 16);
  memset(memory, 0xa5, sizeof(memory));
  assert(gbsplash_render(image, sizeof(image), memory, sizeof(memory),
                         &fixed, &variable, error, sizeof(error)) == 0);
  assert(word16(memory) == 0 && word16(memory + 6) == 0);
  assert(word16(memory + 2) == colors[0]);
  assert(word16(memory + 4) == colors[1]);
  assert(word16(memory + 10) == colors[2]);
  assert(word16(memory + 12) == colors[3]);
  assert(memory[16] == 0xa5);
}

static void rejected(const struct fb_fix_screeninfo *fixed,
                     const struct fb_var_screeninfo *variable) {
  uint8_t memory[16], expected[16];
  memset(memory, 0xa5, sizeof(memory));
  memcpy(expected, memory, sizeof(memory));
  assert(gbsplash_render(image, sizeof(image), memory, sizeof(memory),
                         fixed, variable, error, sizeof(error)) != 0);
  assert(memcmp(memory, expected, sizeof(memory)) == 0);
}

static void invalid_layouts(void) {
  struct fb_fix_screeninfo fixed, f;
  struct fb_var_screeninfo variable, v;
  describe(&fixed, &variable, 2, 2, 16);
  assert(gbsplash_validate_framebuffer(&fixed, &variable, error, sizeof(error)) == 0);
  f = fixed; f.type = FB_TYPE_PLANES; rejected(&f, &variable);
  f = fixed; f.visual = FB_VISUAL_DIRECTCOLOR; rejected(&f, &variable);
  f = fixed; f.line_length = 0; rejected(&f, &variable);
  f = fixed; f.line_length = 3; rejected(&f, &variable);
  f = fixed; f.smem_len = 7; rejected(&f, &variable);
  f = fixed; f.smem_len = UINT32_MAX; rejected(&f, &variable);
  v = variable; v.bits_per_pixel = 24; rejected(&fixed, &v);
  v = variable; v.red.offset = 5; rejected(&fixed, &v);
  v = variable; v.red.msb_right = 1; rejected(&fixed, &v);
  v = variable; v.red.length = 0; rejected(&fixed, &v);
  v = variable; v.red.length = 32; rejected(&fixed, &v);
  v = variable; v.red.offset = UINT32_MAX; rejected(&fixed, &v);
  v = variable; v.transp = v.blue; rejected(&fixed, &v);
  v = variable; v.grayscale = 1; rejected(&fixed, &v);
  v = variable; v.nonstd = 1; rejected(&fixed, &v);
  v = variable; v.xres = 0; rejected(&fixed, &v);
  v = variable; v.yres = UINT32_MAX; rejected(&fixed, &v);
  v = variable; v.xoffset = 1; rejected(&fixed, &v);
  v = variable; v.yoffset = UINT32_MAX; rejected(&fixed, &v);
  v = variable; v.xres_virtual = UINT32_MAX; rejected(&fixed, &v);
  v = variable; v.vmode = FB_VMODE_YWRAP; rejected(&fixed, &v);
  {
    uint8_t memory[8];
    memset(memory, 0xa5, sizeof(memory));
    assert(gbsplash_render(image, sizeof(image), memory, 7,
                           &fixed, &variable, error, sizeof(error)) != 0);
    assert(gbsplash_render(image, sizeof(image) - 1U, memory, sizeof(memory),
                           &fixed, &variable, error, sizeof(error)) != 0);
    assert(memory[0] == 0xa5 && memory[7] == 0xa5);
  }
}

static void image_file_validation(void) {
  char name[] = "/tmp/gamebird-splash-test-XXXXXX";
  int fd = mkstemp(name);
  uint8_t *loaded = malloc(sizeof(image));
  assert(fd >= 0 && loaded != NULL);
  assert(write(fd, image, sizeof(image)) == (ssize_t)sizeof(image));
  assert(gbsplash_load_image(name, loaded, sizeof(image), error, sizeof(error)) == 0);
  assert(memcmp(loaded, image, sizeof(image)) == 0);
  assert(ftruncate(fd, sizeof(image) - 1U) == 0);
  assert(gbsplash_load_image(name, loaded, sizeof(image), error, sizeof(error)) != 0);
  assert(ftruncate(fd, sizeof(image) + 1U) == 0);
  assert(gbsplash_load_image(name, loaded, sizeof(image), error, sizeof(error)) != 0);
  assert(gbsplash_load_image("/dev/zero", loaded, sizeof(image), error, sizeof(error)) != 0);
  close(fd);
  unlink(name);
  free(loaded);
}

int main(void) {
  conversion_and_orientation();
  viewport_padding_and_scaling();
  invalid_layouts();
  image_file_validation();
  assert(gbsplash_is_spi_panel_path("/sys/devices/platform/spi0/spi0.0/drm/card1"));
  assert(gbsplash_is_spi_panel_path("/sys/devices/spi0.0"));
  assert(!gbsplash_is_spi_panel_path("/sys/devices/spi0.01/drm/card1"));
  assert(!gbsplash_is_spi_panel_path("/sys/devices/other-spi0.0/drm/card1"));
  assert(!gbsplash_is_spi_panel_path("/sys/devices/spi0.0-other/drm/card1"));
  assert(!gbsplash_is_spi_panel_path("/sys/devices/platform/hdmi/drm/card0"));
  puts("splash renderer tests passed");
  return 0;
}
