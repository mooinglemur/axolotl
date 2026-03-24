#include "Application.h"
#include "ChatWindow.h"
#include "FontScanner.h"
#include "HintWindow.h"
#include "ItemFeedWindow.h"
#include "Platform.h"
#include "ReceivedItemsWindow.h"
#include "SettingsWindow.h"
#include "version.h"
#include <chrono>
#include <iostream>
#include <thread>

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#if defined(__APPLE__) && defined(__OBJC__)
#define GLFW_EXPOSE_NATIVE_COCOA
#import <AppKit/AppKit.h>
#include <GLFW/glfw3native.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <backends/imgui_impl_metal.h>
#elif defined(_WIN32)
#include <backends/imgui_impl_dx11.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#else
#include <backends/imgui_impl_opengl3.h>
#endif
#ifdef _WIN32
#include <ixwebsocket/IXNetSystem.h>
#endif
#include <imgui.h>
#include <imgui_internal.h>

Application::Application()
    : current_config_(Config::Load()), pending_config_(current_config_) {
  is_first_launch_ = !std::filesystem::exists(Config::GetConfigPath());
  ap_network_.SetSettings(&current_config_);
  ap_network_.on_history_updated = [this]() {};
  for (const auto &slot : current_config_.slots) {
    ap_network_.AddSession(slot.name);
  }
}

Application::~Application() {
  for (const auto &window : windows_) {
    current_config_.show_windows[window->GetName()] = window->GetOpen();
    window->SaveState(current_config_);
  }

  if (window_) {
    glfwGetWindowSize(window_, &current_config_.window_width,
                      &current_config_.window_height);
    glfwGetWindowPos(window_, &current_config_.window_x,
                     &current_config_.window_y);
  }

  Config::Save(current_config_);

#ifdef _WIN32
  ix::uninitNetSystem();
#endif
  // Clear all callbacks BEFORE disconnecting to avoid background thread
  // wake-ups
  ap_network_.on_history_updated = nullptr;
  ap_network_.SetWakeUpCallback(nullptr);

  // Explicitly disconnect all sessions
  ap_network_.DisconnectAll();

  // Clear windows (destroying UI objects)
  windows_.clear();

#ifdef __APPLE__
  ImGui_ImplMetal_Shutdown();
#elif defined(_WIN32)
  ImGui_ImplDX11_Shutdown();
#else
  ImGui_ImplOpenGL3_Shutdown();
#endif
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

#ifdef _WIN32
  CleanupDeviceD3D();
#endif

  if (window_) {
    glfwDestroyWindow(window_);
  }
  // Stop logging GLFW errors right before termination
  glfwSetErrorCallback(nullptr);
  glfwTerminate();
}

static void glfw_error_callback(int error, const char *description) {
  // Silently handle Wayland window position error
  if (error == 65548 && description &&
      std::string(description).find("Wayland") != std::string::npos) {
    return;
  }
  std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

static void RenderLink(const char *label, const char *url) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
  ImGui::Text("%s", label);
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) {
      Platform::OpenURL(url);
    }
    ImGui::SetTooltip("Open %s in your browser", url);
  }
}

bool Application::Initialize() {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW." << std::endl;
    return false;
  }

#ifdef _WIN32
  ix::initNetSystem();
#endif

#ifdef __APPLE__
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#elif defined(_WIN32)
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

  std::string window_title =
      "Axolotl - Archipelago Text Client (" AXOLOTL_VERSION_STRING;
#ifdef GIT_HASH
  window_title += "-" + std::string(GIT_HASH);
#endif
  window_title += ")";

  window_ = glfwCreateWindow(current_config_.window_width,
                             current_config_.window_height,
                             window_title.c_str(), NULL, NULL);
  if (!window_) {
    const char *description;
    int code = glfwGetError(&description);
    std::cerr << "Failed to create GLFW window. Error " << code << ": "
              << description << std::endl;
    return false;
  }

  if (current_config_.window_x != -1 && current_config_.window_y != -1) {
    glfwSetWindowPos(window_, current_config_.window_x,
                     current_config_.window_y);
  }
  glfwShowWindow(window_);

#ifdef __APPLE__
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);
  glsl_version_ = "#version 150";
#elif defined(_WIN32)
  // No GL context for Windows
#else
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);
  glsl_version_ = "#version 130";
#endif

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();

  // Relocate imgui.ini
  imgui_ini_path_ = Config::GetImguiIniPath().string();
  io.IniFilename = imgui_ini_path_.c_str();

  ap_network_.SetMaxHistory(current_config_.max_history_size);
  ap_network_.SetWakeUpCallback([this]() {
    frames_to_render_ = 3;
    glfwPostEmptyEvent();
  });

  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  ImGui::StyleColorsDark();

#ifdef __APPLE__
  device_ = MTLCreateSystemDefaultDevice();
  commandQueue_ = [device_ newCommandQueue];

  NSWindow *nswin = glfwGetCocoaWindow(window_);
  layer_ = [CAMetalLayer layer];
  layer_.device = device_;
  layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
  nswin.contentView.layer = layer_;
  nswin.contentView.wantsLayer = YES;

  ImGui_ImplGlfw_InitForOther(window_, true);
  ImGui_ImplMetal_Init(device_);
#elif defined(_WIN32)
  ImGui_ImplGlfw_InitForOther(window_, true);
  if (!CreateDeviceD3D(glfwGetWin32Window(window_))) {
    std::cerr << "Failed to initialize DirectX 11 (Hardware and WARP failed)."
              << std::endl;
    return false;
  }
  ImGui_ImplDX11_Init(pd3dDevice_, pd3dDeviceContext_);

  glfwSetWindowSizeCallback(
      window_, [](GLFWwindow *window, int width, int height) {
        auto app = static_cast<Application *>(glfwGetWindowUserPointer(window));
        if (app->pSwapChain_) {
          app->CleanupRenderTarget();
          app->pSwapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
          app->CreateRenderTarget();
        }
      });
#else
  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init(glsl_version_.c_str());
#endif

  glfwSetWindowUserPointer(window_, this);
  glfwSetWindowCloseCallback(window_, [](GLFWwindow *w) {
    auto app = static_cast<Application *>(glfwGetWindowUserPointer(w));
    if (app->current_config_.confirm_exit) {
      glfwSetWindowShouldClose(w, GLFW_FALSE);
      app->show_exit_confirmation_ = true;
    }
  });

  ReloadFonts();

  // Create all windows at startup
  AddWindow(std::make_unique<ChatWindow>(ap_network_, current_config_));

  AddWindow(std::make_unique<SettingsWindow>(
      current_config_,
      [this](const ConnectionSettings &s) {
        pending_config_ = s;
        Config::Save(s);
        settings_changed_pending_ = true;
      },
      [this](const std::string &p) { SetPreviewFont(p); },
      [this](const std::string &p) { SetPreviewFallbackFont(p); }));

  AddWindow(
      std::make_unique<ReceivedItemsWindow>(ap_network_, current_config_));

  AddWindow(std::make_unique<ItemFeedWindow>(ap_network_, current_config_,
                                             false, "Full Feed"));

  AddWindow(std::make_unique<ItemFeedWindow>(ap_network_, current_config_, true,
                                             "Personal Feed"));

  AddWindow(std::make_unique<HintWindow>(ap_network_, current_config_));

  if (is_first_launch_) {
    current_config_.show_windows["Chat"] = true;
    current_config_.show_windows["Hints"] = true;
    current_config_.show_windows["Full Feed"] = true;
    current_config_.show_windows["Personal Feed"] = false;
    current_config_.show_windows["Received Items"] = false;
    current_config_.show_windows["Settings"] = false;
  }

  // Load visibility from config
  for (auto &window : windows_) {
    if (current_config_.show_windows.count(window->GetName())) {
      window->SetOpen(current_config_.show_windows[window->GetName()]);
    }
  }

  return true;
}

void Application::ReloadFonts() {
  ImGuiIO &io = ImGui::GetIO();
  io.Fonts->Clear();

  // Set global UI scale
  io.FontGlobalScale =
      current_config_.ui_scale > 0 ? current_config_.ui_scale : 1.0f;

  // The content font atlas size should be absolute, so we divide by GlobalScale
  // to prevent double-scaling
  float content_base_size =
      16.0f *
      (current_config_.content_scale > 0 ? current_config_.content_scale
                                         : 1.0f) /
      io.FontGlobalScale;

  // 0. Load System UI Font (Always first, becomes default)
  // Note: AddFontDefault doesn't scale well via cfg, so we rely on
  // FontGlobalScale
  ui_font_ = io.Fonts->AddFontDefault();
  io.FontDefault = ui_font_;

  // 1. Load Content Font
  static const ImWchar emoji_ranges[] = {
      0x2000,  0x3300, // General Punctuation to Enclosed CJK Letters and Months
      0x1F000, 0x1F9FF, // Pictographs, Emoticons, symbols etc.
      0,
  };

  const ImWchar *content_font_ranges = io.Fonts->GetGlyphRangesCyrillic();
  bool content_font_loaded = false;
  if (!current_config_.font_path.empty() &&
      FontScanner::IsValidFontFile(current_config_.font_path)) {
    content_font_ = io.Fonts->AddFontFromFileTTF(
        current_config_.font_path.c_str(), content_base_size, nullptr,
        content_font_ranges);
    if (content_font_)
      content_font_loaded = true;
  }

  if (!content_font_loaded) {
    // If no custom font, we use default but at content_base_size
    // This allows the content area to scale independently even with default
    // font. We adjust for io.FontGlobalScale to avoid double-scaling.
    ImFontConfig content_cfg;
    content_cfg.SizePixels = content_base_size;
    content_font_ = io.Fonts->AddFontDefault(&content_cfg);
    content_font_loaded = true;
  }

  // 2. Merge Fallback/CJK Font if specified
  if (content_font_loaded && content_font_ != ui_font_ &&
      !current_config_.fallback_font_path.empty() &&
      FontScanner::IsValidFontFile(current_config_.fallback_font_path)) {
    ImFontConfig merge_cfg;
    merge_cfg.MergeMode = true;
    merge_cfg.OversampleH = 3;
    merge_cfg.OversampleV = 1;

    // Use Chinese Full + Japanese + Korean + Thai + etc ranges
    const ImWchar *cjk_ranges = io.Fonts->GetGlyphRangesChineseFull();
    io.Fonts->AddFontFromFileTTF(current_config_.fallback_font_path.c_str(),
                                 content_base_size, &merge_cfg, cjk_ranges);

    const ImWchar *jp_ranges = io.Fonts->GetGlyphRangesJapanese();
    io.Fonts->AddFontFromFileTTF(current_config_.fallback_font_path.c_str(),
                                 content_base_size, &merge_cfg, jp_ranges);

    const ImWchar *kr_ranges = io.Fonts->GetGlyphRangesKorean();
    io.Fonts->AddFontFromFileTTF(current_config_.fallback_font_path.c_str(),
                                 content_base_size, &merge_cfg, kr_ranges);

    io.Fonts->AddFontFromFileTTF(current_config_.fallback_font_path.c_str(),
                                 content_base_size, &merge_cfg, emoji_ranges);
  }

  // 3. Load Preview Font (Separate Atlas Entry)
  if (!preview_font_path_.empty() &&
      FontScanner::IsValidFontFile(preview_font_path_)) {
    preview_font_ = io.Fonts->AddFontFromFileTTF(preview_font_path_.c_str(),
                                                 content_base_size, nullptr,
                                                 content_font_ranges);
  } else {
    preview_font_ = nullptr;
  }

  // 4. Load Preview Fallback Font (Separate Atlas Entry)
  if (!preview_fallback_font_path_.empty() &&
      FontScanner::IsValidFontFile(preview_fallback_font_path_)) {
    preview_fallback_font_ = io.Fonts->AddFontFromFileTTF(
        preview_fallback_font_path_.c_str(), content_base_size, nullptr,
        io.Fonts->GetGlyphRangesChineseFull());

    ImFontConfig merge_cfg;
    merge_cfg.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(preview_fallback_font_path_.c_str(),
                                 content_base_size, &merge_cfg, emoji_ranges);
  } else {
    preview_fallback_font_ = nullptr;
  }

  io.Fonts->Build();

  // Recreate GPU textures for live reload
#if defined(__APPLE__) && defined(__OBJC__)
  ImGui_ImplMetal_DestroyDeviceObjects();
  ImGui_ImplMetal_CreateDeviceObjects(device_);
#elif defined(_WIN32)
  ImGui_ImplDX11_InvalidateDeviceObjects();
  ImGui_ImplDX11_CreateDeviceObjects();
#else
  ImGui_ImplOpenGL3_DestroyDeviceObjects();
  ImGui_ImplOpenGL3_CreateDeviceObjects();
#endif
}

void Application::SetPreviewFont(const std::string &path) {
  preview_font_path_ = path;
  fonts_reload_pending_ = true;
}

void Application::SetPreviewFallbackFont(const std::string &path) {
  preview_fallback_font_path_ = path;
  fonts_reload_pending_ = true;
}

void Application::Run() {
  while (!glfwWindowShouldClose(window_)) {
    double t_start_frame = glfwGetTime();
#ifdef __APPLE__
    id<CAMetalDrawable> drawable = nil;
    id<MTLCommandBuffer> commandBuffer = nil;
    MTLRenderPassDescriptor *renderPassDescriptor = nil;
#endif
    double t_start = glfwGetTime();
    glfwWaitEventsTimeout(0.016); // 60 FPS base
    double t_after_poll = glfwGetTime();

    bool net_changed = ap_network_.Update();
    if (net_changed)
      frames_to_render_ = 3;

    // Render if network changed OR if we received a real window event (not a
    // timeout) OR if we are in a settlement period
    bool should_render =
        (frames_to_render_ > 0) || (t_after_poll - t_start < 0.015);

    if (frames_to_render_ > 0)
      frames_to_render_--;

    if (fonts_reload_pending_) {
      should_render = true;
      ReloadFonts();
      fonts_reload_pending_ = false;
      frames_to_render_ = 3;
    }

    if (settings_changed_pending_) {
      current_config_ = pending_config_;
      ap_network_.SetMaxHistory(current_config_.max_history_size);
      fonts_reload_pending_ = true;
      settings_changed_pending_ = false;
      should_render = true;
      frames_to_render_ = 3;
    }

    if (first_render_) {
      should_render = true;
    }

    if (!should_render)
      continue;

    first_render_ = false;

    // Start ImGui frame
#ifdef __APPLE__
    drawable = [layer_ nextDrawable];
    if (drawable == nil) {
      glfwPollEvents();
      continue;
    }
    commandBuffer = [commandQueue_ commandBuffer];
    renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
    renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPassDescriptor.colorAttachments[0].clearColor =
        MTLClearColorMake(0.1f, 0.1f, 0.12f, 1.0f);
    renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
#elif defined(_WIN32)
    ImGui_ImplDX11_NewFrame();
#else
    ImGui_ImplOpenGL3_NewFrame();
#endif
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();

    if (is_first_launch_) {
      is_first_launch_ = false;

      ImGui::DockBuilderRemoveNode(dockspace_id);
      ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dockspace_id,
                                    ImGui::GetMainViewport()->Size);

      ImGuiID dock_id_top, dock_id_bottom;
      ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.5f, &dock_id_top,
                                  &dock_id_bottom);

      ImGuiID dock_id_top_left, dock_id_top_right;
      ImGui::DockBuilderSplitNode(dock_id_top, ImGuiDir_Left, 0.25f,
                                  &dock_id_top_left, &dock_id_top_right);

      ImGuiID dock_id_bottom_left, dock_id_bottom_right;
      ImGui::DockBuilderSplitNode(dock_id_bottom, ImGuiDir_Left, 0.5f,
                                  &dock_id_bottom_left, &dock_id_bottom_right);

      ImGui::DockBuilderDockWindow("Chat", dock_id_top_right);
      ImGui::DockBuilderDockWindow("Item Feed", dock_id_top_left);
      ImGui::DockBuilderDockWindow("Log", dock_id_bottom_left);
      ImGui::DockBuilderDockWindow("Hints", dock_id_bottom_right);
      ImGui::DockBuilderFinish(dockspace_id);
    }

    // Main Menu
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Settings")) {
          // Open Settings
          for (auto &w : windows_) {
            if (w->GetName() == "Settings")
              w->SetOpen(true);
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
          if (current_config_.confirm_exit) {
            show_exit_confirmation_ = true;
          } else {
            glfwSetWindowShouldClose(window_, true);
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Windows")) {
        for (auto &window : windows_) {
          bool open = window->GetOpen();
          if (ImGui::MenuItem(window->GetName().c_str(), NULL, &open)) {
            window->SetOpen(open);
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) {
          show_about_ = true;
        }
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    if (show_about_) {
      ImGui::OpenPopup("About Axolotl Archipelago Client");
    }

    if (ImGui::BeginPopupModal("About Axolotl Archipelago Client", &show_about_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Axolotl Archipelago Text Client");
      ImGui::Text("Version: %s (%s)", AXOLOTL_VERSION_STRING, GIT_HASH);
      ImGui::Text("(c) 2026 MooingLemur");
      ImGui::Separator();
      ImGui::Text("A modern, multi-slot capable Archipelago text client.");
      ImGui::Text("Built with ImGui and IXWebSocket.");
      ImGui::Spacing();
      ImGui::Text("Credits:");
      ImGui::BulletText("ImGui by ocornut");
      ImGui::BulletText("IXWebSocket by machinezone");
      ImGui::BulletText("Archipelago by the AP Team");

      ImGui::Separator();
      ImGui::Text("Licenses:");

      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("Axolotl (");
      ImGui::SameLine(0, 0);
      RenderLink(
          "MIT",
          "https://github.com/MooingLemur/axolotl/blob/main/LICENSE.txt");
      ImGui::SameLine(0, 0);
      ImGui::Text(")");
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("Dear ImGui (");
      ImGui::SameLine(0, 0);
      RenderLink("MIT",
                 "https://github.com/ocornut/imgui/blob/master/LICENSE.txt");
      ImGui::SameLine(0, 0);
      ImGui::Text(")");

      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("IXWebSocket (");
      ImGui::SameLine(0, 0);
      RenderLink(
          "BSD 3-Clause",
          "https://github.com/machinezone/IXWebSocket/blob/master/LICENSE.txt");
      ImGui::SameLine(0, 0);
      ImGui::Text(")");

      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("nlohmann/json (");
      ImGui::SameLine(0, 0);
      RenderLink("MIT",
                 "https://github.com/nlohmann/json/blob/develop/LICENSE.MIT");
      ImGui::SameLine(0, 0);
      ImGui::Text(")");

      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("yaml-cpp (");
      ImGui::SameLine(0, 0);
      RenderLink("MIT",
                 "https://github.com/jbeder/yaml-cpp/blob/master/LICENSE");
      ImGui::SameLine(0, 0);
      ImGui::Text(")");

      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("GLFW (");
      ImGui::SameLine(0, 0);
      RenderLink("Zlib", "https://github.com/glfw/glfw/blob/master/LICENSE.md");
      ImGui::SameLine(0, 0);
      ImGui::Text(")");

      ImGui::Separator();
      if (ImGui::Button("Close", ImVec2(120, 0))) {
        show_about_ = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    if (show_exit_confirmation_) {
      ImGui::OpenPopup("Confirm Exit");
    }

    if (ImGui::BeginPopupModal("Confirm Exit", &show_exit_confirmation_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Are you sure you want to exit Axolotl?");
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      if (ImGui::Button("Yes, Exit", ImVec2(120, 0))) {
        glfwSetWindowShouldClose(window_, true);
        show_exit_confirmation_ = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SetItemDefaultFocus();
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        show_exit_confirmation_ = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    std::time_t now = std::time(nullptr);
    std::tm current_tm;
#ifdef _WIN32
    localtime_s(&current_tm, &now);
#else
    localtime_r(&now, &current_tm);
#endif

    RenderUI(&current_tm);

    ImGui::Render();
#ifdef __APPLE__
    if (renderPassDescriptor != nil) {
      id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer
          renderCommandEncoderWithDescriptor:renderPassDescriptor];
      ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer,
                                     renderEncoder);
      [renderEncoder endEncoding];
      [commandBuffer presentDrawable:drawable];
    }
    [commandBuffer commit];
#elif defined(_WIN32)
    const float clear_color_with_alpha[4] = {0.1f, 0.1f, 0.12f, 1.0f};
    pd3dDeviceContext_->OMSetRenderTargets(1, &mainRenderTargetView_, nullptr);
    pd3dDeviceContext_->ClearRenderTargetView(mainRenderTargetView_,
                                              clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    pSwapChain_->Present(1, 0); // Present with vsync
#else
    int display_w, display_h;
    glfwGetFramebufferSize(window_, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
#endif

    // Cap at 60 FPS to be a "polite" desktop app
    double t_cap_end = glfwGetTime();
    double frame_dt = t_cap_end - t_start_frame;
    if (frame_dt < 0.0166) {
      std::this_thread::sleep_for(
          std::chrono::duration<double>(0.0166 - frame_dt));
    }
  }

  // Workaround for Wayland segmentation fault on exit:
  // After the last glfwSwapBuffers, we need a final glfwPollEvents before
  // destroying the window or terminating GLFW to clear out Wayland queues.
  glfwPollEvents();
}

void Application::AddWindow(std::unique_ptr<Window> window) {
  windows_.push_back(std::move(window));
}

void Application::RenderUI(std::tm *current_tm) {
  for (auto &window : windows_) {
    window->Render(current_tm, content_font_, preview_font_,
                   preview_fallback_font_);
  }
}

#ifdef _WIN32
bool Application::CreateDeviceD3D(HWND hWnd) {
  // Setup swap chain
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };

  // Try Hardware and then fallback to WARP (Software)
  D3D_DRIVER_TYPE driverTypes[] = {
      D3D_DRIVER_TYPE_HARDWARE,
      D3D_DRIVER_TYPE_WARP,
  };

  HRESULT res = E_FAIL;
  for (auto driverType : driverTypes) {
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, driverType, nullptr, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &pSwapChain_, &pd3dDevice_, &featureLevel,
        &pd3dDeviceContext_);
    if (SUCCEEDED(res)) {
      if (driverType == D3D_DRIVER_TYPE_WARP) {
        std::cout << "DirectX 11: Using Software WARP Driver." << std::endl;
      }
      break;
    }
  }

  if (res != S_OK)
    return false;

  CreateRenderTarget();
  return true;
}

void Application::CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (pSwapChain_) {
    pSwapChain_->Release();
    pSwapChain_ = nullptr;
  }
  if (pd3dDeviceContext_) {
    pd3dDeviceContext_->Release();
    pd3dDeviceContext_ = nullptr;
  }
  if (pd3dDevice_) {
    pd3dDevice_->Release();
    pd3dDevice_ = nullptr;
  }
}

void Application::CreateRenderTarget() {
  ID3D11Texture2D *pBackBuffer;
  pSwapChain_->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
  pd3dDevice_->CreateRenderTargetView(pBackBuffer, nullptr,
                                      &mainRenderTargetView_);
  pBackBuffer->Release();
}

void Application::CleanupRenderTarget() {
  if (mainRenderTargetView_) {
    mainRenderTargetView_->Release();
    mainRenderTargetView_ = nullptr;
  }
}
#endif
