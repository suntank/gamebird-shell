#ifndef GAMEBIRD_GBSPLASH_SPLASH_H
#define GAMEBIRD_GBSPLASH_SPLASH_H

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#define GBSPLASH_WIDTH 240U
#define GBSPLASH_HEIGHT 240U
#define GBSPLASH_IMAGE_BYTES (GBSPLASH_WIDTH * GBSPLASH_HEIGHT * 2U)

/* The asset is always little-endian RGB565, with rows ordered top to bottom. */
int gbsplash_load_image(const char *path, uint8_t *image, size_t image_size,
                       char *error, size_t error_size);

/* Validate before mapping any framebuffer memory. No device is modified here. */
int gbsplash_validate_framebuffer(const struct fb_fix_screeninfo *fixed,
                                const struct fb_var_screeninfo *variable,
                                char *error, size_t error_size);

/*
 * Render into native-endian packed truecolor framebuffer memory. Only visible
 * pixels are touched, including black letterboxing when the target is not
 * square. Stride padding and pixels outside the visible viewport are preserved.
 * All validation completes before the first write.
 */
int gbsplash_render(const uint8_t *image, size_t image_size,
                   uint8_t *memory, size_t memory_size,
                   const struct fb_fix_screeninfo *fixed,
                   const struct fb_var_screeninfo *variable,
                   char *error, size_t error_size);

/* Match a complete component in a resolved sysfs device path, not a prefix. */
int gbsplash_is_spi_panel_path(const char *resolved_device_path);

#endif
