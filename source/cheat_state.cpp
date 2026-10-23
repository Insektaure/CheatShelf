#include "cheat_state.hpp"

#include <cstdio>
#include <cstring>

namespace shelf {

  namespace {

    // dmnt keeps up to 0x40 name bytes, our parser 0x3F (it keeps the terminator):
    // a long name loaded by Atmosphere must still match the same cheat read by CheatShelf.
    bool SameDefinition(const DmntCheatDefinition &a, const DmntCheatDefinition &b) {
      return a.num_opcodes == b.num_opcodes &&
             strncmp(a.readable_name, b.readable_name, sizeof(a.readable_name) - 1) == 0 &&
             memcmp(a.opcodes, b.opcodes, a.num_opcodes * sizeof(a.opcodes[0])) == 0;
    }

  }  // namespace

  CheatState &State() {
    static CheatState state;
    return state;
  }

  Result CheatState::Initialize() {
    Result rc = dmntchtInitialize();
    if (R_FAILED(rc)) {
      return rc;
    }
    m_entries.reserve(MaxCheatCount);
    Attach();
    return 0;
  }

  void CheatState::Attach() {
    u64 pid = 0;
    m_has_application = R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid));

    // Attaches dmnt to the running game if nothing has done so yet.
    m_attach_rc = dmntchtForceOpenCheatProcess();
    m_has_process = false;
    dmntchtHasCheatProcess(&m_has_process);
    if (m_has_process && R_FAILED(dmntchtGetCheatProcessMetadata(&m_metadata))) {
      m_has_process = false;
    }
    if (m_has_process) {
      Refresh();
    } else {
      IdentifyWithoutDmnt(pid);
    }
  }

  // When another debugger holds the game, dmnt knows nothing about it, but pm and the loader still
  // tell which game and build it is, enough to find or create its folder.
  void CheatState::IdentifyWithoutDmnt(u64 pid) {
    m_metadata = {};
    m_entries.clear();
    if (!m_has_application) {
      return;
    }
    m_metadata.process_id = pid;
    if (R_FAILED(pmdmntGetProgramId(&m_metadata.title_id, pid))) {
      m_metadata.title_id = 0;
      return;
    }

    // Applications have rtld then main: the build id is main's, as dmnt reads it.
    LoaderModuleInfo modules[2];
    s32 count = 0;
    if (R_SUCCEEDED(ldrDmntInitialize())) {
      if (R_FAILED(ldrDmntGetProcessModuleInfo(pid, modules, 2, &count))) {
        count = 0;
      }
      ldrDmntExit();
    }
    if (count < 1) {
      m_metadata.title_id = 0;
      return;
    }
    memcpy(m_metadata.main_nso_build_id, modules[count == 2 ? 1 : 0].build_id, sizeof(m_metadata.main_nso_build_id));
  }

  void CheatState::Exit() { dmntchtExit(); }

  void CheatState::Suspend() {
    if (!m_suspended) {
      dmntchtExit();
      m_suspended = true;
    }
  }

  bool CheatState::Resume() {
    if (!m_suspended) {
      return true;
    }
    m_suspended = false;

    const bool had_process = m_has_process;
    const DmntCheatProcessMetadata before = m_metadata;

    // libtesla only keeps sm open while the overlay starts, and opening a service needs it.
    smInitialize();
    const Result rc = dmntchtInitialize();
    if (R_SUCCEEDED(rc)) {
      Attach();
    } else {
      m_has_process = false;
    }
    smExit();
    if (R_FAILED(rc)) {
      return !had_process;
    }

    if (had_process != m_has_process) {
      return false;
    }
    return !m_has_process || (before.process_id == m_metadata.process_id &&
                              memcmp(before.main_nso_build_id, m_metadata.main_nso_build_id, 8) == 0);
  }

  std::string CheatState::BuildIdHex() const {
    char buf[17];
    for (size_t i = 0; i < 8; i++) {
      snprintf(buf + i * 2, 3, "%02X", m_metadata.main_nso_build_id[i]);
    }
    return buf;
  }

  Result CheatState::Refresh() {
    m_entries.clear();
    m_generation++;

    u64 count = 0;
    Result rc = dmntchtGetCheatCount(&count);
    if (R_FAILED(rc) || count == 0) {
      return rc;
    }

    m_entries.resize(count);
    u64 read = 0;
    rc = dmntchtGetCheats(m_entries.data(), count, 0, &read);
    m_entries.resize(R_SUCCEEDED(rc) ? read : 0);
    return rc;
  }

  size_t CheatState::UserCheatCount() const {
    size_t n = 0;
    for (const auto &e : m_entries) {
      n += e.cheat_id != 0;
    }
    return n;
  }

  size_t CheatState::EnabledOpcodes() const {
    size_t n = 0;
    for (const auto &e : m_entries) {
      if (e.enabled) {
        n += e.definition.num_opcodes;
      }
    }
    return n;
  }

  const DmntCheatEntry *CheatState::Find(const DmntCheatDefinition &def) const {
    for (const auto &e : m_entries) {
      if (e.cheat_id != 0 && SameDefinition(e.definition, def)) {
        return &e;
      }
    }
    return nullptr;
  }

  Status CheatState::StatusOf(const DmntCheatDefinition &def) const {
    const DmntCheatEntry *e = Find(def);
    if (e == nullptr) {
      return Status::NotLoaded;
    }
    return e->enabled ? Status::On : Status::Off;
  }

  bool CheatState::IsMaster(const DmntCheatDefinition &def) const {
    for (const auto &e : m_entries) {
      if (e.cheat_id == 0) {
        return SameDefinition(e.definition, def);
      }
    }
    return false;
  }

  bool CheatState::HasMaster() const {
    for (const auto &e : m_entries) {
      if (e.cheat_id == 0) {
        return true;
      }
    }
    return false;
  }

  // dmnt drops the whole program, without any error, when the enabled cheats go over the limit.
  // Refusing here is the only way the user gets told.
  std::string CheatState::CheckFitsEnabled(const DmntCheatDefinition &def) const {
    if (EnabledOpcodes() + def.num_opcodes > MaxProgramOpcodes) {
      return "Too big";
    }
    return "";
  }

  std::string CheatState::Error(Result rc) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Err %X", rc);
    return buf;
  }

  std::string CheatState::Load(const DmntCheatDefinition &def, bool enabled) {
    if (Find(def) != nullptr) {
      return enabled ? SetEnabled(def, true) : "";
    }
    if (UserCheatCount() >= MaxUserCheats) {
      return "Full";
    }
    if (enabled) {
      if (auto err = CheckFitsEnabled(def); !err.empty()) {
        return err;
      }
    }

    // dmnt takes a non-const pointer but only copies it.
    DmntCheatDefinition copy = def;
    u32 id = 0;
    Result rc = dmntchtAddCheat(&copy, enabled, &id);
    Refresh();
    return R_SUCCEEDED(rc) ? "" : Error(rc);
  }

  std::string CheatState::Unload(const DmntCheatDefinition &def) {
    const DmntCheatEntry *e = Find(def);
    return e == nullptr ? "" : RemoveById(e->cheat_id);
  }

  std::string CheatState::SetEnabled(const DmntCheatDefinition &def, bool enabled) {
    const DmntCheatEntry *e = Find(def);
    if (e == nullptr) {
      return enabled ? Load(def, true) : "";
    }
    if (e->enabled == enabled) {
      return "";
    }
    return ToggleById(e->cheat_id);
  }

  std::string CheatState::ToggleById(u32 cheat_id) {
    for (const auto &e : m_entries) {
      if (e.cheat_id == cheat_id && !e.enabled) {
        if (auto err = CheckFitsEnabled(e.definition); !err.empty()) {
          return err;
        }
      }
    }

    Result rc = dmntchtToggleCheat(cheat_id);
    Refresh();
    return R_SUCCEEDED(rc) ? "" : Error(rc);
  }

  std::string CheatState::RemoveById(u32 cheat_id) {
    // Slot 0 is the master code, removing it would break every cheat that relies on it.
    if (cheat_id == 0) {
      return "Master";
    }
    Result rc = dmntchtRemoveCheat(cheat_id);
    Refresh();
    return R_SUCCEEDED(rc) ? "" : Error(rc);
  }

  std::string CheatState::SetMaster(const DmntCheatDefinition &def) {
    size_t current_master = 0;
    for (const auto &e : m_entries) {
      if (e.cheat_id == 0) {
        current_master = e.definition.num_opcodes;
      }
    }
    // The master code is always enabled, and replaces the previous one.
    if (EnabledOpcodes() - current_master + def.num_opcodes > MaxProgramOpcodes) {
      return "Too big";
    }

    DmntCheatDefinition copy = def;
    Result rc = dmntchtSetMasterCheat(&copy);
    Refresh();
    return R_SUCCEEDED(rc) ? "" : Error(rc);
  }

}  // namespace shelf
