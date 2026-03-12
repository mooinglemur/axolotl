#include "Application.h"
#include "ChatWindow.h"
#include "HintWindow.h"
#include "ItemFeedWindow.h"
#include "ReceivedItemsWindow.h"
#include "SettingsWindow.h"
#include <iostream>

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#if defined(__APPLE__) && defined(__OBJC__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <backends/imgui_impl_metal.h>
#else
#include <backends/imgui_impl_opengl3.h>
#endif
#include <imgui.h>

Application::Application()
    : current_config_(Config::Load()), pending_config_(current_config_) {
  ap_network_.on_history_updated = [this]() {};
}

Application::~Application() {
  for (const auto &window : windows_) {
    current_config_.show_windows[window->GetName()] = window->GetOpen();
  }
  Config::Save(current_config_);

  ap_network_.Disconnect();
  windows_.clear();

#ifdef __APPLE__
  ImGui_ImplMetal_Shutdown();
#else
  ImGui_ImplOpenGL3_Shutdown();
#endif
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  if (window_) {
    glfwDestroyWindow(window_);
  }
  glfwTerminate();
}

bool Application::Initialize() {
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW." << std::endl;
    return false;
  }

#ifdef __APPLE__
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

  window_ =
      glfwCreateWindow(1280, 720, "Axolotl - Archipelago Client", NULL, NULL);
  if (!window_) {
    const char *description;
    int code = glfwGetError(&description);
    std::cerr << "Failed to create GLFW window. Error " << code << ": "
              << description << std::endl;
    return false;
  }
  glsl_version_ = "#version 150";
#else
  struct GLVersion {
    int major;
    int minor;
    int profile;
    const char *glsl;
  };
  std::vector<GLVersion> versions = {
      {3, 3, GLFW_OPENGL_CORE_PROFILE, "#version 130"},
      {3, 0, 0, "#version 130"},
      {2, 1, 0, "#version 120"}};

  for (const auto &v : versions) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, v.major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, v.minor);
    if (v.major > 3 || (v.major == 3 && v.minor >= 2)) {
      glfwWindowHint(GLFW_OPENGL_PROFILE, v.profile);
    }

    window_ =
        glfwCreateWindow(1280, 720, "Axolotl - Archipelago Client", NULL, NULL);
    if (window_) {
      glsl_version_ = v.glsl;
      break;
    }
  }

  if (!window_) {
    const char *description;
    int code = glfwGetError(&description);
    std::cerr << "Failed to create OpenGL context (Tried 3.3, 3.0, 2.1). Last "
                 "Error "
              << code << ": " << description << std::endl;
    return false;
  }
#endif

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();

  // Relocate imgui.ini
  imgui_ini_path_ = Config::GetImguiIniPath().string();
  io.IniFilename = imgui_ini_path_.c_str();

  ap_network_.SetMaxHistory(current_config_.max_history_size);

  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window_, true);
#ifdef __APPLE__
  device_ = MTLCreateSystemDefaultDevice();
  commandQueue_ = [device_ newCommandQueue];
  ImGui_ImplMetal_Init(device_);
#else
  ImGui_ImplOpenGL3_Init(glsl_version_.c_str());
#endif

  ReloadFonts();

  // Initial windows
  auto chat = std::make_unique<ChatWindow>(
      ap_network_.GetChatHistory(),
      [this](const std::string &msg) { ap_network_.SendChat(msg); },
      [this]() { return ap_network_.GetState(); },
      [this](const std::string &u, const std::string &s, const std::string &p) {
        ap_network_.Connect(u, s, p);
      },
      [this]() { ap_network_.Disconnect(); },
      [this]() -> const std::map<int, std::string> & {
        return ap_network_.GetPlayerNames();
      });
  if (current_config_.show_windows.count(chat->GetName())) {
    chat->SetOpen(current_config_.show_windows[chat->GetName()]);
  }
  AddWindow(std::move(chat));

  if (current_config_.show_windows["Settings"]) {
    AddWindow(std::make_unique<SettingsWindow>(
        current_config_,
        [this](const ConnectionSettings &s) {
          pending_config_ = s;
          Config::Save(s);
          settings_changed_pending_ = true;
        },
        [this](const std::string &p) { SetPreviewFont(p); }));
  }
  if (current_config_.show_windows["Received Items"]) {
    AddWindow(std::make_unique<ReceivedItemsWindow>(
        ap_network_.GetReceivedItemsHistory()));
  }
  if (current_config_.show_windows["Full Feed"]) {
    AddWindow(std::make_unique<ItemFeedWindow>(
        ap_network_.GetItemHistory(),
        [this]() { return ap_network_.GetGlobalSlot(); }, false, "Full Feed"));
  }
  if (current_config_.show_windows["My Feed"]) {
    AddWindow(std::make_unique<ItemFeedWindow>(
        ap_network_.GetItemHistory(),
        [this]() { return ap_network_.GetGlobalSlot(); }, true, "My Feed"));
  }
  if (current_config_.show_windows["Hints"]) {
    AddWindow(std::make_unique<HintWindow>(
        ap_network_.GetHints(), ap_network_.GetPlayerNames(),
        ap_network_.GetItemNames(), ap_network_.GetLocationNames(),
        [this]() { return ap_network_.GetGlobalSlot(); }));
  }

  return true;
}

void Application::ReloadFonts() {
  ImGuiIO &io = ImGui::GetIO();
  io.Fonts->Clear();

  // Load default font
  io.Fonts->AddFontDefault();

  // Load custom UI font if set
  if (!current_config_.font_path.empty()) {
    float size =
        16.0f *
        (current_config_.ui_scale > 0 ? current_config_.ui_scale : 1.0f);
    custom_font_ =
        io.Fonts->AddFontFromFileTTF(current_config_.font_path.c_str(), size);
  } else {
    custom_font_ = nullptr;
  }

  // Load preview font if set
  if (!preview_font_path_.empty()) {
    float size =
        16.0f *
        (current_config_.ui_scale > 0 ? current_config_.ui_scale : 1.0f);
    preview_font_ =
        io.Fonts->AddFontFromFileTTF(preview_font_path_.c_str(), size);
  } else {
    preview_font_ = nullptr;
  }

  io.Fonts->Build();
#ifdef __APPLE__
  // Metal font atlas update logic would go here if needed per frame
#else
  // Texture update is handled by modern backends
#endif
}

void Application::SetPreviewFont(const std::string &path) {
  preview_font_path_ = path;
  fonts_reload_pending_ = true;
}

void Application::Run() {
  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();

    if (fonts_reload_pending_) {
      ReloadFonts();
      fonts_reload_pending_ = false;
    }

    if (settings_changed_pending_) {
      current_config_ = pending_config_;
      ap_network_.SetMaxHistory(current_config_.max_history_size);
      fonts_reload_pending_ = true;
      settings_changed_pending_ = false;
    }

    ap_network_.Update();

    // Start ImGui frame
#ifdef __APPLE__
    id<MTLCommandBuffer> commandBuffer = [commandQueue_ commandBuffer];
    MTLRenderPassDescriptor *renderPassDescriptor =
        [view currentRenderPassDescriptor];
    id<MTLRenderCommandEncoder> renderEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
#else
    ImGui_ImplOpenGL3_NewFrame();
#endif
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport();

    // Main Menu
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Settings")) {
          AddWindow(std::make_unique<SettingsWindow>(
              current_config_,
              [this](const ConnectionSettings &s) {
                pending_config_ = s;
                Config::Save(s);
                settings_changed_pending_ = true;
              },
              [this](const std::string &p) { SetPreviewFont(p); }));
        }
        if (ImGui::MenuItem("Exit")) {
          glfwSetWindowShouldClose(window_, 1);
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Windows")) {
        if (ImGui::MenuItem("Chat")) {
          bool found = false;
          for (auto &window : windows_) {
            if (dynamic_cast<ChatWindow *>(window.get())) {
              window->SetOpen(true);
              found = true;
              break;
            }
          }
          if (!found) {
            AddWindow(std::make_unique<ChatWindow>(
                ap_network_.GetChatHistory(),
                [this](const std::string &msg) { ap_network_.SendChat(msg); },
                [this]() { return ap_network_.GetState(); },
                [this](const std::string &u, const std::string &s,
                       const std::string &p) { ap_network_.Connect(u, s, p); },
                [this]() { ap_network_.Disconnect(); },
                [this]() -> const std::map<int, std::string> & {
                  return ap_network_.GetPlayerNames();
                }));
          }
        }
        if (ImGui::MenuItem("Received Items")) {
          bool found = false;
          for (auto &window : windows_) {
            if (dynamic_cast<ReceivedItemsWindow *>(window.get())) {
              window->SetOpen(true);
              found = true;
              break;
            }
          }
          if (!found) {
            AddWindow(std::make_unique<ReceivedItemsWindow>(
                ap_network_.GetReceivedItemsHistory()));
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Full Feed")) {
          bool found = false;
          for (auto &window : windows_) {
            auto feed = dynamic_cast<ItemFeedWindow *>(window.get());
            if (feed && feed->GetName() == "Full Feed") {
              window->SetOpen(true);
              found = true;
              break;
            }
          }
          if (!found) {
            AddWindow(std::make_unique<ItemFeedWindow>(
                ap_network_.GetItemHistory(),
                [this]() { return ap_network_.GetGlobalSlot(); }, false,
                "Full Feed"));
          }
        }
        if (ImGui::MenuItem("My Feed")) {
          bool found = false;
          for (auto &window : windows_) {
            auto feed = dynamic_cast<ItemFeedWindow *>(window.get());
            if (feed && feed->GetName() == "My Feed") {
              window->SetOpen(true);
              found = true;
              break;
            }
          }
          if (!found) {
            AddWindow(std::make_unique<ItemFeedWindow>(
                ap_network_.GetItemHistory(),
                [this]() { return ap_network_.GetGlobalSlot(); }, true,
                "My Feed"));
          }
        }
        if (ImGui::MenuItem("Hints")) {
          bool found = false;
          for (auto &window : windows_) {
            if (dynamic_cast<HintWindow *>(window.get())) {
              window->SetOpen(true);
              found = true;
              break;
            }
          }
          if (!found) {
            AddWindow(std::make_unique<HintWindow>(
                ap_network_.GetHints(), ap_network_.GetPlayerNames(),
                ap_network_.GetItemNames(), ap_network_.GetLocationNames(),
                [this]() { return ap_network_.GetGlobalSlot(); }));
          }
        }
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    RenderUI();

    ImGui::Render();
#ifdef __APPLE__
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer,
                                   renderEncoder);
    [renderEncoder endEncoding];
    [commandBuffer presentDrawable:view.currentDrawable];
    [commandBuffer commit];
#else
    int display_w, display_h;
    glfwGetFramebufferSize(window_, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif

    ImGuiIO &io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      GLFWwindow *backup_current_context = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup_current_context);
    }

    glfwSwapBuffers(window_);
  }

  // Workaround for Wayland segmentation fault on exit:
  // After the last glfwSwapBuffers, we need a final glfwPollEvents before
  // destroying the window or terminating GLFW to clear out Wayland queues.
  glfwPollEvents();
}

void Application::AddWindow(std::unique_ptr<Window> window) {
  windows_.push_back(std::move(window));
}

void Application::RenderUI() {
  for (auto &window : windows_) {
    window->Render(custom_font_, preview_font_);
  }
}
