//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/rendezvous_server.h"

#include "cxxopts.h"

int main(int argc, char* argv[]) {
  cxxopts::Options opts(
      "rendezvous-server",
      "rendezvous-server brokers peer-to-peer punches for znet, and can run "
      "the relay peers fall back to");
  opts.add_options()
      ("p,port", "Port to listen on",
       cxxopts::value<uint16_t>()->default_value("5001"))
      ("t,target", "Host to listen on",
       cxxopts::value<std::string>()->default_value("0.0.0.0"))
      ("c,conn", "Punch connection type: tcp or zdt",
       cxxopts::value<std::string>()->default_value("zdt"))
      ("r,relay", "Run a relay alongside the rendezvous")
      ("relay-host",
       "Host peers reach the relay at; empty means the rendezvous host",
       cxxopts::value<std::string>()->default_value(""))
      ("relay-port", "The UDP port the relay and its reflector answer on",
       cxxopts::value<uint16_t>()->default_value("5002"))
      ("h,help", "Print usage");

  auto result = opts.parse(argc, argv);
  if (result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  std::string conn = result["conn"].as<std::string>();
  znet::p2p::RendezvousServerConfig config;
  config.bind_port = result["port"].as<uint16_t>();
  config.bind_address = result["target"].as<std::string>();
  config.punch_connection_type = conn == "tcp" ? znet::ConnectionType::TCP
                                               : znet::ConnectionType::ZDT;
  config.relay_enabled = result["relay"].as<bool>();
  config.relay_host = result["relay-host"].as<std::string>();
  config.relay.bind_address = config.bind_address;
  config.relay.port = result["relay-port"].as<uint16_t>();
  ZNET_LOG_INFO("Starting rendezvous on {}:{}... (punch type: {}, relay: {})",
                config.bind_address, config.bind_port,
                conn == "tcp" ? "tcp" : "zdt",
                config.relay_enabled ? "on" : "off");

  znet::p2p::RendezvousServer rendezvous{config};
  if (rendezvous.Start() != znet::Result::Success) {
    return 1;
  }
  rendezvous.Wait();
  return 0;
}
