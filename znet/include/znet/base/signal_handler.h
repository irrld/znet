
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

#include <functional>

namespace znet {

enum Signal {
  kSignalInterrupt = SIGINT,
  kSignalIllegalInstruction = SIGILL,
#ifdef TARGET_APPLE
  kSignalHangup = SIGHUP,
  kSignalQuit = SIGQUIT,
  kSignalTrap = SIGTRAP,
#endif
  kSignalAbort = SIGABRT,
#if  (defined(_POSIX_C_SOURCE) && !defined(_DARWIN_C_SOURCE))
  kSignalPoll = SIGPOLL,
#elif !defined(TARGET_WIN)   /* (!_POSIX_C_SOURCE || _DARWIN_C_SOURCE) */
  kSignalEMT = SIGEMT,
#endif  /* (!_POSIX_C_SOURCE || _DARWIN_C_SOURCE) */
  kSignalFPE = SIGFPE,
  kSignalSegFault = SIGSEGV,
  kSignalTermination = SIGTERM,
#ifndef TARGET_WIN
  kSignalKill = SIGKILL,
  kSignalBus = SIGBUS,
  kSignalSys = SIGSYS,
  kSignalPipe = SIGPIPE,
  kSignalAlarm = SIGALRM,
  kSignalUrgent = SIGURG,
  kSignalSendableStop = SIGSTOP,
  kSignalStop = SIGTSTP,
  kSignalContinue = SIGCONT,
  kSignalChild = SIGCHLD,
  kSignalTTIN = SIGTTIN,
  kSignalTTOU = SIGTTOU,
#if  (!defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE))
  kSignalIO = SIGIO,
#endif
  kSignalXCPU = SIGXCPU,
  kSignalXFSZ = SIGXFSZ,
  kSignalVirtualAlarm = SIGVTALRM,
  kSignalProfilingAlarm = SIGPROF,
#if  (!defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE))
  kSignalWindowSize = SIGWINCH,
  kSignalInfo = SIGINFO,
#endif
  kSignalUser1 = SIGUSR1,
  kSignalUser2 = SIGUSR2
#endif
};

typedef std::function<bool(Signal)>(SignalHandlerFn);

void RegisterSignalHandler(SignalHandlerFn fn);

}