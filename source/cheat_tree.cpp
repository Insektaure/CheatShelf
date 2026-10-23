#include "cheat_tree.hpp"

#include <cstring>

namespace shelf {

  namespace {

    constexpr const char *SectionStartTag = "--SectionStart:";
    constexpr const char *SectionEndTag = "--SectionEnd:";

    constexpr uint32_t FolderStartOpcode = 0x20000000;
    constexpr uint32_t FolderEndOpcode = 0x20000001;

    bool StartsWith(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

    // "--SectionStart:Item Codes--" -> "Item Codes", "Player" -> "Player"
    std::string FolderName(const DmntCheatDefinition &def) {
      std::string name = NameOf(def);
      if (StartsWith(name.c_str(), SectionStartTag)) {
        name = name.substr(strlen(SectionStartTag));
        if (name.size() >= 2 && name.compare(name.size() - 2, 2, "--") == 0) {
          name.resize(name.size() - 2);
        }
      }
      return name;
    }

  }  // namespace

  std::string NameOf(const DmntCheatDefinition &def) {
    const void *nul = memchr(def.readable_name, 0, sizeof(def.readable_name));
    return std::string(def.readable_name, nul ? (const char *)nul - def.readable_name : sizeof(def.readable_name));
  }

  Marker MarkerOf(const DmntCheatDefinition &def) {
    if (StartsWith(def.readable_name, SectionStartTag)) {
      return Marker::Start;
    }
    if (StartsWith(def.readable_name, SectionEndTag)) {
      return Marker::End;
    }
    if (def.num_opcodes == 1 && def.opcodes[0] == FolderStartOpcode) {
      return Marker::Start;
    }
    if (def.num_opcodes == 1 && def.opcodes[0] == FolderEndOpcode) {
      return Marker::End;
    }
    return Marker::None;
  }

  std::vector<Section> BuildSections(const std::vector<const DmntCheatDefinition *> &defs, std::vector<std::string> &labels) {
    // Pair the markers first, so an unmatched start is known before it opens anything.
    std::vector<bool> paired(defs.size(), false);
    std::vector<size_t> open;
    for (size_t i = 0; i < defs.size(); i++) {
      const Marker m = MarkerOf(*defs[i]);
      if (m == Marker::Start) {
        open.push_back(i);
      } else if (m == Marker::End && !open.empty()) {
        paired[open.back()] = true;
        paired[i] = true;
        open.pop_back();
      }
    }

    std::vector<Section> sections = { { "", {}, {}, SIZE_MAX, SIZE_MAX } };
    std::vector<size_t> stack = { 0 };
    for (size_t i = 0; i < defs.size(); i++) {
      switch (MarkerOf(*defs[i])) {
        case Marker::Start:
          if (paired[i]) {
            sections.push_back({ FolderName(*defs[i]), {}, {}, i, SIZE_MAX });
            sections[stack.back()].sections.push_back(sections.size() - 1);
            stack.push_back(sections.size() - 1);
          } else {
            labels.push_back(FolderName(*defs[i]));
          }
          break;
        case Marker::End:
          // an unpaired end closes nothing
          if (paired[i] && stack.size() > 1) {
            sections[stack.back()].end = i;
            stack.pop_back();
          }
          break;
        case Marker::None:
          sections[stack.back()].cheats.push_back(i);
          break;
      }
    }
    return sections;
  }

}  // namespace shelf
