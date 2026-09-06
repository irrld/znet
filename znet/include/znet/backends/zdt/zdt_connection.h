//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: internal (see the wiki, API Stability)

#ifndef ZNET_BACKENDS_ZDT_ZDT_CONNECTION_H_
#define ZNET_BACKENDS_ZDT_ZDT_CONNECTION_H_

#include <cstdint>

namespace znet {
namespace backends {

// per-connection parameters settled by the handshake. In its own header so
// holders of one (p2p::Agent) need none of the ZDT protocol surface.
struct ZDTConnection {
  uint16_t mtu = 1200;
  uint64_t local_guid = 0;
  uint64_t remote_guid = 0;
  // negotiated in the handshake: connection migration runs only when both ends
  // offered it (kZDTCapMigration). Gates the cid stamped on every datagram.
  bool migration_enabled = false;
  // nonzero on a connection that runs through a p2p::Relay: every
  // datagram is prefixed with the relay channel header carrying it, and the
  // MTU budget shrinks by that header
  uint32_t relay_channel = 0;
};

}  // namespace backends
}  // namespace znet

#endif  // ZNET_BACKENDS_ZDT_ZDT_CONNECTION_H_
