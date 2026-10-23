#pragma once

#include <dmntcht.h>
#include <string>
#include <switch.h>
#include <vector>

namespace shelf {

  // dmnt limits, see stratosphere/dmnt/source/cheat/impl in Atmosphere.
  constexpr size_t MaxCheatCount = 0x80;              // slot 0 is the master code
  constexpr size_t MaxUserCheats = MaxCheatCount - 1;
  constexpr size_t MaxProgramOpcodes = 0x400;         // all enabled cheats together

  enum class Status { NotLoaded, Off, On };

  // Mirror of the cheats dmnt has loaded for the running game.
  class CheatState {
   public:
    Result Initialize();
    void Exit();

    // dmnt:cht has two sessions, and another tool may need one:
    // the overlay gives its own back while hidden.
    void Suspend();
    // False when the game is not the one the views were built for.
    bool Resume();

    bool HasProcess() const { return m_has_process; }
    // Why HasProcess() is false: the game is not the running application, or dmnt could not attach to it.
    bool HasApplication() const { return m_has_application; }
    // TitleId() and BuildIdHex() are valid, from dmnt or, when it holds nothing, from pm and the loader.
    bool KnowsGame() const { return m_metadata.title_id != 0; }
    Result AttachResult() const { return m_attach_rc; }
    u64 TitleId() const { return m_metadata.title_id; }
    std::string BuildIdHex() const;

    Result Refresh();

    // Bumped on every change so open views know to redraw their values.
    u64 Generation() const { return m_generation; }

    const std::vector<DmntCheatEntry> &Entries() const { return m_entries; }
    size_t UserCheatCount() const;
    size_t EnabledOpcodes() const;

    Status StatusOf(const DmntCheatDefinition &def) const;
    bool IsMaster(const DmntCheatDefinition &def) const;
    bool HasMaster() const;

    // These return a short reason to show next to the cheat, or "" on success.
    std::string Load(const DmntCheatDefinition &def, bool enabled);
    std::string Unload(const DmntCheatDefinition &def);
    std::string SetEnabled(const DmntCheatDefinition &def, bool enabled);
    std::string ToggleById(u32 cheat_id);
    std::string RemoveById(u32 cheat_id);
    std::string SetMaster(const DmntCheatDefinition &def);

   private:
    void Attach();
    void IdentifyWithoutDmnt(u64 pid);
    const DmntCheatEntry *Find(const DmntCheatDefinition &def) const;
    std::string CheckFitsEnabled(const DmntCheatDefinition &def) const;
    static std::string Error(Result rc);

    bool m_suspended = false;
    bool m_has_process = false;
    bool m_has_application = false;
    Result m_attach_rc = 0;
    DmntCheatProcessMetadata m_metadata = {};
    std::vector<DmntCheatEntry> m_entries;
    u64 m_generation = 0;
  };

  CheatState &State();

}  // namespace shelf
