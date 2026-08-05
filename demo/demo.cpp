/**
 * @file
 * @brief small program demonstrating the main properties of logger::Logger
 *
 * Run it from the build directory - it looks for its config files relative
 * to the current working directory (see README.md "running the demo").
 *
 *   ./logger_demo                    # loads config/log.debug.json (or LOG_CONFIG)
 *   LOG_CONFIG=/path/to/other.json ./logger_demo
 */
#include "error_info.hpp"
#include "logger/logger.hpp"
#include "logger/logger_config.hpp"
#include <chrono>
#include <thread>

namespace
{
  /// @brief property 1: logging is configured from a file, not hardcoded
  ///
  /// load_logger_config() tries the LOG_CONFIG environment variable first,
  /// then the path given here, then falls back to a hardcoded default if
  /// neither can be read - see README.md "configuration file" for the full
  /// list of settings and what each one means.
  void show_configurable_logging()
  {
    fmt::print("\n--- 1. configuring logging from a file ---\n");
    const auto cfg = logger::load_logger_config("config/log.debug.json");
    fmt::print(
      "loaded config: app_name={}, console_level={}, file_level={}\n", cfg.app_name, static_cast<int>(cfg.console_level), static_cast<int>(cfg.file_level));

    const logger::Logger log(cfg);
    log.info("Logger constructed from a config file - this line goes to both console and file.");
  }

  /// @brief property 2: debug()/trace() compile out entirely in a release build
  ///
  /// This is not a runtime filter - `if constexpr (is_debug_build())` means
  /// the call site's formatting work is not present in the compiled code at
  /// all when NDEBUG is defined. Build this demo both ways to see the
  /// difference: `nm` the debug and release binaries and grep for the
  /// string below - it is only present in the debug one.
  void show_debug_trace_elimination()
  {
    fmt::print("\n--- 2. debug()/trace() elimination in a release build ---\n");
    const logger::Logger log(logger::logger_config{
      .app_name = "demo_trace", .console_level = logger::level::trace, .file_level = logger::level::trace, .log_folder = "./logs"});

    log.debug("this debug message and the work to format it do not exist in a release build");
    log.trace("neither does this trace message");
    fmt::print("(check logs/demo_trace_*.log: these two lines are present in a debug build, absent in release)\n");
  }

  /// @brief property 3: active() lets a caller skip expensive work up front
  ///
  /// A log call that ends up suppressed by level still costs nothing extra
  /// (active() is checked internally before formatting) - active() is for
  /// the case where building the *arguments* themselves is the expensive
  /// part, not just formatting them.
  std::string expensive_diagnostic()
  {
    fmt::print("  (expensive_diagnostic() called - this should only happen when trace is active)\n");
    return "computed diagnostic payload";
  }

  void show_active_check()
  {
    fmt::print("\n--- 3. active() avoids paying for work that would be thrown away ---\n");
    // active() reflects the *lower* of console_level/file_level (Logger's
    // effective level is whichever sink asked for the most detail) - both
    // are set explicitly here so "quiet" really is quiet on every sink, not
    // accidentally verbose through file_level's own default.
    const logger::Logger quiet(
      logger::logger_config{.app_name = "demo_quiet", .console_level = logger::level::warn, .file_level = logger::level::warn, .log_folder = "./logs"});
    const logger::Logger verbose(
      logger::logger_config{.app_name = "demo_verbose", .console_level = logger::level::trace, .file_level = logger::level::trace, .log_folder = "./logs"});

    fmt::print("quiet logger, trace active: {}\n", quiet.active(logger::level::trace));
    if (quiet.active(logger::level::trace)) quiet.trace("{}", expensive_diagnostic());
    else fmt::print("  (skipped: expensive_diagnostic() was not called)\n");

    fmt::print("verbose logger, trace active: {}\n", verbose.active(logger::level::trace));
    if (verbose.active(logger::level::trace)) verbose.trace("{}", expensive_diagnostic());
  }

  /// @brief property 4: sync vs async logging
  ///
  /// sync: the calling thread writes to the sinks itself - a call returns
  /// only once the message has actually reached them. async: a background
  /// thread pool does the writing - the call returns immediately, at the
  /// cost of log order across threads being best-effort. See
  /// logger_config::run_mode / README.md "mode" for when to pick which.
  void show_sync_vs_async()
  {
    fmt::print("\n--- 4. sync vs async logging ---\n");

    const logger::Logger sync_log(
      logger::logger_config{.app_name = "demo_sync", .run_mode = logger::mode::sync, .console_level = logger::level::info, .log_folder = "./logs"});
    sync_log.info("sync: this line has already reached its sinks by the time info() returns");

    const logger::Logger async_log(
      logger::logger_config{.app_name = "demo_async", .run_mode = logger::mode::async, .console_level = logger::level::info, .log_folder = "./logs"});
    async_log.info("async: this line was handed to a background thread pool");
    async_log.flush(); // wait for the background thread to catch up before the demo exits
  }

  /// @brief bonus: a caller's own structured error type, logged through the
  /// generic Logger::error<E>()/critical<E>() template overloads
  ///
  /// demo::error_info (see error_info.hpp) is not part of the logger
  /// library - any type with a to_string() works the same way.
  void show_structured_error_logging()
  {
    fmt::print("\n--- bonus: logging a caller-defined error type ---\n");
    const logger::Logger log(
      logger::logger_config{.app_name = "demo_error", .console_level = logger::level::error, .file_level = logger::level::error, .log_folder = "./logs"});

    const demo::error_info err(demo::error_code::parse_failed, "unexpected token", "input.yaml", 42);
    log.error(err);
    log.critical(err);
  }
} // namespace

int main()
{
  fmt::print("logger demo - see README.md for how each property below is activated\n");

  show_configurable_logging();
  show_debug_trace_elimination();
  show_active_check();
  show_sync_vs_async();
  show_structured_error_logging();

  fmt::print("\ndone - see ./logs/ for the files these loggers wrote to\n");
  return 0;
}
