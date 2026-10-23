#include "cheat_file.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shelf {

  namespace {

    bool ReadWholeFile(const std::string &path, std::string &out) {
      FILE *f = fopen(path.c_str(), "rb");
      if (f == nullptr) {
        return false;
      }

      char buf[0x1000];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
      }
      fclose(f);
      return true;
    }

    void SkipLine(const std::string &s, size_t &i) {
      while (i < s.size() && s[i] != '\n') {
        i++;
      }
    }

  }  // namespace

  bool ParseCheatFile(const std::string &path, CheatFile &out) {
    std::string s;
    if (!ReadWholeFile(path, s)) {
      return false;
    }

    out = {};

    // Every block in file order, folder markers included; folders are built once all are read.
    std::vector<Cheat> blocks;
    std::vector<bool> dropped;

    // Index of the block that receives opcodes, or -1 when the current block takes none.
    ssize_t current = -1;

    size_t i = 0;
    while (i < s.size()) {
      const unsigned char c = s[i];

      if (std::isspace(c)) {
        i++;
      } else if (c == '[' || c == '{') {
        const char close = c == '[' ? ']' : '}';
        const size_t end = s.find(close, i + 1);
        const size_t eol = s.find('\n', i + 1);
        if (end == std::string::npos || (eol != std::string::npos && eol < end)) {
          out.skipped_lines++;
          SkipLine(s, i);
          continue;
        }

        const std::string name = s.substr(i + 1, end - i - 1);
        i = end + 1;

        Cheat cheat = {};
        cheat.is_master = c == '{';
        // Same 0x40 byte limit as dmnt, keeping room for the terminator.
        strncpy(cheat.definition.readable_name, name.c_str(), sizeof(cheat.definition.readable_name) - 1);
        blocks.push_back(cheat);
        dropped.push_back(false);
        current = blocks.size() - 1;
      } else if (std::isxdigit(c)) {
        bool valid = i + 8 <= s.size();
        for (size_t j = 0; valid && j < 8; j++) {
          valid = std::isxdigit((unsigned char)s[i + j]);
        }
        // An opcode is exactly 8 hex digits, anything longer is text that happens to start with hex.
        if (valid && i + 8 < s.size() && !std::isspace((unsigned char)s[i + 8])) {
          valid = false;
        }

        if (current < 0) {
          out.skipped_lines += !valid;
          SkipLine(s, i);
          continue;
        }

        auto &def = blocks[current].definition;
        // A cheat missing part of its code would write garbage, and dmnt rejects more than 0x100 opcodes,
        // so either way the whole cheat is dropped.
        if (!valid || def.num_opcodes >= sizeof(def.opcodes) / sizeof(def.opcodes[0])) {
          dropped[current] = true;
          out.skipped_lines++;
          current = -1;
          SkipLine(s, i);
          continue;
        }

        char hex[9] = {};
        memcpy(hex, s.data() + i, 8);
        def.opcodes[def.num_opcodes++] = strtoul(hex, nullptr, 16);
        i += 8;
      } else {
        out.skipped_lines++;
        SkipLine(s, i);
      }
    }

    // Markers and cheats go through BuildSections in file order; masters and empty entries stay out.
    std::vector<const DmntCheatDefinition *> ordered;
    std::vector<size_t> ordered_to_cheat;
    for (size_t n = 0; n < blocks.size(); n++) {
      const auto &block = blocks[n];
      if (dropped[n]) {
        continue;
      }
      const bool marker = !block.is_master && MarkerOf(block.definition) != Marker::None;
      if (!marker && block.definition.num_opcodes == 0) {
        out.notes.push_back(block.definition.readable_name);
        continue;
      }
      if (block.is_master) {
        out.cheats.push_back(block);
        continue;
      }
      ordered.push_back(&block.definition);
      if (marker) {
        ordered_to_cheat.push_back(SIZE_MAX);
      } else {
        ordered_to_cheat.push_back(out.cheats.size());
        out.cheats.push_back(block);
      }
    }

    out.sections = BuildSections(ordered, out.notes);
    for (const auto *def : ordered) {
      out.entries.push_back(*def);
    }
    for (auto &section : out.sections) {
      for (size_t &idx : section.cheats) {
        idx = ordered_to_cheat[idx];
      }
    }

    return true;
  }

}  // namespace shelf
