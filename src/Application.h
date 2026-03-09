#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <atomic>
#include <imgui.h>
#include <memory>
#include <vector>
#if defined(__APPLE__) && defined(__OBJC__)
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#endif

struct GLFWwindow;

class Application {
public:
  Application();
  ~Application();

  bool Initialize();
  void Run();
  void ReloadFonts();
  void ApplyUIScale();
  ImFont *GetCustomFont() const { return custom_font_; }
  ImFont *GetPreviewFont() const { return preview_font_; }
  void SetPreviewFont(const std::string &font_path);

  void AddWindow(std::unique_ptr<Window> window);
  ArchipelagoNetwork &GetNetwork() { return ap_network_; }

private:
  void RenderUI();

  GLFWwindow *window_ = nullptr;

  // Preferences
  ConnectionSettings current_config_;
  ConnectionSettings pending_config_;
  std::atomic<bool> settings_changed_pending_{false};
  std::atomic<bool> fonts_reload_pending_{false};
  std::atomic<bool> is_initialized_{false};
  ImFont *custom_font_ = nullptr;
  ImFont *preview_font_ = nullptr;
  std::string preview_font_path_;
#if defined(__APPLE__) && defined(__OBJC__)
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> commandQueue_ = nil;
  MTLRenderPassDescriptor *renderPassDescriptor_ = nil;
  CAMetalLayer *layer_ = nil;
#elif defined(__APPLE__)
  void *device_ = nullptr;
  void *commandQueue_ = nullptr;
  void *renderPassDescriptor_ = nullptr;
  void *layer_ = nullptr;
#endif

  std::string imgui_ini_path_;
  ArchipelagoNetwork ap_network_;
  std::vector<std::unique_ptr<Window>> windows_;
};
