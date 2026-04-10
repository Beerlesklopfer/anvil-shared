// Copyright 2025-2026 Blacksmith (blacksmith@forgeiec.io)
// Part of the Anvil IPC transport layer for ForgeIEC / OpenPLC v3.
// Licensed under the GNU General Public License v3.

#ifndef I_ANVIL_DATA_SINK_H
#define I_ANVIL_DATA_SINK_H

#include <cstdint>

namespace anvil_wrapper {

template <typename Ht>
class i_anvil_data_sink {
public:
    virtual ~i_anvil_data_sink() = default;

    virtual void OnDataReceived(Ht* header, const void* data, uint32_t size) = 0;
    virtual void OnDataReady() = 0;
};

}  // namespace anvil_wrapper

#endif  // I_ANVIL_DATA_SINK_H
