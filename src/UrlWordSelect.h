#pragma once
#include "imgui.h"
#include <string>

// Replaces ImGui's word boundaries with URL-aware ones for a single InputText.
//
// ImGui's separator set (ImCharIsSeparatorW in imgui_widgets.cpp) contains '.'
// and '/' but not ':', '@', '?', '&', '=' or '#', so double-clicking the port
// in "archipelago.gg:12345" selects "gg:12345". Here anything that isn't
// alphanumeric, '-' or '_' is a boundary, so host labels, the port and path
// segments each select on their own.
//
// ImGui exposes no hook for this and lives in a git submodule, so rather than
// patch it we let it apply its own word logic and re-snap the result: the
// ImGuiInputTextFlags_CallbackAlways callback runs later in the same frame than
// both the mouse and the keyboard handling, and CursorPos/SelectionStart/
// SelectionEnd are writable there. Covers double-click, double-click drag,
// Ctrl+Left/Right (with or without Shift) and Ctrl+Backspace/Delete.
//
// Usage — keep one instance alive alongside the buffer:
//
//   ImGui::InputText(label, buf, sizeof(buf),
//                    flags | ImGuiInputTextFlags_CallbackAlways,
//                    &UrlWordSelect::Callback, &url_word_select_);
class UrlWordSelect {
public:
  static int Callback(ImGuiInputTextCallbackData *data);

private:
  int Apply(ImGuiInputTextCallbackData *data);
  void Reset();

  ImGuiID id_ = 0;
  // End-of-previous-frame state. Keyboard fix-ups need to recompute from where
  // things stood before ImGui applied its own boundaries this frame.
  int prev_cursor_ = -1;
  int prev_select_start_ = -1;
  int prev_select_end_ = -1;
  std::string prev_text_;

  int click_pos_ = -1;    // char index of the most recent single click
  int anchor_start_ = -1; // word grabbed by the double-click...
  int anchor_end_ = -1;   // ...which a subsequent drag extends from
  int drag_pos_ = -1;     // char index the drag has last reached
  bool word_drag_ = false;
};
