#include "artic/ls/crash.h"
#define B_STACKTRACE_IMPL
#include "b_stacktrace.h"
#include <iostream>
#include <csignal>
#include <cstdlib>

namespace artic::ls::crash {

static void crash_handler(int sig) {
    std::cerr << "\n=== CRASH DETECTED ===\n";
    std::cerr << "Signal: ";
    
    switch (sig) {
        case SIGSEGV: std::cerr << "SIGSEGV (Segmentation fault)"; break;
        case SIGABRT: std::cerr << "SIGABRT (Abort)"; break;
        case SIGFPE:  std::cerr << "SIGFPE (Floating point exception)"; break;
        case SIGILL:  std::cerr << "SIGILL (Illegal instruction)"; break;
        case SIGBUS:  std::cerr << "SIGBUS (Bus error)"; break;
        default:      std::cerr << "Unknown signal " << sig; break;
    }
    std::cerr << "\n";
    
    std::cerr << b_stacktrace_get_string() << std::flush;
    
    // Restore default handler and re-raise
    signal(sig, SIG_DFL);
    raise(sig);
}

void setup_crash_handler() {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGBUS, crash_handler);
}

} // namespace artic::ls::crash
