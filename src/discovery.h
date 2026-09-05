#pragma once

#include <cstdint>

namespace kmmo {

struct GameOffsets {
    uintptr_t base = 0;

    uintptr_t CharacterSpawn = 0;
    uintptr_t CharacterDestroy = 0;
    uintptr_t CreateRandomSquad = 0;
    uintptr_t CharacterSerialise = 0;
    uintptr_t ApplyDamage = 0;
    uintptr_t StartAttack = 0;
    uintptr_t GameFrameUpdate = 0;
    uintptr_t TimeUpdate = 0;
    uintptr_t SaveGame = 0;
    uintptr_t LoadGame = 0;
    uintptr_t ZoneLoad = 0;
    uintptr_t SquadCreate = 0;
    uintptr_t SquadAddMember = 0;

    uintptr_t PlayerBase = 0;      // address of the .data global (pointer to object)
    uintptr_t GameWorldSingleton = 0;
};

class Discovery {
public:
    static Discovery& Get();

    void Start();   // spawns the background polling thread
    void Stop();

    const GameOffsets& Result() const { return m_offsets; }
    bool Ready() const { return m_ready; }

private:
    void Poll();    // repeatedly attempts discovery until PlayerBase validates

    GameOffsets m_offsets;
    volatile bool m_ready = false;
    volatile bool m_running = false;
    void* m_thread = nullptr;
};

} // namespace kmmo