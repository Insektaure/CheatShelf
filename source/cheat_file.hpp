#pragma once

#include <dmntcht.h>
#include <string>
#include <vector>

#include "cheat_tree.hpp"

namespace shelf {

  // One [name] or {name} block of an Atmosphere cheat file.
  struct Cheat {
    DmntCheatDefinition definition;
    bool is_master;
  };

  struct CheatFile {
    std::vector<Cheat> cheats;
    std::vector<Section> sections;  // folders over cheats, sections[0] is the top level of the file
    // Everything but the master codes in file order, folder markers included: what "Load all" loads,
    // so the folders show in what dmnt holds too. Section::begin/end index this.
    std::vector<DmntCheatDefinition> entries;
    std::vector<std::string> notes; // empty entries such as [Credits ...] and unpaired folder starts, shown as text
    size_t skipped_lines = 0;       // lines Atmosphere would have rejected
  };

  // Unlike Atmosphere, a malformed line is skipped instead of rejecting the whole file,
  // so readme text at the top of a file does not prevent loading the cheats below it.
  bool ParseCheatFile(const std::string &path, CheatFile &out);

}  // namespace shelf
