#include "UrlWordSelect.h"
#include <algorithm>

namespace {

enum class CharClass { Blank, Word, Separator };

// URLs are ASCII. Bytes >= 0x80 count as word characters so that a pasted
// multi-byte codepoint is never split down the middle by a selection index.
CharClass Classify(char c) {
  unsigned char u = (unsigned char)c;
  if (u == ' ' || u == '\t' || u == '\n' || u == '\r')
    return CharClass::Blank;
  if (u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
      (u >= 'a' && u <= 'z') || u == '-' || u == '_')
    return CharClass::Word;
  return CharClass::Separator;
}

// Extent of the run of same-class characters containing `pos`.
void WordSpan(const char *buf, int len, int pos, int *out_start, int *out_end) {
  if (len <= 0) {
    *out_start = *out_end = 0;
    return;
  }
  int p = pos < 0 ? 0 : (pos >= len ? len - 1 : pos);
  CharClass cls = Classify(buf[p]);
  int s = p;
  while (s > 0 && Classify(buf[s - 1]) == cls)
    --s;
  int e = p + 1;
  while (e < len && Classify(buf[e]) == cls)
    ++e;
  *out_start = s;
  *out_end = e;
}

// Start of the run to the left of `pos` (Ctrl+Left, Ctrl+Backspace).
int PrevBoundary(const char *buf, int len, int pos) {
  int p = pos > len ? len : pos;
  if (p <= 0)
    return 0;
  int i = p - 1;
  CharClass cls = Classify(buf[i]);
  while (i > 0 && Classify(buf[i - 1]) == cls)
    --i;
  return i;
}

// End of the run to the right of `pos` (Ctrl+Right, Ctrl+Delete).
int NextBoundary(const char *buf, int len, int pos) {
  int p = pos < 0 ? 0 : pos;
  if (p >= len)
    return len;
  int i = p;
  CharClass cls = Classify(buf[i]);
  while (i < len && Classify(buf[i]) == cls)
    ++i;
  return i;
}

} // namespace

int UrlWordSelect::Callback(ImGuiInputTextCallbackData *data) {
  return static_cast<UrlWordSelect *>(data->UserData)->Apply(data);
}

void UrlWordSelect::Reset() {
  prev_cursor_ = -1;
  prev_select_start_ = -1;
  prev_select_end_ = -1;
  prev_text_.clear();
  click_pos_ = -1;
  anchor_start_ = -1;
  anchor_end_ = -1;
  drag_pos_ = -1;
  word_drag_ = false;
}

int UrlWordSelect::Apply(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways)
    return 0;

  // The masked and unmasked server URL fields are distinct widgets; don't carry
  // one's click/drag state into the other.
  if (data->ID != id_) {
    Reset();
    id_ = data->ID;
  }

  ImGuiIO &io = ImGui::GetIO();
  const char *buf = data->Buf;
  const int len = data->BufTextLen;
  const bool is_osx = io.ConfigMacOSXBehaviors;
  const bool wordmove = is_osx ? io.KeyAlt : io.KeyCtrl;
  // On macOS Ctrl+Arrow is line start/end rather than word motion — hands off.
  const bool startend = is_osx && io.KeyCtrl && !io.KeySuper && !io.KeyAlt;

  const int clicks = io.MouseClickedCount[0];
  if (clicks == 1) {
    // A plain click leaves the cursor on the clicked character. Remember it:
    // the double-click lands on a later frame, by which point ImGui has already
    // expanded the cursor to its own word and the click point is gone.
    click_pos_ = data->CursorPos;
    word_drag_ = false;
  } else if (clicks >= 2) {
    if (((clicks - 2) % 2) == 0 && !io.KeyShift) {
      // Double-click. ImGui's word contains the clicked character, so clamp to
      // it in case the pointer drifted between the two clicks.
      int lo = std::min(data->SelectionStart, data->SelectionEnd);
      int hi = std::max(data->SelectionStart, data->SelectionEnd);
      int seed = click_pos_ >= 0 ? std::min(std::max(click_pos_, lo), hi)
                                 : data->CursorPos;
      int s, e;
      WordSpan(buf, len, seed, &s, &e);
      data->SetSelection(s, e);
      anchor_start_ = s;
      anchor_end_ = e;
      drag_pos_ = -1;
      word_drag_ = true;
    } else {
      word_drag_ = false; // triple-click selects the line; that stays ImGui's
    }
  } else if (word_drag_ && io.MouseDown[0]) {
    // Double-click drag: ImGui drags by character, so re-snap to whole words,
    // keeping the double-clicked word selected throughout. ImGui only moves the
    // cursor on frames where the mouse actually moved, and we overwrite
    // CursorPos below, so latch the reached position rather than re-reading it.
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
      drag_pos_ = data->CursorPos;
    if (drag_pos_ >= 0) {
      int s, e;
      WordSpan(buf, len, drag_pos_, &s, &e);
      if (drag_pos_ >= anchor_end_)
        data->SetSelection(anchor_start_, e);
      else if (drag_pos_ <= anchor_start_)
        data->SetSelection(anchor_end_, s);
      else
        data->SetSelection(anchor_start_, anchor_end_);
    }
  }
  if (!io.MouseDown[0])
    word_drag_ = false;

  // Keyboard word motion. ImGui has already moved the cursor using its own
  // boundaries, so recompute from where the cursor sat last frame.
  const int prev_len = (int)prev_text_.size();
  if (wordmove && !startend && prev_cursor_ >= 0 && prev_cursor_ <= prev_len) {
    const char *prev = prev_text_.c_str();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
      int p = PrevBoundary(buf, len, prev_cursor_);
      data->SetSelection(io.KeyShift ? data->SelectionStart : p, p);
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
      int p = NextBoundary(buf, len, prev_cursor_);
      data->SetSelection(io.KeyShift ? data->SelectionStart : p, p);
    } else if (!(data->Flags & ImGuiInputTextFlags_ReadOnly) && !io.KeyShift &&
               prev_select_start_ == prev_select_end_) {
      // Ctrl+Backspace / Ctrl+Delete already deleted out to ImGui's boundary.
      // Every ImGui separator is a separator here too, so ImGui can only ever
      // have deleted a superset of what we want — put the overshoot back.
      const int deleted = prev_len - len;
      if (deleted > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
          const int removed_from = prev_cursor_ - deleted;
          const int want = PrevBoundary(prev, prev_len, prev_cursor_);
          if (removed_from >= 0 && data->CursorPos == removed_from &&
              want > removed_from)
            data->InsertChars(removed_from, prev + removed_from, prev + want);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
          const int removed_to = prev_cursor_ + deleted;
          const int want = NextBoundary(prev, prev_len, prev_cursor_);
          if (removed_to <= prev_len && data->CursorPos == prev_cursor_ &&
              want < removed_to) {
            data->InsertChars(prev_cursor_, prev + want, prev + removed_to);
            data->SetSelection(prev_cursor_, prev_cursor_); // deleting forward
          }                                                 // leaves the cursor
        }                                                   // where it was
      }
    }
  }

  prev_cursor_ = data->CursorPos;
  prev_select_start_ = data->SelectionStart;
  prev_select_end_ = data->SelectionEnd;
  prev_text_.assign(data->Buf, data->BufTextLen);
  return 0;
}
