#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "EmbeddedWebServer.h"
#include "Window.h"
#include <atomic>
#include <imgui.h>
#include <memory>
#include <vector>
#if defined(__APPLE__) && defined(__OBJC__)
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#elif defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

struct GLFWwindow;

class Application {
public:
  Application();
  ~Application();

  bool InitializeNetwork();
  bool InitializeUI();
  void CleanupUI();
  void Run();
  void ReloadFonts();
  void ParseArguments(int argc, char **argv);

  bool UserRequestedExit() const { return user_requested_exit_ || should_exit_; }
  bool WasDisconnected() const { return is_disconnected_; }
  void ResetDisconnected() { is_disconnected_ = false; }

  static void SignalHandler(int signum);

  ImFont *GetUIFont() const { return ui_font_; }
  ImFont *GetContentFont() const { return content_font_; }
  ImFont *GetPreviewFont() const { return preview_font_; }
  ImFont *GetPreviewFallbackFont() const { return preview_fallback_font_; }
  void SetPreviewFont(const std::string &font_path);
  void SetPreviewFallbackFont(const std::string &font_path);

  void AddWindow(std::unique_ptr<Window> window);
  ArchipelagoNetwork &GetNetwork() { return ap_network_; }

private:
  void RenderUI(std::tm *current_tm);

  GLFWwindow *window_ = nullptr;

  // Preferences
  ConnectionSettings current_config_;
  ConnectionSettings pending_config_;
  std::atomic<bool> settings_changed_pending_{false};
  std::atomic<bool> fonts_reload_pending_{false};
  std::atomic<bool> is_initialized_{false};
  bool is_first_launch_ = false;
  ImFont *ui_font_ = nullptr;
  ImFont *content_font_ = nullptr;
  ImFont *preview_font_ = nullptr;
  ImFont *preview_fallback_font_ = nullptr;
  std::string preview_font_path_;
  std::string preview_fallback_font_path_;
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
#elif defined(_WIN32)
  ID3D11Device *pd3dDevice_ = nullptr;
  ID3D11DeviceContext *pd3dDeviceContext_ = nullptr;
  IDXGISwapChain *pSwapChain_ = nullptr;
  ID3D11RenderTargetView *mainRenderTargetView_ = nullptr;
  bool CreateDeviceD3D(HWND hWnd);
  void CleanupDeviceD3D();
  void CreateRenderTarget();
  void CleanupRenderTarget();
#endif

  static void glfw_error_callback(int error, const char *description);

  std::string glsl_version_;
  std::string imgui_ini_path_;
  ArchipelagoNetwork ap_network_;
  std::unique_ptr<EmbeddedWebServer> web_server_;
  std::vector<std::unique_ptr<Window>> windows_;
  bool show_about_ = false;
  bool show_exit_confirmation_ = false;
  int frames_to_render_ = 0;
  bool first_render_ = true;

  bool user_requested_exit_ = false;
  bool is_disconnected_ = false;
  bool debug_mode_ = false;
  static std::atomic<bool> should_exit_;
};
