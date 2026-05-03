#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "EmbeddedWebServer.h"
#include "LogicManager.h"
#include "ProfileLock.h"
#include "Window.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <memory>
#include <mutex>
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

  // Take ownership of the acquired profile lock. Optional; if not set, the
  // Application doesn't release a lock at shutdown. main() should always set
  // it after acquiring.
  void SetProfileLock(std::unique_ptr<ProfileLock> lock) {
    profile_lock_ = std::move(lock);
  }

  bool InitializeNetwork();

  // Two-phase UI lifecycle so the pre-init profile picker can render on the
  // platform-native backend (Metal/DX11/OpenGL) without duplicating the
  // per-platform setup. Backend init is idempotent across recovery loop
  // iterations; AppState (windows_, web server) is per-iteration.
  bool InitializeBackend();
  void ShutdownBackend();
  bool InitializeUI();   // assumes backend is up; sets up app state
  void CleanupUI();      // tears down app state; backend stays

  void Run();

  // Pre-init modal: renders a profile-conflict picker using the established
  // backend. Returns the user's decision; does not modify Application state.
  // Caller is responsible for acting on the result (acquire/force-acquire/
  // re-exec).
  struct PickerResult {
    enum class Action { TakeOver, SwitchTo, Quit };
    Action action = Action::Quit;
    std::string profile_name; // populated for SwitchTo
  };
  PickerResult ShowProfilePicker(const std::string &locked_profile,
                                 long long owner_pid, bool is_stale);

  // Save all in-memory state to disk without exiting. Used by SwitchProfile.
  void SaveAllState();

  // Everything Application::~Application() does *except* GLFW teardown:
  // save state, stop the web server, stop the network thread (so AP
  // server sees a clean disconnect), join pack-loader threads, etc. Used
  // before ReExec, where the process is replaced and no destructor runs.
  void PrepareForExit();

  // Schedule a switch to another profile: saves state, releases the current
  // profile lock, and re-execs this binary with --profile=<new>. Does not
  // return on success.
  void SwitchProfile(const std::string &new_profile_name);
  void ReloadFonts();
  void ParseArguments(int argc, char **argv);

  bool UserRequestedExit() const { return user_requested_exit_ || should_exit_; }

  static void SignalHandler(int signum);

  ImFont *GetUIFont() const { return ui_font_; }
  ImFont *GetContentFont() const { return content_font_; }
  ImFont *GetPreviewFont() const { return preview_font_; }
  ImFont *GetPreviewFallbackFont() const { return preview_fallback_font_; }
  void SetPreviewFont(const std::string &font_path);
  void SetPreviewFallbackFont(const std::string &font_path);
  void RemovePopTrackerPack(const std::string &game);

  void AddWindow(std::unique_ptr<Window> window);
  ArchipelagoNetwork &GetNetwork() { return ap_network_; }

  struct ChecksSnapshot {
    double timestamp;
    int checked_locations;
    int total_locations;
  };

  // Run callback under the mutex protecting checks_history_. Callers must not
  // store the reference past the callback's return, and must not block on
  // anything that could re-enter Application.
  void WithChecksHistory(
      const std::function<void(const std::vector<ChecksSnapshot> &)> &cb) const;
  void SaveChecksHistory();
  void LoadChecksHistory();
  void ClearChecksHistory();

  LogicManager *GetOrCreateLogicForSession(
      const std::string &name, const std::string &game,
      const nlohmann::json &slotData = nlohmann::json{});
  void DestroyLogicForSession(const std::string &name);

private:
  void RenderUI(std::tm *current_tm);
  std::string BuildGraphHistoryJson() const;
  // Push a "[System]"-prefixed message into the chat history. Used for
  // events that aren't tied to any AP session (e.g. web server bind).
  void PostSystemChatMessage(const std::string &text, uint32_t color);
  // Convert a StartResult into an appropriate chat message (or skip if the
  // server wasn't even attempted).
  void ReportWebServerStart(const EmbeddedWebServer::StartResult &result,
                            bool streamer_mode);

  GLFWwindow *window_ = nullptr;

  // Preferences
  ConnectionSettings current_config_;
  ConnectionSettings pending_config_;
  std::string live_server_url_;
  std::vector<SlotSettings> live_slots_;
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
  std::map<std::string, std::unique_ptr<LogicManager>> logic_managers_;
  std::unique_ptr<EmbeddedWebServer> web_server_;
  std::vector<std::unique_ptr<Window>> windows_;
  bool show_about_ = false;
  bool show_exit_confirmation_ = false;
  int frames_to_render_ = 0;
  bool first_render_ = true;

  bool user_requested_exit_ = false;
  bool debug_mode_ = false;
  bool in_picker_modal_ = false;
  // Set by SwitchProfile() to defer the actual teardown+exec to the end
  // of the current Run() frame. Switching mid-render would destroy the
  // ProfilesWindow whose Render() is on the call stack.
  std::string pending_switch_profile_;

  // Picker-only cache: ListProfiles() result refreshed at ~1Hz so the modal
  // doesn't stat the profiles dir 60+ times per second.
  std::vector<ProfileInfo> picker_profiles_cache_;
  std::chrono::steady_clock::time_point picker_last_refresh_{};
  static std::atomic<bool> should_exit_;
  mutable std::mutex checks_history_mutex_;
  std::vector<ChecksSnapshot> checks_history_;
  std::string checks_history_server_url_;
  std::unique_ptr<ProfileLock> profile_lock_;
};
