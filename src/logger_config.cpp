#include "logger/logger_config.hpp"
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>

namespace
{
  /**
   * @brief maps a level name (trace/debug/info/warn/error/critical/off, plus
   * the spdlog-style "warning"/"err"/"critical") to logger::level, mirroring
   * spdlog::level::from_str()'s own behavior: any unrecognized name silently
   * maps to level::off rather than being rejected.
   */
  logger::level level_from_string(const std::string& raw)
  {
    std::string v = raw;
    if (v == "warning") v = "warn";
    else if (v == "err") v = "error";

    if (v == "trace") return logger::level::trace;
    if (v == "debug") return logger::level::debug;
    if (v == "info") return logger::level::info;
    if (v == "warn") return logger::level::warn;
    if (v == "error") return logger::level::error;
    if (v == "critical") return logger::level::critical;
    return logger::level::off;
  }

  /// @brief the reverse of level_from_string() - used to render a logger_config back to JSON
  /// (see cfg_to_json_text() below), not accessible to a config *file* (an unrecognized value
  /// there silently maps to level::off, a one-way trip)
  ///
  /// Every logger::level enumerator is listed, no default: to fall through - but
  /// cfg_to_json_text() below only ever calls this with the hardcoded fallback
  /// logger_config's own console_level/file_level (both level::warn), so only that one
  /// branch is exercised in practice; the rest exist for this function to stay correct
  /// (and safe to call with any level) rather than to be hit by a test.
  const char* level_to_string(logger::level l) // GCOVR_EXCL_BR_LINE
  {
    switch (l)
    {
    case logger::level::trace: return "trace"; // GCOVR_EXCL_LINE
    case logger::level::debug: return "debug"; // GCOVR_EXCL_LINE
    case logger::level::info: return "info";   // GCOVR_EXCL_LINE
    case logger::level::warn: return "warn";
    case logger::level::error: return "error";       // GCOVR_EXCL_LINE
    case logger::level::critical: return "critical"; // GCOVR_EXCL_LINE
    case logger::level::off: return "off";           // GCOVR_EXCL_LINE
    }
    return "off"; // GCOVR_EXCL_LINE -- unreachable: logger::level has no value outside the switch above
  }

  /// @brief renders cfg as the same JSON shape a config file is read from - printed to stderr
  /// when load_logger_config() falls all the way back to a hardcoded default, so a caller has a
  /// ready-to-paste starting point for its own LOG_CONFIG/logger.conf
  std::string cfg_to_json_text(const logger::logger_config& cfg)
  {
    const nlohmann::json j{
      {"app_name", cfg.app_name},
      {"mode", cfg.run_mode == logger::mode::async ? "async" : "sync"},
      {"console_level", level_to_string(cfg.console_level)},
      {"file_level", level_to_string(cfg.file_level)},
      {"rotation_hour", cfg.rotation_hour},
      {"rotation_minute", cfg.rotation_minute},
      {"keep_days", cfg.keep_days},
      {"pattern", cfg.pattern},
      {"log_folder", cfg.log_folder},
      {"flush_on", level_to_string(cfg.flush_on)},
    };
    return j.dump(2);
  }

  /// @brief logger_config::app_name etc., overridden field-by-field from `j` - shared by
  /// logger::parse_logger_config() (already-parsed json text) and parse_config_file() below
  /// (a whole file) so the field list lives in exactly one place
  std::optional<logger::logger_config> parse_config_json(const nlohmann::json& j, const logger::logger_config& defaults)
  {
    logger::logger_config cfg = defaults;
    try
    {
      cfg.app_name        = j.value("app_name", cfg.app_name);
      cfg.run_mode        = (j.value("mode", "sync") == "async") ? logger::mode::async : logger::mode::sync;
      cfg.console_level   = level_from_string(j.value("console_level", "warn"));
      cfg.file_level      = level_from_string(j.value("file_level", "trace"));
      cfg.rotation_hour   = j.value("rotation_hour", cfg.rotation_hour);
      cfg.rotation_minute = j.value("rotation_minute", cfg.rotation_minute);
      cfg.keep_days       = j.value("keep_days", cfg.keep_days);
      cfg.pattern         = j.value("pattern", cfg.pattern);
      cfg.log_folder      = j.value("log_folder", cfg.log_folder);
      cfg.flush_on        = level_from_string(j.value("flush_on", "warn"));
    }
    // Same gcov branch-attribution quirk as parse_config_file()'s own catch
    // below - this one is exercised too (see "load_logger_config falls back
    // when a value has the wrong JSON type").
    catch (const std::exception&) // GCOVR_EXCL_BR_LINE
    {
      return std::nullopt;
    }
    return cfg;
  }

  /**
   * @brief reads and parses `path` as a logger_config
   *
   * `parse_error` receives a human-readable description of why parsing
   * failed - byte offset and message from nlohmann::json's own exception for
   * malformed JSON or a value of the wrong type, or nullopt if `path`
   * couldn't be opened at all (no file there, not readable, ...) or parsing
   * succeeded. load_logger_config() surfaces this on stderr as the
   * "syntax check" for whichever file path it was actually given - the
   * detail lives here, one level below the two-line fallback message
   * load_logger_config() itself prints when it gives up entirely.
   */
  std::optional<logger::logger_config> parse_config_file(const std::string& path, std::optional<std::string>& parse_error)
  {
    std::ifstream file(path);
    if (! file.is_open()) return std::nullopt;

    nlohmann::json j;
    try
    {
      j = nlohmann::json::parse(file);
    }
    // gcov reports this catch's own "entered via throw vs. fell through"
    // branch on the catch line itself, which --exclude-throw-branches
    // doesn't cover - the catch *is* exercised (see "load_logger_config
    // falls back when LOG_CONFIG points at malformed JSON" in
    // test_logger.cpp), this is purely how gcov attributes the branch.
    catch (const nlohmann::json::exception& e) // GCOVR_EXCL_BR_LINE
    {
      parse_error = e.what();
      return std::nullopt;
    }

    if (auto cfg = parse_config_json(j, {})) return cfg;
    parse_error = "valid JSON, but not the expected object shape (a field has the wrong type)";
    return std::nullopt;
  }
} // namespace

namespace logger
{
  logger_config load_logger_config(std::string_view config_path)
  {
    // Safe: called once at startup, before any worker thread exists.
    const char* env_path = std::getenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
    const bool  env_set  = env_path != nullptr && *env_path != '\0';

    if (env_set)
    {
      std::optional<std::string> parse_error;
      if (auto cfg = parse_config_file(env_path, parse_error)) return *cfg;
      if (parse_error)
        std::cerr << "LOG_CONFIG='" << env_path << "' does not parse as valid JSON: " << *parse_error << " -- falling back.\n";
      else std::cerr << "LOG_CONFIG='" << env_path << "' could not be read -- falling back.\n";
    }

    {
      std::optional<std::string> parse_error;
      if (auto cfg = parse_config_file(std::string(config_path), parse_error)) return *cfg;
      if (parse_error) std::cerr << "'" << config_path << "' does not parse as valid JSON: " << *parse_error << " -- falling back.\n";
    }

    const logger_config fallback{
      .console_level = level::warn,
      .file_level    = level::warn}; // GCOVR_EXCL_BR_LINE -- std::string field allocation's own bad_alloc branch, not exercised on purpose

    std::cerr << "No logger configuration found - looked for the LOG_CONFIG environment variable"
                 " and for '"
              << config_path
              << "' in the current working directory. Logging to the console only, until one of"
                 " those is available. Point LOG_CONFIG at a JSON file, or place one at '"
              << config_path << "', with content like:\n"
              << cfg_to_json_text(fallback) << "\n";

    return fallback;
  }

  logger_config parse_logger_config(std::string_view json_text, const logger_config& defaults)
  {
    nlohmann::json j;
    try
    {
      j = nlohmann::json::parse(json_text);
    }
    catch (const std::exception&) // GCOVR_EXCL_BR_LINE -- same gcov branch-attribution quirk as parse_config_file()'s own catch
    {
      return defaults;
    }

    if (auto cfg = parse_config_json(j, defaults)) return *cfg;
    return defaults;
  }
} // namespace logger
