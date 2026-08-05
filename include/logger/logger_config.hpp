#pragma once
/**
 * @file
 * @brief logger configuration: the plain-data struct plus how to load one
 *
 * No spdlog type appears here - logger_config is what a caller builds (by
 * hand or from JSON) and hands to logger's constructor.
 */

#include <cstdint>
#include <string>
#include <string_view>

namespace logger
{
  const int             logger_keep_days_default = 7; ///< how long rotated log files are kept by default
  constexpr const char* def_logger_cfg_path       = "config/log.debug.json";
  constexpr const char* def_logger_path           = "logs/fallback.log";

  /// @brief mnemonic level names, higher value == more severe
  enum class level : std::uint8_t
  {
    trace    = 0,
    debug    = 1,
    info     = 2,
    warn     = 3,
    error    = 4,
    critical = 5,
    off      = 6,
  };

  /// @brief sync: caller's thread writes to the sinks directly. async: a
  /// background thread pool does, via spdlog's async_logger - use it when
  /// the calling thread cannot afford sink latency (a slow disk, a full
  /// terminal), at the cost of log order across threads being best-effort.
  enum class mode : std::uint8_t
  {
    sync,
    async
  };

  /// @brief every knob Logger's constructor/the JSON config file exposes
  struct logger_config
  {
    std::string app_name        = "app";
    mode        run_mode        = mode::sync;
    level       console_level   = level::warn;
    level       file_level      = level::trace;
    int         rotation_hour   = 2;
    int         rotation_minute = 0;
    int         keep_days       = logger_keep_days_default;
    std::string pattern         = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%*] %v";
    std::string log_folder      = "./logs";
    level       flush_on        = level::warn;
  };

  /**
   * @brief loads a logger_config, trying in order:
   * 1. LOG_CONFIG environment variable -> path to a JSON config file, if set and non-empty.
   * 2. `config_path` (defaults to def_logger_cfg_path), if LOG_CONFIG was unset, empty, or
   *    pointed at a file that couldn't be read/parsed.
   * 3. a hardcoded fallback if neither file could be read: console only, level warn.
   *
   * Never throws - a missing or broken config file is not a reason to fail
   * startup, just a reason to fall back.
   */
  [[nodiscard]] logger_config load_logger_config(std::string_view config_path = def_logger_cfg_path);
} // namespace logger
