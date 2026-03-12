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
#elif defined(_WIN32)
#include <backends/imgui_impl_dx11.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
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
#elif defined(_WIN32)
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

  window_ =
      glfwCreateWindow(1280, 720, "Axolotl - Archipelago Client", NULL, NULL);
  if (!window_) {
    const char *description;
    int code = glfwGetError(&description);
    std::cerr << "Failed to create GLFW window. Error " << code << ": "
              << description << std::endl;
    return false;
  }

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

  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

  ImGui::StyleColorsDark();

#ifdef __APPLE__
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

  glfwSetWindowUserPointer(window_, this);
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
#elif defined(_WIN32)
    ImGui_ImplDX11_NewFrame();
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
    id<MTLCommandBuffer> commandBuffer = [commandQueue_ commandBuffer];
    MTLRenderPassDescriptor *renderPassDescriptor =
        [view currentRenderPassDescriptor];
    if (renderPassDescriptor != nil) {
      id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer
          renderCommandEncoderWithDescriptor:renderPassDescriptor];
      ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer,
                                     renderEncoder);
      [renderEncoder endEncoding];
      [commandBuffer presentDrawable:view.currentDrawable];
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

    ImGuiIO &io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
#ifndef _WIN32
      GLFWwindow *backup_current_context = glfwGetCurrentContext();
#endif
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
#ifndef _WIN32
      glfwMakeContextCurrent(backup_current_context);
#endif
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

void Application::RenderUI() {
  for (auto &window : windows_) {
    window->Render(custom_font_, preview_font_);
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
