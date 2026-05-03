#include "ChecksGraphWindow.h"
#include "Application.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

ChecksGraphWindow::ChecksGraphWindow(Application &app)
    : Window("Checks Graph"), app_(app) {}

void ChecksGraphWindow::Render(std::tm *current_tm, ImFont *custom_font,
                                ImFont *preview_font,
                                ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    // Snapshot the data we need under the mutex, then render outside the
    // lock so ImGui calls don't run with the application mutex held.
    std::vector<float> values;
    float max_val = 0.0f;
    int last_checked = 0;
    int last_total = 0;
    double elapsed_secs = 0.0;
    size_t history_size = 0;
    app_.WithChecksHistory([&](const std::vector<Application::ChecksSnapshot>
                                   &history) {
      history_size = history.size();
      if (history.size() < 2)
        return;

      static constexpr size_t kMaxDisplayPoints = 2000;
      if (history.size() <= kMaxDisplayPoints) {
        values.reserve(history.size());
        for (const auto &snap : history) {
          float v = static_cast<float>(snap.checked_locations);
          values.push_back(v);
          max_val = std::max(max_val, v);
        }
      } else {
        values.reserve(kMaxDisplayPoints);
        double stride = static_cast<double>(history.size() - 1) /
                        (kMaxDisplayPoints - 1);
        for (size_t i = 0; i < kMaxDisplayPoints; ++i) {
          size_t idx = std::min(static_cast<size_t>(i * stride),
                                history.size() - 1);
          float v = static_cast<float>(history[idx].checked_locations);
          values.push_back(v);
          max_val = std::max(max_val, v);
        }
      }
      last_checked = history.back().checked_locations;
      last_total = history.back().total_locations;
      elapsed_secs = history.back().timestamp - history.front().timestamp;
    });

    ImGui::BeginDisabled(history_size == 0);
    if (ImGui::Button("Clear History")) {
      app_.ClearChecksHistory();
    }
    ImGui::EndDisabled();

    if (history_size == 0) {
      ImGui::TextDisabled("Waiting for tracker data...");
      ImGui::End();
      return;
    }

    if (history_size < 2) {
      ImGui::TextDisabled("Waiting for more data points...");
      ImGui::End();
      return;
    }

    float total = static_cast<float>(last_total);
    max_val = std::max(max_val, total);

    int elapsed_min = static_cast<int>(elapsed_secs / 60.0);
    int elapsed_hr = elapsed_min / 60;
    elapsed_min %= 60;

    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%d/%d (tracked %dh%dm)", last_checked,
             last_total, elapsed_hr, elapsed_min);

    ImVec2 graph_size = ImVec2(-1, ImGui::GetContentRegionAvail().y - 30);
    ImGui::PlotLines("##ChecksGraph", values.data(),
                     static_cast<int>(values.size()), 0, overlay, 0.0f,
                     max_val * 1.05f, graph_size);

    float pct = (total > 0) ? (values.back() / total * 100.0f) : 0.0f;
    ImGui::Text("Progress: %.1f%%  |  Data points: %zu", pct, history_size);
  }
  ImGui::End();
}
