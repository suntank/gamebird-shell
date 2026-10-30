#pragma once

#include <memory>
#include <string>

#include "nest/service.h"
#include "platform/platform.h"
#include "render/surface_240.h"
#include "render/theme.h"

namespace gb::ui::screens {

// Native, controller-first Nest. All network and package work runs off the UI
// thread. The shell keeps this controller alive while it is hidden.
class NestController {
 public:
  NestController();
  explicit NestController(const nest::Config& config);
  ~NestController();
  NestController(const NestController&) = delete;
  NestController& operator=(const NestController&) = delete;

  void Configure(const nest::Config& config);
  void Open();
  // True means return to the shell. B from Nest's home relocks the PIN.
  bool HandleButton(platform::Button button);
  // Poll completed work without blocking; true requests a repaint.
  bool Tick();
  void Draw(render::Surface240& surface, const render::Theme& theme) const;
  bool TakeLibraryChanged();
  std::string TakeLaunchPath();
  bool Busy() const;
  const std::map<std::string, nest::InstalledGame>& Installed() const;
  const std::vector<nest::Entry>& Catalog() const;
  // Shell launch/scan failures remain visible until B dismisses them.
  void ReportStatus(const std::string& message, bool failed);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend struct NestControllerTestAccess;
};

}  // namespace gb::ui::screens
