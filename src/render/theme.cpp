#include "render/theme.h"

#include "render/surface_240.h"
#include "render/assets.h"
#include "render/image_cache.h"

namespace gb::render {

Theme DefaultTheme() {
  return Theme{
      .bg = Rgb565(16, 67, 108),
      .panel = Rgb565(28, 91, 145),
      .panel_border = Rgb565(114, 180, 224),
      .accent = Rgb565(37, 115, 177),
      .text = Rgb565(255, 255, 255),
      .text_dim = Rgb565(255, 255, 255),
      .success = Rgb565(142, 244, 184),
  };
}

void DrawThemeBackground(Surface240& surface, const Theme& theme) {
  static ImageCache images(1);
  std::string error;
  if (const auto* background = images.LoadPng(BundledAsset("ui/systemback.png").string(), error)) {
    surface.BlitScaled(background->pixels.data(), background->width, background->height,
                       0, 0, surface.Width(), surface.Height());
  } else {
    surface.Clear(theme.bg);
  }
}

}  // namespace gb::render
