//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <any>
#include <iostream>
#include <sstream>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <cassert>

#ifdef __APPLE__
#define TARGET_APPLE
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define ZNET_NODISCARD [[nodiscard]]
