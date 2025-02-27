/*
 * Copyright (c) 2022 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_packet/packet_factory.h"
#include "roc_packet/packet.h"

namespace roc {
namespace packet {

PacketFactory::PacketFactory(core::IAllocator& allocator, bool poison)
    : pool_(allocator, sizeof(Packet), poison) {
}

core::SharedPtr<Packet> PacketFactory::new_packet() {
    return new (pool_) Packet(*this);
}

void PacketFactory::destroy(Packet& packet) {
    pool_.destroy_object(packet);
}

} // namespace packet
} // namespace roc
