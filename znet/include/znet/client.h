//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// API stability: stable (see the wiki, API Stability)

#ifndef ZNET_CLIENT_H_
#define ZNET_CLIENT_H_

#include "znet/compat.h"
#include "znet/interface.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/scheduler.h"
#include "znet/session_encoder.h"
#include "znet/task.h"
#include "znet/worker_signal.h"

namespace znet {

namespace backends {
class ClientBackend;
}  // namespace backends

/**
 * @brief Everything a Client is constructed from.
 *
 * The aggregate is meant for brace-init:
 * `ClientConfig{"1.2.3.4", 25000, std::chrono::seconds(10)}`.
 */
struct ClientConfig {
  /** @brief Server host: an IP, a hostname, or "unix:/path" with TCP. */
  std::string server_address;
  PortNumber server_port = 0;
  /** @brief Give up on a connect that is not ready after this long. Zero
   * waits forever. */
  std::chrono::steady_clock::duration connection_timeout{
      std::chrono::seconds(10)};
  ConnectionType connection_type = ConnectionType::ZDT;
  /** @brief This client's session options; see options.h. */
  SessionOptions options;
};

/**
 * @brief Connects to one server and drives that session on a thread of its
 *        own, reporting through the event callback.
 */
class Client : public Interface {
 public:
  explicit Client(const ClientConfig& config);
  Client(const Client&) = delete;
  ~Client() override;

  /** @brief Binds a local socket for the connection. Not thread-safe.
   *         InvalidAddress, CannotCreateSocket or CannotBind say what failed. */
  Result Bind() override;

  /**
   * @brief Same, but to an explicit local address: a specific interface, or a
   *        fixed source port. Same Result values as Bind().
   */
  Result Bind(const std::string& ip, PortNumber port);

  /**
   * @brief Sets how often the client's loop services the session.
   *
   * The loop sleeps out the rest of each tick, but a datagram arriving or a
   * Send() on an idle session cuts that short, so raising this trades CPU for
   * responsiveness only where neither of those applies.
   *
   * @param tps Ticks per second, clamped to at least 1.
   */
  void SetTicksPerSecond(uint16_t tps) { scheduler_.SetTicksPerSecond(tps); }

  /** @brief Starts connecting on a thread of its own and returns at once;
   *         the outcome arrives as an event. Not thread-safe.
   *         AlreadyConnected or InvalidRemoteAddress say why it did not start. */
  Result Connect();

  /** @brief Closes the session. Not thread-safe. Failure when there is none. */
  Result Disconnect(CloseOptions options = {});

  /** @brief Blocks until the client's thread has finished, which is once the
   *         session is gone. Thread-safe. */
  void Wait() override;

  /**
   * @brief This client's one session, or null before Connect(). Most code
   *        takes it from ClientConnectedToServerEvent instead, which also
   *        marks the moment it is ready.
   */
  ZNET_NODISCARD std::shared_ptr<PeerSession> client_session() const {
    return client_session_;
  }

  /**
   * @brief Drops the client's reference to a session that already ended.
   *
   * The transport closes its descriptor when the last holder lets go, and
   * that is what frees the local port: a hole punch has to bind the very
   * port the relay observed, which a merely shut-down socket still owns.
   * A session that is still alive is left alone.
   */
  void ReleaseSession();

  ZNET_NODISCARD std::shared_ptr<InetAddress> server_address() const {
    return server_address_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const;

  /**
   * @brief Moves the live connection to a new local address without dropping
   *        the session, so a client that changed network or port keeps talking.
   *
   * Only connection-oriented backends that survive an address change support
   * it (ZDT with enable_connection_migration on both ends); others return
   * InvalidBackend. Same local-address Result values as Bind() otherwise.
   */
  Result Rebind(const std::string& ip, PortNumber port);

 private:
  ClientConfig config_;
  std::shared_ptr<InetAddress> server_address_;
  std::unique_ptr<backends::ClientBackend> backend_;
  std::shared_ptr<PeerSession> client_session_;

  Task task_;
  Scheduler scheduler_{120};
  std::shared_ptr<WorkerSignal> signal_{std::make_shared<WorkerSignal>()};
  // a client has one session and one loop, so without this the loop would
  // serialize encoding behind putting bytes on the wire
  SessionEncoder encoder_;
};

}  // namespace znet

#endif  // ZNET_CLIENT_H_
