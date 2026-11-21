#pragma once

#include "addresses.h"
#include "game.h"
#include "log.h"
#include <cstdint>

namespace referencebirth {
// Final listener for mod-owned reference creation. The owning subsystem has already admitted the request.
// Forward exactly the supplied creation data on the caller's existing thread, inside its existing fault boundary.
inline void Commit(const char* source, std::uintptr_t dataHandler, std::uint64_t& handle, void* creationData) {
    logger::Write("OBS system=reference.birth event=commit source=%s", source);
    reinterpret_cast<void* (*)(std::uintptr_t, std::uint64_t*, void*)>(game::Base() + addr::kCreateReference)(
        dataHandler, &handle, creationData);
    logger::Write("OBS system=reference.birth event=returned source=%s handle=%08X", source, unsigned(handle));
}
}
