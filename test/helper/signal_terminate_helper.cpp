/**
 * @file
 * @brief standalone helper executable for testing Logger::setup_signal_handler()/
 * setup_terminate_handler(), which end in std::exit()/std::abort() and so cannot
 * be exercised inside the Catch2 test binary itself - Catch2's own fatal-condition
 * handler for SIGABRT/SIGSEGV/etc. survives fork() into the child process and
 * intercepts/reports the signal itself before Logger's handler runs, and
 * Catch2's per-test invocation also wraps test bodies in its own try/catch,
 * which swallows an exception meant to reach std::terminate() before it gets
 * there.
 *
 * A plain, Catch2-free executable sidesteps both problems: logger_test forks
 * and execs this program (see test_logger.cpp's "signal handler / terminate
 * handler" section), and only observes this process's exit status/log output.
 *
 * argv[1] selects the scenario:
 *   sigterm               - setup_signal_handler(), then raise(SIGTERM)
 *   sigterm_no_target     - setup_signal_handler(), Logger destroyed, then raise(SIGTERM)
 *   sigfpe/sigsegv/sigabrt/sigill
 *                          - setup_signal_handler(), then raise() the matching signal - rounds
 *                            out signal_name()'s named cases. raise() only *delivers* the
 *                            signal, it never performs the faulting instruction/memory access a
 *                            real SIGSEGV/SIGILL would come from, so this cannot itself corrupt
 *                            process state - Logger's handler logs and calls std::exit() same as
 *                            for SIGTERM/SIGFPE.
 *   terminate_with_exc    - setup_terminate_handler(), then throw an uncaught std::exception
 *   terminate_nonstd_exc  - setup_terminate_handler(), then throw an uncaught non-std::exception
 *   terminate_no_exc      - setup_terminate_handler(), then std::terminate() directly
 *   terminate_no_target   - setup_terminate_handler(), Logger destroyed, then std::terminate() -
 *                            covers the installed handler's self == nullptr branch, same idea as
 *                            sigterm_no_target above
 *   create_or_exit_fail   - Logger::create_or_exit() with a log_folder that cannot be created
 *                            (same shape as "Logger::create returns an error instead of throwing
 *                            when log_folder cannot be created" in test_logger.cpp, but through
 *                            create_or_exit() instead of create() - covers create_or_exit()'s own
 *                            exit(1) path, which (like the signal/terminate handlers above) ends
 *                            the process and so cannot run inside the Catch2 binary itself)
 *
 * argv[2] is the log_folder to use (the parent picks a fresh temp directory
 * per test case, same as the rest of the suite).
 * argv[3] is the app_name (used to derive the log file name the parent reads back).
 */

#include "logger/logger.hpp"
#include "logger/logger_config.hpp"
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// GCC's gcov runtime normally flushes coverage counters to .gcda via a static
// destructor (priority 99, runs after atexit handlers) - which std::abort()
// skips entirely, since it terminates the process without unwinding or
// running any destructors/atexit handlers. Logger's terminate handler always
// ends in std::abort() (see logger.cpp), so without an explicit dump here,
// every line reached only inside that handler would show up as "not
// covered" purely as an artifact of how this helper's coverage data reaches
// disk - not because the code wasn't exercised. __gcov_dump() is GCC's
// documented escape hatch for exactly this case (see gcov(1), "Long-running
// applications"). Only declared/linkable when this binary itself is built
// with --coverage (LOGGER_TEST_COVERAGE_BUILD, set by CMakeLists.txt iff
// ENABLE_COVERAGE) - a plain Debug/Release build of this helper has no gcov
// runtime to call into.
#ifdef LOGGER_TEST_COVERAGE_BUILD
extern "C" void __gcov_dump(); // NOLINT(readability-identifier-naming, bugprone-reserved-identifier)
#endif

namespace
{
  using logger::level;
  using logger::Logger;
  using logger::logger_config;

  /// Logger::create() is not allowed to throw - a broken cfg is reported
  /// through std::expected instead (see logger.hpp). None of the scenarios
  /// below are testing that path, so a construction failure here is this
  /// helper's own bug, not something to report through argv/exit status the
  /// way the scenarios themselves are - abort with the message create()
  /// already logged to stderr.
  Logger* require_logger(const logger_config& cfg)
  {
    auto log = Logger::create(cfg);
    if (! log) std::abort();
    return log->release(); // deliberately leaked - must outlive the signal/terminate handler
  }

  /// installs a SIGABRT handler that dumps gcov coverage data before letting
  /// the abort proceed - only relevant for the terminate_* scenarios below,
  /// where Logger's own terminate handler ends in std::abort(). Installed
  /// ahead of Logger::setup_terminate_handler() and layered on top of it: it
  /// runs first (it's the actual SIGABRT handler), dumps, then restores and
  /// re-raises so the process still dies the same way the test expects
  /// (WIFSIGNALED/SIGABRT). A no-op outside a coverage build - there is no
  /// gcov data to dump.
  void install_gcov_dump_on_abort()
  {
#ifdef LOGGER_TEST_COVERAGE_BUILD
    struct sigaction act{};          // NOLINT(cppcoreguidelines-pro-type-member-init)
    act.sa_handler = [](int /*sig*/) // NOLINT(cppcoreguidelines-avoid-c-arrays, misc-unused-parameters)
    {
      __gcov_dump();
      std::signal(SIGABRT, SIG_DFL); // NOLINT(cert-err33-c)
      ::raise(SIGABRT);
    };
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGABRT, &act, nullptr);
#endif
  }

  [[noreturn]] void run_sigterm(const std::string& log_folder, const std::string& app_name)
  {
    logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::critical, .log_folder = log_folder};
    const Logger* lg = require_logger(cfg);
    lg->setup_signal_handler();
    ::raise(SIGTERM);
    std::exit(1); // NOLINT(concurrency-mt-unsafe) -- unreachable if the handler did its job
  }

  [[noreturn]] void run_sigterm_no_target(const std::string& log_folder, const std::string& app_name)
  {
    {
      const logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::off, .log_folder = log_folder};
      const auto          lg = Logger::create(cfg);
      if (! lg) std::abort();
      (*lg)->setup_signal_handler();
    } // Logger destroyed here -> signal_target_ reset to nullptr
    ::raise(SIGTERM);
    std::exit(1); // NOLINT(concurrency-mt-unsafe) -- unreachable if the handler did its job
  }

  /// covers Logger::signal_name()'s named cases - see the file header comment
  /// for why raise()-ing each of these is safe here.
  [[noreturn]] void run_signal(int sig, const std::string& log_folder, const std::string& app_name)
  {
    logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::critical, .log_folder = log_folder};
    const Logger* lg = require_logger(cfg);
    lg->setup_signal_handler();
    ::raise(sig);
    std::exit(1); // NOLINT(concurrency-mt-unsafe) -- unreachable if the handler did its job
  }

  [[noreturn]] void run_terminate_with_exc(const std::string& log_folder, const std::string& app_name)
  {
    logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::critical, .log_folder = log_folder};
    const Logger* lg = require_logger(cfg);
    install_gcov_dump_on_abort();
    lg->setup_terminate_handler();
    throw std::runtime_error("uncaught");
  }

  /// covers the terminate handler's catch(...) branch (an uncaught exception
  /// that is not a std::exception, so it cannot be walked/described the way
  /// log_exception_with_chain does - only a fixed "UNKNOWN EXCEPTION" message
  /// is logged).
  [[noreturn]] void run_terminate_with_nonstd_exc(const std::string& log_folder, const std::string& app_name)
  {
    logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::critical, .log_folder = log_folder};
    const Logger* lg = require_logger(cfg);
    install_gcov_dump_on_abort();
    lg->setup_terminate_handler();
    throw 99; // NOLINT(hicpp-exception-baseclass) -- deliberately not a std::exception
  }

  [[noreturn]] void run_terminate_no_exc(const std::string& log_folder, const std::string& app_name)
  {
    logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::critical, .log_folder = log_folder};
    const Logger* lg = require_logger(cfg);
    install_gcov_dump_on_abort();
    lg->setup_terminate_handler();
    std::terminate();
  }

  /// covers the installed terminate handler's self == nullptr branch: the
  /// Logger that called setup_terminate_handler() is destroyed (clearing
  /// signal_target_) before std::terminate() fires. Unlike the other
  /// terminate_* scenarios, the handler takes its self == nullptr path
  /// straight to std::abort() with nothing logged first - there is no
  /// Logger left to log through.
  [[noreturn]] void run_terminate_no_target(const std::string& log_folder, const std::string& app_name)
  {
    install_gcov_dump_on_abort();
    {
      const logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::off, .log_folder = log_folder};
      const auto          lg = Logger::create(cfg);
      if (! lg) std::abort();
      (*lg)->setup_terminate_handler();
    } // Logger destroyed here -> signal_target_ reset to nullptr
    std::terminate();
  }

  /// covers create_or_exit()'s own exit(1) path - log_folder is expected to
  /// be a path whose parent does not exist either (not just the leaf), same
  /// as the negative Logger::create() test in test_logger.cpp, so build()'s
  /// mkdir() fails without -p and create() returns an error. Nothing to log
  /// through on this path (there is no Logger yet), so the parent process
  /// only ever gets to check the exit code and stdout, not a log file.
  [[noreturn]] void run_create_or_exit_fail(const std::string& log_folder, const std::string& app_name)
  {
    const logger_config cfg{.app_name = app_name, .console_level = level::off, .file_level = level::off, .log_folder = log_folder};
    const auto          lg = Logger::create_or_exit(cfg); // never returns - log_folder is unusable
    (void)lg;
    std::exit(3); // NOLINT(concurrency-mt-unsafe) -- unreachable if create_or_exit() did its job
  }
} // namespace

int main(int argc, char** argv)
{
  if (argc != 4) return 2;

  const std::string_view scenario(argv[1]);   // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const std::string      log_folder(argv[2]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const std::string      app_name(argv[3]);   // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  if (scenario == "sigterm") run_sigterm(log_folder, app_name);
  if (scenario == "sigterm_no_target") run_sigterm_no_target(log_folder, app_name);
  if (scenario == "sigfpe") run_signal(SIGFPE, log_folder, app_name);
  if (scenario == "sigsegv") run_signal(SIGSEGV, log_folder, app_name);
  if (scenario == "sigabrt") run_signal(SIGABRT, log_folder, app_name);
  if (scenario == "sigill") run_signal(SIGILL, log_folder, app_name);
  if (scenario == "terminate_with_exc") run_terminate_with_exc(log_folder, app_name);
  if (scenario == "terminate_nonstd_exc") run_terminate_with_nonstd_exc(log_folder, app_name);
  if (scenario == "terminate_no_exc") run_terminate_no_exc(log_folder, app_name);
  if (scenario == "terminate_no_target") run_terminate_no_target(log_folder, app_name);
  if (scenario == "create_or_exit_fail") run_create_or_exit_fail(log_folder, app_name);
  return 2;
}
