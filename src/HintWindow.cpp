#include "HintWindow.h"
#include <algorithm>

HintWindow::HintWindow(
    const std::vector<Hint> &hints,
    const std::map<int, std::string> &player_names,
    const std::map<std::string, std::map<int64_t, std::string>> &item_names,
    const std::map<std::string, std::map<int64_t, std::string>> &location_names,
    const std::map<int, std::string> &slot_to_game,
    std::function<int()> get_global_slot, const ConnectionSettings &settings,
    const std::string &name)
    : Window(name), hints_(hints), player_names_(player_names),
      item_names_(item_names), location_names_(location_names),
      slot_to_game_(slot_to_game), get_global_slot_(get_global_slot),
      settings_(settings) {}

void HintWindow::Render(ImFont *custom_font, ImFont *preview_font,
                        ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-1.0f);
    ImGui::InputText("##Filter", filter_text_, sizeof(filter_text_));
    ImGui::PopItemWidth();

    ImGui::Separator();

    ImGui::Separator();

    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("HintTable", 5, flags)) {
      ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Receiver", ImGuiTableColumnFlags_WidthFixed,
                              100.0f);
      ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Finder", ImGuiTableColumnFlags_WidthFixed,
                              100.0f);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                              80.0f);
      ImGui::TableHeadersRow();
      if (custom_font)
        ImGui::PushFont(custom_font);

      std::string l_filter = filter_text_;
      std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                     ::tolower);

      ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs();

      bool needs_resort = false;
      if (sorted_indices_.size() != hints_.size()) {
        sorted_indices_.resize(hints_.size());
        for (size_t i = 0; i < hints_.size(); ++i) {
          sorted_indices_[i] = i;
        }
        needs_resort = true;
      }

      if (sorts_specs && sorts_specs->SpecsDirty) {
        needs_resort = true;
        sorts_specs->SpecsDirty = false;
      }

      if (needs_resort && sorts_specs && hints_.size() > 1 &&
          sorts_specs->SpecsCount > 0) {
        const auto *spec = &sorts_specs->Specs[0];
        auto sort_func = [this, spec](int a, int b) {
          const Hint &hA = hints_[a];
          const Hint &hB = hints_[b];

          int delta = 0;
          if (spec->ColumnIndex == 0) { // Item
            auto resolve_item = [&](const Hint &h) {
              std::string game = "";
              if (h.receiver_slot != -1 && slot_to_game_.count(h.receiver_slot))
                game = slot_to_game_.at(h.receiver_slot);
              if (!game.empty() && item_names_.count(game) &&
                  item_names_.at(game).count(h.item_id))
                return item_names_.at(game).at(h.item_id);
              for (auto const &[gn, items] : item_names_)
                if (items.count(h.item_id))
                  return items.at(h.item_id);
              return std::string("Unknown");
            };
            delta = resolve_item(hA).compare(resolve_item(hB));
          } else if (spec->ColumnIndex == 1) { // Receiver
            std::string nA = player_names_.count(hA.receiver_slot)
                                 ? player_names_.at(hA.receiver_slot)
                                 : std::to_string(hA.receiver_slot);
            std::string nB = player_names_.count(hB.receiver_slot)
                                 ? player_names_.at(hB.receiver_slot)
                                 : std::to_string(hB.receiver_slot);
            delta = nA.compare(nB);
          } else if (spec->ColumnIndex == 2) { // Location
            auto resolve_loc = [&](const Hint &h) {
              std::string game = "";
              if (h.finder_slot != -1 && slot_to_game_.count(h.finder_slot))
                game = slot_to_game_.at(h.finder_slot);
              if (!game.empty() && location_names_.count(game) &&
                  location_names_.at(game).count(h.location_id))
                return location_names_.at(game).at(h.location_id);
              for (auto const &[gn, locs] : location_names_)
                if (locs.count(h.location_id))
                  return locs.at(h.location_id);
              return std::string("Unknown");
            };
            delta = resolve_loc(hA).compare(resolve_loc(hB));
          } else if (spec->ColumnIndex == 3) { // Finder
            std::string nA = player_names_.count(hA.finder_slot)
                                 ? player_names_.at(hA.finder_slot)
                                 : std::to_string(hA.finder_slot);
            std::string nB = player_names_.count(hB.finder_slot)
                                 ? player_names_.at(hB.finder_slot)
                                 : std::to_string(hB.finder_slot);
            delta = nA.compare(nB);
          } else if (spec->ColumnIndex == 4) { // Status
            delta = (int)hA.found - (int)hB.found;
          }
          if (delta == 0)
            delta = a - b;
          return (spec->SortDirection == ImGuiSortDirection_Ascending)
                     ? (delta < 0)
                     : (delta > 0);
        };
        std::sort(sorted_indices_.begin(), sorted_indices_.end(), sort_func);
      }

      int global_slot = get_global_slot_ ? get_global_slot_() : -1;

      for (int orig_idx : sorted_indices_) {
        const auto &h = hints_[orig_idx];
        std::string item = "Unknown Item";
        std::string i_game = "";
        if (h.receiver_slot != -1 && slot_to_game_.count(h.receiver_slot))
          i_game = slot_to_game_.at(h.receiver_slot);
        if (!i_game.empty() && item_names_.count(i_game) &&
            item_names_.at(i_game).count(h.item_id)) {
          item = item_names_.at(i_game).at(h.item_id);
        } else {
          // Fallback to searching all games if specific game not found or item
          // not in specific game
          for (auto const &[gn, items] : item_names_) {
            if (items.count(h.item_id)) {
              item = items.at(h.item_id);
              break;
            }
          }
        }

        std::string receiver =
            player_names_.count(h.receiver_slot)
                ? player_names_.at(h.receiver_slot)
                : (h.receiver_slot != -1
                       ? "Slot " + std::to_string(h.receiver_slot)
                       : "Unknown Player");

        std::string location = "Unknown Location";
        std::string l_game = "";
        if (h.finder_slot != -1 && slot_to_game_.count(h.finder_slot))
          l_game = slot_to_game_.at(h.finder_slot);
        if (!l_game.empty() && location_names_.count(l_game) &&
            location_names_.at(l_game).count(h.location_id)) {
          location = location_names_.at(l_game).at(h.location_id);
        } else {
          for (auto const &[gn, locs] : location_names_) {
            if (locs.count(h.location_id)) {
              location = locs.at(h.location_id);
              break;
            }
          }
        }

        std::string finder =
            player_names_.count(h.finder_slot)
                ? player_names_.at(h.finder_slot)
                : (h.finder_slot != -1 ? "Slot " + std::to_string(h.finder_slot)
                                       : "Unknown Player");
        std::string status = h.found ? "Found" : "Not Found";

        if (!l_filter.empty()) {
          std::string l_all = item + " " + receiver + " " + location + " " +
                              finder + " " + status;
          std::transform(l_all.begin(), l_all.end(), l_all.begin(), ::tolower);
          if (l_all.find(l_filter) == std::string::npos)
            continue;
        }

        ImGui::TableNextRow();

        // Item column with flags color
        ImGui::TableSetColumnIndex(0);

        ImGui::PushID(orig_idx);
        char label[32];
        snprintf(label, sizeof(label), "##hintrow_%d", orig_idx);
        bool is_selected = false;
        ImGui::Selectable(label, is_selected,
                          ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0, 0));

        if (ImGui::BeginPopupContextItem("HintCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
          if (ImGui::MenuItem("Copy Hint Text (with markdown)")) {
            std::string cb = "**[Hint]:** " + receiver + "'s *" + item +
                             "* is at **" + location + "** in **" + finder +
                             "**'s World. (" +
                             (h.found ? "found" : "not found") + ")";
            ImGui::SetClipboardText(cb.c_str());
          }
          ImGui::EndPopup();
        }
        ImGui::PopID();

        ImGui::SameLine(0, 0);

        uint32_t item_color = 0xFFFFFFFF; // White Opaque (AABBGGRR)
        if (h.item_flags & 0x01)
          item_color = 0xFFFF5FAF; // Purple
        else if (h.item_flags & 0x02)
          item_color = 0xFFED9564; // Blue
        else if (h.item_flags & 0x04)
          item_color = 0xFF0045FF; // Red
        else
          item_color = 0xFFFFFF00; // Cyan

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(item_color), "%s",
                           item.c_str());

        // Receiver column: Magenta if it's us
        ImGui::TableSetColumnIndex(1);
        if (h.receiver_slot == global_slot) {
          ImGui::TextColored(ImColor(255, 0, 255), "%s", receiver.c_str());
        } else {
          ImGui::Text("%s", receiver.c_str());
        }

        // Location column: Green
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImColor(0, 255, 0), "%s", location.c_str());

        ImGui::TableSetColumnIndex(3);
        if (h.finder_slot == global_slot) {
          ImGui::TextColored(ImColor(255, 0, 255), "%s", finder.c_str());
        } else {
          ImGui::Text("%s", finder.c_str());
        }

        // Status column: Green if found, Red if not, Yellow if unknown
        ImGui::TableSetColumnIndex(4);
        uint32_t status_color = 0xFF00FFFF; // Yellow Opaque (R+G)
        if (status == "Found") {
          status_color = 0xFF00FF00; // Green Opaque
        } else if (status == "Not Found") {
          status_color = 0xFF0000FF; // Red Opaque
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(status_color), "%s",
                           status.c_str());
      }
      if (custom_font)
        ImGui::PopFont();
      ImGui::EndTable();
    }
  }
  ImGui::End();
}
