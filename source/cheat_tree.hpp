#pragma once

#include <cstdint>
#include <dmntcht.h>
#include <string>
#include <vector>

namespace shelf {

  // Cheat files mark folders with entries that are not cheats:
  //   EdiZon-Overlay:   [--SectionStart:Name--] ... [--SectionEnd:Name--]
  //   Breeze/EdiZon-SE: [Name] 20000000           ... [Name end] 20000001
  // Atmosphere loads both as ordinary cheats, so they also show up in what dmnt holds.
  enum class Marker { None, Start, End };

  // dmnt may fill all 0x40 name bytes without a terminator.
  std::string NameOf(const DmntCheatDefinition &def);

  Marker MarkerOf(const DmntCheatDefinition &def);

  struct Section {
    std::string name;
    std::vector<size_t> cheats;    // indices into the list given to BuildSections
    std::vector<size_t> sections;  // indices into the returned sections
    // Its start and end markers, indices into the list given to BuildSections (none for the top level).
    size_t begin = SIZE_MAX;
    size_t end = SIZE_MAX;
  };

  // Groups defs, in order, into folders; sections[0] is the top level, markers are left out.
  // A start marker without a matching end (a credit line, often) is not a folder: its name goes
  // to labels, so it does not swallow everything after it.
  std::vector<Section> BuildSections(const std::vector<const DmntCheatDefinition *> &defs, std::vector<std::string> &labels);

}  // namespace shelf
