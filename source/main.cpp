#define TESLA_INIT_IMPL

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <memory>
#include <tesla.hpp>
#include <zlib.h>

#include "cheat_file.hpp"
#include "cheat_state.hpp"

using namespace shelf;

namespace {

  constexpr const char *RootDir = "sdmc:/config/cheatshelf/cheats/";

  std::string TitleIdHex() {
    char tid[17];
    snprintf(tid, sizeof(tid), "%016lX", State().TitleId());
    return tid;
  }

  // The game's name from its control data, read at startup (ns needs the sm session libtesla only
  // keeps open there). Empty when unknown.
  std::string g_game_title;

  std::string FirstName(const NacpLanguageEntry *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
      const char *name = entries[i].name;
      if (name[0] != 0) {
        const void *nul = memchr(name, 0, sizeof(entries[i].name));
        return std::string(name, nul ? (const char *)nul - name : sizeof(entries[i].name));
      }
    }
    return "";
  }

  // Since HOS 21.0.0 the names can be stored compressed, which libnx does not decode: the byte at
  // 0x3215 is then 1, and the 0x3000 bytes of names hold a u16 size and raw deflate data that
  // inflates to 32 entries instead of 16. Same handling as sphaira's NormalizeNacpLangData.
  std::string NacpTitle(const NacpStruct &nacp) {
    constexpr size_t NamesSize = 0x3000;
    constexpr size_t NamesFormatOffset = 0x3215;
    constexpr size_t CompressedCount = 32;
    static_assert(sizeof(NacpLanguageEntry) * 16 == NamesSize);
    static_assert(sizeof(NacpStruct) > NamesFormatOffset);

    const u8 *raw = reinterpret_cast<const u8 *>(&nacp);
    const auto *names = reinterpret_cast<const NacpLanguageEntry *>(raw);
    if (raw[NamesFormatOffset] != 1) {
      return FirstName(names, 16);
    }

    u16 compressed_size = 0;
    memcpy(&compressed_size, raw, sizeof(compressed_size));
    if (compressed_size == 0 || compressed_size > NamesSize - sizeof(u16)) {
      return "";
    }

    auto inflated = std::make_unique<NacpLanguageEntry[]>(CompressedCount);
    z_stream stream = {};
    if (inflateInit2(&stream, -15) != Z_OK) {
      return "";
    }
    stream.next_in = const_cast<Bytef *>(raw + sizeof(u16));
    stream.avail_in = compressed_size;
    stream.next_out = reinterpret_cast<Bytef *>(inflated.get());
    stream.avail_out = sizeof(NacpLanguageEntry) * CompressedCount;
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return result == Z_STREAM_END ? FirstName(inflated.get(), CompressedCount) : "";
  }

  std::string LookupGameTitle(u64 title_id) {
    if (R_FAILED(nsInitialize())) {
      return "";
    }
    std::string title;
    auto data = std::make_unique<NsApplicationControlData>();
    u64 size = 0;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, title_id, data.get(), sizeof(*data), &size))) {
      // First language with a name, American English for most games.
      title = NacpTitle(data->nacp);
    }
    nsExit();
    return title;
  }

  // "Pokémon Legends: Z-A" -> "Pokémon Legends Z-A", FAT refuses these characters.
  // Length of the valid UTF-8 sequence at s[i], 0 if it is not one.
  size_t Utf8Length(const std::string &s, size_t i) {
    const unsigned char c = s[i];
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0 || (n == 2 && c < 0xC2) || i + n > s.size()) {
      return 0;
    }
    for (size_t k = 1; k < n; k++) {
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
        return 0;
      }
    }
    return n;
  }

  bool IsValidUtf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
      const size_t n = Utf8Length(s, i);
      if (n == 0) {
        return false;
      }
      i += n;
    }
    return true;
  }

  // é -> e, Æ -> AE... for the Latin-1 letters; "" for anything else outside ASCII.
  const char *AsciiFor(u32 cp) {
    static const char *const Latin1[0x40] = {
      "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",  // U+00C0
      "D", "N", "O", "O", "O", "O", "O", "",  "O", "U", "U", "U", "U", "Y", "",  "ss", // U+00D0
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  // U+00E0
      "d", "n", "o", "o", "o", "o", "o", "",  "o", "u", "u", "u", "u", "y", "",  "y",  // U+00F0
    };
    if (cp >= 0xC0 && cp <= 0xFF) {
      return Latin1[cp - 0xC0];
    }
    if (cp == 0x152) {
      return "OE";
    }
    if (cp == 0x153) {
      return "oe";
    }
    return "";
  }

  // Plain ASCII, what every MTP and FTP client and PC shows: accents are dropped (é -> e), other
  // characters such as ® ™ or Japanese removed, and so are bytes that are not UTF-8.
  // Horizon refuses :*?<>| in paths, FAT also "\/ and control characters (some names have a line
  // break, to show on two lines in the HOME menu). Spaces are collapsed, and FAT drops trailing dots.
  std::string FolderSafe(const std::string &raw) {
    std::string name;
    for (size_t i = 0; i < raw.size();) {
      const size_t n = Utf8Length(raw, i);
      if (n == 0) {
        i++;
        continue;
      }
      if (n == 1) {
        name += raw[i];
      } else {
        u32 cp = (unsigned char)raw[i] & (0x7F >> n);
        for (size_t k = 1; k < n; k++) {
          cp = (cp << 6) | ((unsigned char)raw[i + k] & 0x3F);
        }
        name += AsciiFor(cp);
      }
      i += n;
    }

    std::string out;
    for (char c : name) {
      const unsigned char u = c;
      if (u < 0x20 || u == 0x7F) {
        c = ' ';
      } else if (strchr("\\/:*?\"<>|", c)) {
        continue;
      }
      if (c == ' ' && (out.empty() || out.back() == ' ')) {
        continue;
      }
      out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
      out.pop_back();
    }
    return out;
  }

  // "0100606026F12000", or any name ending with "[0100606026F12000]".
  bool IsGameFolder(const std::string &name, const std::string &tid) {
    if (strcasecmp(name.c_str(), tid.c_str()) == 0) {
      return true;
    }
    const std::string tag = "[" + tid + "]";
    return name.size() >= tag.size() && strcasecmp(name.c_str() + name.size() - tag.size(), tag.c_str()) == 0;
  }

  // Every missing directory of path, which ends with '/'.
  // 0, or the errno of the first directory that could not be made.
  int MakeDirs(const std::string &path) {
    for (size_t slash = path.find('/', strlen("sdmc:/")); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      const std::string dir = path.substr(0, slash);
      if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
        return errno;
      }
    }
    return 0;
  }

  // What other tools can show: valid UTF-8, nothing FAT or Horizon refuses.
  bool IsReadableName(const std::string &name) { return IsValidUtf8(name) && FolderSafe(name) == name; }

  // "<game name> [<title id>]", or the title id alone when the name is unknown.
  std::string NamedFolder() {
    const std::string title = FolderSafe(g_game_title);
    return title.empty() ? TitleIdHex() : title + " [" + TitleIdHex() + "]";
  }

  // sdmc:/config/cheatshelf/cheats/<game name> [<title id>]/<build id>/. When there is no folder for the game
  // yet, the path suggested to create, named after the game.
  std::string GameDir() {
    const std::string tid = TitleIdHex();
    std::vector<std::string> matches;
    if (DIR *dir = opendir(RootDir)) {
      while (dirent *ent = readdir(dir)) {
        if (ent->d_type == DT_DIR && IsGameFolder(ent->d_name, tid)) {
          matches.push_back(ent->d_name);
        }
      }
      closedir(dir);
    }

    // A readable name first, when an unreadable one (see AddFolderRepair) is also there.
    std::sort(matches.begin(), matches.end(), [](const std::string &a, const std::string &b) {
      const bool a_ok = IsReadableName(a), b_ok = IsReadableName(b);
      return a_ok != b_ok ? a_ok : a < b;
    });
    const std::string folder = matches.empty() ? NamedFolder() : matches.front();
    return std::string(RootDir) + folder + "/" + State().BuildIdHex() + "/";
  }

  // The folder named by the title id only, also recognized: what is created when the name is refused.
  std::string PlainGameDir() { return std::string(RootDir) + TitleIdHex() + "/" + State().BuildIdHex() + "/"; }

  // The file dmnt loads at launch.
  std::string AtmosphereCheatFile() {
    return "sdmc:/atmosphere/contents/" + TitleIdHex() + "/cheats/" + State().BuildIdHex() + ".txt";
  }

  void ShowStatus(tsl::elm::ListItem *item, Status status) {
    switch (status) {
      case Status::On:
        item->setValue("ON");
        break;
      case Status::Off:
        item->setValue("OFF", true);
        break;
      case Status::NotLoaded:
        item->setValue("");
        break;
    }
  }

  // Same 2XXX-YYYY form as the Switch error screen, with the raw value for searching.
  std::string ResultCode(Result rc) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%04u-%04u (0x%X)", 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
    return buf;
  }

  std::string Counter(size_t n, size_t max) { return std::to_string(n) + "/" + std::to_string(max); }

  // Splits text into lines of about max_chars, preferring to break after a space or a '/'
  // (paths), never inside a UTF-8 character. The font is not measured: the list needs the
  // height before anything is drawn.
  std::vector<std::string> Wrap(const std::string &text, size_t max_chars) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (text.size() - pos > max_chars) {
      size_t cut = pos + max_chars;
      size_t sep = text.find_last_of(" /", cut - 1);
      if (sep != std::string::npos && sep > pos) {
        cut = sep + 1;
      } else {
        while (cut > pos + 1 && (text[cut] & 0xC0) == 0x80) {
          cut--;
        }
      }
      lines.push_back(text.substr(pos, cut - pos));
      pos = cut;
      while (pos < text.size() && text[pos] == ' ') {
        pos++;
      }
    }
    if (pos < text.size()) {
      lines.push_back(text.substr(pos));
    }
    return lines;
  }

  // Text to read, not an item to select.
  void AddText(tsl::elm::List *list, const std::vector<std::string> &paragraphs) {
    constexpr size_t MaxChars = 34;
    constexpr s32 LineHeight = 24;
    constexpr u32 FontSize = 18;

    std::vector<std::string> lines;
    for (auto p : paragraphs) {
      for (char &c : p) {
        if ((unsigned char)c < 0x20 || c == 0x7F) {
          c = ' ';
        }
      }
      for (auto &line : Wrap(p, MaxChars)) {
        lines.push_back(std::move(line));
      }
    }
    const s32 height = LineHeight * lines.size() + 12;
    list->addItem(new tsl::elm::CustomDrawer([lines](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32, s32) {
      for (size_t i = 0; i < lines.size(); i++) {
        renderer->drawString(lines[i], false, x + 19, y + LineHeight * (i + 1), FontSize, tsl::defaultTextColor);
      }
    }), height);
  }

  // Where the game's cheat files go, and while that folder does not exist, a button to create it.
  // on_created gets the folder actually made: the plain title id one if the game's name was refused.
  void AddFolderSetup(tsl::elm::List *list, const std::string &path,
                      std::function<void(tsl::elm::ListItem *, const std::string &)> on_created) {
    struct stat st;
    const bool exists = stat(path.c_str(), &st) == 0;
    AddText(list, { exists ? "No .txt files in" : "Cheat files for this game go in", path });
    if (exists) {
      return;
    }
    auto create = new tsl::elm::ListItem("Create this folder");
    create->setClickListener([create, path, on_created](u64 keys) {
      if (!(keys & KEY_A)) {
        return false;
      }
      std::string made = path;
      int err = MakeDirs(made);
      if (err != 0 && made != PlainGameDir()) {
        made = PlainGameDir();
        err = MakeDirs(made);
      }
      if (err != 0) {
        create->setValue("Failed, errno " + std::to_string(err));
        return true;
      }
      // A copy: on_created may replace the Gui, and this listener with it.
      auto callback = on_created;
      callback(create, made);
      return true;
    });
    list->addItem(create);
  }

  // Anything that is not printable ASCII as \xNN, to see exactly what a name holds.
  std::string Escaped(const std::string &name) {
    std::string out;
    for (unsigned char c : name) {
      if (c >= 0x20 && c < 0x7F) {
        out += (char)c;
      } else {
        char hex[5];
        snprintf(hex, sizeof(hex), "\\x%02X", c);
        out += hex;
      }
    }
    return out;
  }

  // Removes dir if it only holds empty folders. Files are never deleted.
  bool RemoveEmptyTree(const std::string &dir) {
    if (DIR *d = opendir(dir.c_str())) {
      std::vector<std::string> subdirs;
      bool has_file = false;
      while (dirent *ent = readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") {
          continue;
        }
        if (ent->d_type == DT_DIR) {
          subdirs.push_back(name);
        } else {
          has_file = true;
        }
      }
      closedir(d);
      if (has_file) {
        return false;
      }
      for (const auto &sub : subdirs) {
        if (!RemoveEmptyTree(dir + "/" + sub)) {
          return false;
        }
      }
    }
    return rmdir(dir.c_str()) == 0;
  }

  // Older builds could create a game folder with a line break in its name, or with bytes of a
  // compressed NACP name read as text: CheatShelf finds it by its [title id], but FTP, MTP, file
  // managers and PCs do not show it. While the game's folder has such a name, a button renames it to the clean one.
  // on_repaired gets the new path of path.
  void AddFolderRepair(tsl::elm::List *list, const std::string &path, std::function<void(const std::string &)> on_repaired) {
    const size_t start = strlen(RootDir);
    const size_t slash = path.find('/', start);
    if (path.compare(0, start, RootDir) != 0 || slash == std::string::npos) {
      return;
    }
    const std::string folder = path.substr(start, slash - start);
    if (IsReadableName(folder)) {
      return;
    }
    // The name it should have had, not a cleaned copy of whatever bytes it holds.
    const std::string clean = NamedFolder();

    AddText(list, { "This game's folder has a name other tools may not show:", Escaped(folder), "Renamed it becomes:", clean });
    auto repair = new tsl::elm::ListItem("Repair folder name");
    repair->setClickListener([repair, path, folder, clean, start, on_repaired](u64 keys) {
      if (!(keys & KEY_A)) {
        return false;
      }
      const std::string from = std::string(RootDir) + folder;
      const std::string to = std::string(RootDir) + clean;
      if (rename(from.c_str(), to.c_str()) != 0) {
        // Renaming refused (the clean name may exist already): when the bad folder holds nothing
        // but empty folders, as the ones CheatShelf creates, it is removed and the clean one made.
        const int rename_err = errno;
        if (!RemoveEmptyTree(from) || MakeDirs(to + path.substr(start + folder.size())) != 0) {
          repair->setValue("Failed, errno " + std::to_string(rename_err));
          return true;
        }
      }
      repair->setValue("Repaired");
      // A copy: on_repaired may replace the Gui, and this listener with it.
      const std::string repaired = to + path.substr(start + folder.size());
      auto callback = on_repaired;
      callback(repaired);
      return true;
    });
    list->addItem(repair);
  }

  // The names earlier builds gave the game's folder: the raw title with only \/:*?"<>| removed, then
  // with control characters as spaces, both before names became ASCII. A name with ® made that way
  // was created but never listed by the Switch (neither here nor over MTP): lookups by its exact
  // name are the only way to reach it.
  std::vector<std::string> LegacyFolderNames() {
    std::vector<std::string> out;
    if (g_game_title.empty()) {
      return out;
    }
    std::string kept, spaced;
    for (char c : g_game_title) {
      if (!strchr("\\/:*?\"<>|", c)) {
        kept += c;
      }
      const unsigned char u = c;
      if (u < 0x20 || u == 0x7F) {
        c = ' ';
      } else if (strchr("\\/:*?\"<>|", c)) {
        continue;
      }
      if (c == ' ' && (spaced.empty() || spaced.back() == ' ')) {
        continue;
      }
      spaced += c;
    }
    for (std::string name : { kept, spaced }) {
      while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
      }
      if (name.empty()) {
        continue;
      }
      name += " [" + TitleIdHex() + "]";
      if (name != NamedFolder() && std::find(out.begin(), out.end(), name) == out.end()) {
        out.push_back(name);
      }
    }
    return out;
  }

  // When such a folder exists: renamed to the ASCII name, which the Switch lists, or removed when
  // that name is taken already (only if it holds nothing but empty folders, files are never deleted).
  void AddHiddenFolderCleanup(tsl::elm::List *list, std::function<void()> on_done) {
    for (const auto &name : LegacyFolderNames()) {
      const std::string hidden = std::string(RootDir) + name;
      struct stat st;
      if (stat(hidden.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        continue;
      }
      AddText(list, { "An earlier CheatShelf made a folder the Switch does not list:", Escaped(name) });
      auto button = new tsl::elm::ListItem("Clean up hidden folder");
      button->setClickListener([button, hidden, on_done](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        const std::string target = std::string(RootDir) + NamedFolder();
        struct stat st;
        bool done = false;
        if (stat(target.c_str(), &st) != 0 && rename(hidden.c_str(), target.c_str()) == 0) {
          button->setValue("Renamed");
          done = true;
        } else if (RemoveEmptyTree(hidden)) {
          button->setValue("Removed");
          done = true;
        } else {
          button->setValue("Failed, errno " + std::to_string(errno));
        }
        if (done) {
          // A copy: on_done may replace the Gui, and this listener with it.
          auto callback = on_done;
          callback();
        }
        return true;
      });
      list->addItem(button);
      return;
    }
  }

}  // namespace

// Every view mirrors dmnt's state, so they all redraw when it changes.
class ShelfGui : public tsl::Gui {
 public:
  void update() override {
    if (m_seen_generation != State().Generation()) {
      m_seen_generation = State().Generation();
      refreshValues();
    }
  }

 protected:
  virtual void refreshValues() = 0;

  // Redraws now, then leaves the error on the item until the next change.
  void report(tsl::elm::ListItem *item, const std::string &err) {
    m_seen_generation = State().Generation();
    refreshValues();
    if (!err.empty()) {
      item->setValue(err);
    }
  }

  u64 m_seen_generation = 0;
};

class MessageGui : public tsl::Gui {
 public:
  // with_folder: also show the game's CheatShelf folder, with a button to create it.
  MessageGui(std::string title, std::vector<std::string> paragraphs, bool with_folder = false)
      : m_title(std::move(title)), m_paragraphs(std::move(paragraphs)), m_with_folder(with_folder) {}

  tsl::elm::Element *createUI() override {
    auto frame = new tsl::elm::OverlayFrame("CheatShelf", m_title, !m_with_folder);
    auto list = new tsl::elm::List();
    AddText(list, m_paragraphs);
    AddText(list, { std::string("CheatShelf ") + APP_VERSION });
    if (m_with_folder) {
      AddFolderRepair(list, GameDir(), [](const std::string &) {});
      AddHiddenFolderCleanup(list, [] {});
      AddFolderSetup(list, GameDir(), [](tsl::elm::ListItem *button, const std::string &made) {
        button->setValue(made == PlainGameDir() ? "Created, title id only" : "Created");
      });
    }
    frame->setContent(list);
    return frame;
  }

 private:
  std::string m_title;
  std::vector<std::string> m_paragraphs;
  bool m_with_folder;
};

// One file, or one EdiZon section of it.
class FileGui : public ShelfGui {
 public:
  // set_master: the file is the game's Atmosphere cheat file, whose master code may be put back.
  FileGui(std::shared_ptr<CheatFile> file, std::string title, size_t section, bool set_master = false)
      : m_file(std::move(file)), m_title(std::move(title)), m_section(section), m_set_master(set_master) {}

  tsl::elm::Element *createUI() override {
    auto frame = new tsl::elm::OverlayFrame(m_title, "A load/unload  Y on/off");
    auto list = new tsl::elm::List();
    const Section &section = m_file->sections[m_section];

    if (m_section == 0) {
      for (size_t i = 0; i < m_file->cheats.size(); i++) {
        if (m_file->cheats[i].is_master) {
          addMaster(list, i);
        }
      }
    }

    // Everything in this level and its folders, markers included, in file order: loaded into
    // free slots in that order, the folders show up in Loaded cheats as well.
    if (!section.cheats.empty() || !section.sections.empty()) {
      auto load_all = new tsl::elm::ListItem("Load all");
      load_all->setClickListener([this, load_all](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        std::string err;
        for (size_t k = rangeBegin(); k < rangeEnd(); k++) {
          if (!(err = State().Load(m_file->entries[k], false)).empty()) {
            break;
          }
        }
        report(load_all, err);
        return true;
      });
      list->addItem(load_all);

      auto unload_all = new tsl::elm::ListItem("Unload all");
      unload_all->setClickListener([this, unload_all](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        std::string err;
        for (size_t k = rangeBegin(); k < rangeEnd(); k++) {
          if (!(err = State().Unload(m_file->entries[k])).empty()) {
            break;
          }
        }
        report(unload_all, err);
        return true;
      });
      list->addItem(unload_all);
    }

    for (size_t idx : section.sections) {
      const Section &sub = m_file->sections[idx];
      auto item = new tsl::elm::ListItem(sub.name);
      item->setValue(std::to_string(sub.cheats.size()), true);
      item->setClickListener([this, idx](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        tsl::changeTo<FileGui>(m_file, m_file->sections[idx].name, idx, m_set_master);
        return true;
      });
      list->addItem(item);
    }

    if (!section.cheats.empty()) {
      list->addItem(new tsl::elm::CategoryHeader("Cheats (" + std::to_string(section.cheats.size()) + ")"));
    }
    for (size_t idx : section.cheats) {
      addCheat(list, idx);
    }

    if (m_section == 0) {
      if (!m_file->notes.empty()) {
        list->addItem(new tsl::elm::CategoryHeader("Notes"));
        AddText(list, m_file->notes);
      }
      if (m_file->skipped_lines > 0) {
        list->addItem(new tsl::elm::CategoryHeader(std::to_string(m_file->skipped_lines) + " unreadable lines skipped"));
      }
      if (m_file->cheats.empty()) {
        list->addItem(new tsl::elm::ListItem("No cheats in this file"));
      }
    }

    frame->setContent(list);
    return frame;
  }

 protected:
  void refreshValues() override {
    for (auto &[item, idx] : m_cheat_items) {
      ShowStatus(item, State().StatusOf(m_file->cheats[idx].definition));
    }
    for (auto &[item, idx] : m_master_items) {
      if (State().IsMaster(m_file->cheats[idx].definition)) {
        item->setValue("matches", true);
      } else if (State().HasMaster()) {
        item->setValue("DIFFERENT");
      } else {
        item->setValue("NOT LOADED");
      }
    }
  }

 private:
  size_t rangeBegin() const { return m_section == 0 ? 0 : m_file->sections[m_section].begin; }
  size_t rangeEnd() const { return m_section == 0 ? m_file->entries.size() : m_file->sections[m_section].end + 1; }

  void addCheat(tsl::elm::List *list, size_t idx) {
    auto item = new tsl::elm::ListItem(m_file->cheats[idx].definition.readable_name);
    item->setClickListener([this, item, idx](u64 keys) {
      const auto &def = m_file->cheats[idx].definition;
      const Status status = State().StatusOf(def);
      if (keys & KEY_A) {
        report(item, status == Status::NotLoaded ? State().Load(def, false) : State().Unload(def));
        return true;
      }
      if (keys & KEY_Y) {
        report(item, State().SetEnabled(def, status != Status::On));
        return true;
      }
      return false;
    });
    list->addItem(item);
    m_cheat_items.emplace_back(item, idx);
  }

  // The master code comes from the game's Atmosphere cheat file, loaded at launch. Cheats written
  // against another master code can hook into code it never installed, so the difference is shown.
  void addMaster(tsl::elm::List *list, size_t idx) {
    auto item = new tsl::elm::ListItem(std::string("Master: ") + m_file->cheats[idx].definition.readable_name);
    if (m_set_master) {
      item->setClickListener([this, item, idx](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        report(item, State().SetMaster(m_file->cheats[idx].definition));
        return true;
      });
    }
    list->addItem(item);
    m_master_items.emplace_back(item, idx);
  }

  std::shared_ptr<CheatFile> m_file;
  std::string m_title;
  size_t m_section;
  bool m_set_master;
  std::vector<std::pair<tsl::elm::ListItem *, size_t>> m_cheat_items;
  std::vector<std::pair<tsl::elm::ListItem *, size_t>> m_master_items;
};

// What dmnt held when the view opened, grouped into the folders its markers describe
// (a cheat file loaded by Atmosphere keeps its folder markers as cheats).
struct LoadedSnapshot {
  std::vector<DmntCheatDefinition> defs;  // non-master entries in slot order, markers included
  std::vector<Section> sections;
  std::vector<std::string> labels;
  std::string master;

  static std::shared_ptr<LoadedSnapshot> Take() {
    auto snap = std::make_shared<LoadedSnapshot>();
    for (const auto &e : State().Entries()) {
      if (e.cheat_id == 0) {
        snap->master = NameOf(e.definition);
      } else {
        snap->defs.push_back(e.definition);
      }
    }
    std::vector<const DmntCheatDefinition *> ordered;
    for (const auto &def : snap->defs) {
      ordered.push_back(&def);
    }
    snap->sections = BuildSections(ordered, snap->labels);
    return snap;
  }
};

// Everything dmnt holds, wherever it came from. The list is a snapshot, so an unloaded cheat
// stays visible and A puts it back (in the first free slot, which is usually where it was).
class LoadedGui : public ShelfGui {
 public:
  LoadedGui() : m_snap(LoadedSnapshot::Take()), m_section(0) {}
  LoadedGui(std::shared_ptr<LoadedSnapshot> snap, size_t section) : m_snap(std::move(snap)), m_section(section) {}

  tsl::elm::Element *createUI() override {
    const bool root = m_section == 0;
    const Section &section = m_snap->sections[m_section];
    auto frame = new tsl::elm::OverlayFrame(root ? "Loaded cheats" : section.name, "A on/off  Y unload/reload");
    auto list = new tsl::elm::List();

    if (root) {
      addBulkActions(list);
      m_count_header = new tsl::elm::CategoryHeader("Loaded");
      list->addItem(m_count_header);
      if (!m_snap->master.empty()) {
        list->addItem(new tsl::elm::ListItem("Master: " + m_snap->master, "always on"));
      }
    }

    for (size_t idx : section.sections) {
      auto item = new tsl::elm::ListItem(m_snap->sections[idx].name);
      item->setValue(std::to_string(m_snap->sections[idx].cheats.size()), true);
      item->setClickListener([this, idx](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        tsl::changeTo<LoadedGui>(m_snap, idx);
        return true;
      });
      list->addItem(item);
    }

    for (size_t idx : section.cheats) {
      addCheat(list, idx);
    }

    if (root) {
      if (!m_snap->labels.empty()) {
        AddText(list, m_snap->labels);
      }
      if (m_snap->defs.empty()) {
        list->addItem(new tsl::elm::ListItem("Nothing loaded"));
      }
    }

    frame->setContent(list);
    return frame;
  }

 protected:
  void refreshValues() override {
    if (m_count_header != nullptr) {
      m_count_header->setValue(Counter(State().UserCheatCount(), MaxUserCheats) + " cheats, " +
                               Counter(State().EnabledOpcodes(), MaxProgramOpcodes) + " code on");
    }
    for (auto &[item, idx] : m_items) {
      const Status status = State().StatusOf(m_snap->defs[idx]);
      if (status == Status::NotLoaded) {
        item->setValue("unloaded", true);
      } else {
        ShowStatus(item, status);
      }
    }
  }

 private:
  template <typename Pred>
  static std::vector<u32> ids(Pred pred) {
    std::vector<u32> out;
    for (const auto &e : State().Entries()) {
      if (e.cheat_id != 0 && pred(e)) {
        out.push_back(e.cheat_id);
      }
    }
    return out;
  }

  void addBulkActions(tsl::elm::List *list) {
    auto all_off = new tsl::elm::ListItem("Turn all off");
    all_off->setClickListener([this, all_off](u64 keys) {
      if (!(keys & KEY_A)) {
        return false;
      }
      std::string err;
      for (u32 id : ids([](const DmntCheatEntry &e) { return e.enabled; })) {
        if (!(err = State().ToggleById(id)).empty()) {
          break;
        }
      }
      report(all_off, err);
      return true;
    });
    list->addItem(all_off);

    auto unload_off = new tsl::elm::ListItem("Unload the ones that are off");
    unload_off->setClickListener([this, unload_off](u64 keys) {
      if (!(keys & KEY_A)) {
        return false;
      }
      std::string err;
      for (u32 id : offIds()) {
        if (!(err = State().RemoveById(id)).empty()) {
          break;
        }
      }
      report(unload_off, err);
      return true;
    });
    list->addItem(unload_off);
  }

  // The cheats that are off, and the markers of every folder left with nothing on inside
  // (subfolders included): a folder only stays while it holds a cheat that is on.
  static std::vector<u32> offIds() {
    std::vector<const DmntCheatEntry *> entries;
    std::vector<const DmntCheatDefinition *> defs;
    for (const auto &e : State().Entries()) {
      if (e.cheat_id != 0) {
        entries.push_back(&e);
        defs.push_back(&e.definition);
      }
    }
    std::vector<std::string> labels;
    const std::vector<Section> sections = BuildSections(defs, labels);

    // Sections are numbered in file order, a folder always after its parent: children first from the back.
    std::vector<bool> active(sections.size(), false);
    for (size_t s = sections.size(); s-- > 0;) {
      for (size_t idx : sections[s].cheats) {
        active[s] = active[s] || entries[idx]->enabled;
      }
      for (size_t sub : sections[s].sections) {
        active[s] = active[s] || active[sub];
      }
    }

    std::vector<bool> keep(entries.size(), false);
    for (size_t i = 0; i < entries.size(); i++) {
      keep[i] = MarkerOf(*defs[i]) == Marker::None && entries[i]->enabled;
    }
    for (size_t s = 1; s < sections.size(); s++) {
      if (active[s]) {
        keep[sections[s].begin] = true;
        keep[sections[s].end] = true;
      }
    }

    std::vector<u32> out;
    for (size_t i = 0; i < entries.size(); i++) {
      if (!keep[i]) {
        out.push_back(entries[i]->cheat_id);
      }
    }
    return out;
  }

  void addCheat(tsl::elm::List *list, size_t idx) {
    auto item = new tsl::elm::ListItem(NameOf(m_snap->defs[idx]));
    item->setClickListener([this, item, idx](u64 keys) {
      const auto &def = m_snap->defs[idx];
      const Status status = State().StatusOf(def);
      if (keys & KEY_A) {
        report(item, status == Status::NotLoaded ? State().Load(def, false) : State().SetEnabled(def, status == Status::Off));
        return true;
      }
      if (keys & KEY_Y) {
        report(item, status == Status::NotLoaded ? State().Load(def, false) : State().Unload(def));
        return true;
      }
      return false;
    });
    list->addItem(item);
    m_items.emplace_back(item, idx);
  }

  std::shared_ptr<LoadedSnapshot> m_snap;
  size_t m_section;
  tsl::elm::CategoryHeader *m_count_header = nullptr;
  std::vector<std::pair<tsl::elm::ListItem *, size_t>> m_items;
};

class DirGui : public ShelfGui {
 public:
  DirGui(std::string path, std::string title, bool is_root) : m_path(std::move(path)), m_title(std::move(title)), m_is_root(is_root) {}

  tsl::elm::Element *createUI() override {
    auto frame = new tsl::elm::OverlayFrame(m_title, m_is_root ? State().BuildIdHex() : "Cheat files");
    auto list = new tsl::elm::List();

    if (m_is_root) {
      const std::string title = m_title;
      AddFolderRepair(list, m_path, [title](const std::string &repaired) { tsl::swapTo<DirGui>(repaired, title, true); });
      AddHiddenFolderCleanup(list, [title] { tsl::swapTo<DirGui>(GameDir(), title, true); });

      m_loaded_item = new tsl::elm::ListItem("Loaded cheats");
      m_loaded_item->setClickListener([](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        tsl::changeTo<LoadedGui>();
        return true;
      });
      list->addItem(m_loaded_item);

      m_code_item = new tsl::elm::ListItem("Code turned on");
      list->addItem(m_code_item);

      // Its cheats can be unloaded like any other, this puts them back.
      auto atmosphere = new tsl::elm::ListItem("Atmosphere cheat file");
      atmosphere->setClickListener([atmosphere](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        auto file = std::make_shared<CheatFile>();
        if (!ParseCheatFile(AtmosphereCheatFile(), *file)) {
          atmosphere->setValue("none", true);
          return true;
        }
        tsl::changeTo<FileGui>(file, "Atmosphere cheat file", 0, true);
        return true;
      });
      list->addItem(atmosphere);

      list->addItem(new tsl::elm::CategoryHeader("Cheat files"));
    }

    std::vector<std::string> dirs, files;
    if (DIR *dir = opendir(m_path.c_str())) {
      while (dirent *ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name.empty() || name[0] == '.') {
          continue;
        }
        if (ent->d_type == DT_DIR) {
          dirs.push_back(name);
        } else if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".txt") == 0) {
          files.push_back(name);
        }
      }
      closedir(dir);
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    for (const auto &name : dirs) {
      auto item = new tsl::elm::ListItem(name);
      item->setClickListener([this, name](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        tsl::changeTo<DirGui>(m_path + name + "/", name, false);
        return true;
      });
      list->addItem(item);
    }

    for (const auto &name : files) {
      auto item = new tsl::elm::ListItem(name.substr(0, name.size() - 4));
      item->setClickListener([this, item, name](u64 keys) {
        if (!(keys & KEY_A)) {
          return false;
        }
        auto file = std::make_shared<CheatFile>();
        if (!ParseCheatFile(m_path + name, *file)) {
          item->setValue("Unreadable");
          return true;
        }
        tsl::changeTo<FileGui>(file, name.substr(0, name.size() - 4), 0);
        return true;
      });
      list->addItem(item);
    }

    if (dirs.empty() && files.empty()) {
      if (m_is_root) {
        // Shows the new, empty folder; this Gui is gone once swapTo returns.
        const std::string title = m_title;
        AddFolderSetup(list, m_path, [title](tsl::elm::ListItem *, const std::string &made) { tsl::swapTo<DirGui>(made, title, true); });
      } else {
        AddText(list, { "No .txt files in " + m_path });
      }
    }

    frame->setContent(list);
    return frame;
  }

 protected:
  void refreshValues() override {
    if (m_loaded_item != nullptr) {
      m_loaded_item->setValue(Counter(State().UserCheatCount(), MaxUserCheats));
      m_code_item->setValue(Counter(State().EnabledOpcodes(), MaxProgramOpcodes), true);
    }
  }

 private:
  std::string m_path;
  std::string m_title;
  bool m_is_root;
  tsl::elm::ListItem *m_loaded_item = nullptr;
  tsl::elm::ListItem *m_code_item = nullptr;
};

class ShelfOverlay : public tsl::Overlay {
 public:
  void initServices() override {
    m_init_rc = State().Initialize();
    if (State().KnowsGame()) {
      g_game_title = LookupGameTitle(State().TitleId());
    }
  }
  void exitServices() override { State().Exit(); }

  void onHide() override {
    State().Suspend();
    if (m_close_on_hide) {
      close();
    }
  }

  // Another tool may have changed the cheats while the overlay was hidden, or another game may run.
  void onShow() override {
    if (!State().Resume() && !m_close_on_hide) {
      m_close_on_hide = true;
      tsl::changeTo<MessageGui>("Game changed", std::vector<std::string>{ "Close CheatShelf and", "open it again." });
    }
  }

  std::unique_ptr<tsl::Gui> loadInitialGui() override {
    if (R_FAILED(m_init_rc)) {
      return initially<MessageGui>("dmnt:cht unavailable",
                                   std::vector<std::string>{ "Error " + ResultCode(m_init_rc), "Are Atmosphere cheats enabled?" });
    }
    if (!State().HasProcess()) {
      if (!State().HasApplication()) {
        return initially<MessageGui>("No game running", std::vector<std::string>{ "Start a game, then open CheatShelf again." });
      }
      // svcDebugActiveProcess fails when something else already debugs the game.
      return initially<MessageGui>("dmnt can't attach to the game",
                                   std::vector<std::string>{ "Error " + ResultCode(State().AttachResult()),
                                                             "Another debugger holds the game (SysDVR-UVC, sys-botbase...).",
                                                             "Give the game an Atmosphere cheat file so dmnt attaches at launch, then restart it." },
                                   State().KnowsGame());
    }
    return initially<DirGui>(GameDir(), "CheatShelf", true);
  }

 private:
  Result m_init_rc = 0;
  bool m_close_on_hide = false;
};

int main(int argc, char **argv) { return tsl::loop<ShelfOverlay>(argc, argv); }
