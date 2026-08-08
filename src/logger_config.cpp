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

  std::optional<logger::logger_config> parse_config_file(const std::string& path)
  {
    std::ifstream file(path);
    if (! file.is_open()) return std::nullopt;

    nlohmann::json j;
    try
    {
      file >> j;
    }
    // gcov reports this catch's own "entered via throw vs. fell through"
    // branch on the catch line itself, which --exclude-throw-branches
    // doesn't cover - the catch *is* exercised (see "load_logger_config
    // falls back when LOG_CONFIG points at malformed JSON" in
    // test_logger.cpp), this is purely how gcov attributes the branch.
    catch (const std::exception&) // GCOVR_EXCL_BR_LINE
    {
      return std::nullopt;
    }

    return parse_config_json(j, {});
  }
} // namespace

namespace logger
{
  logger_config load_logger_config(std::string_view config_path)
  {
    // Safe: called once at startup, before any worker thread exists.
    if (const char* env_path = std::getenv("LOG_CONFIG"); env_path != nullptr && *env_path != '\0') // NOLINT(concurrency-mt-unsafe)
    {
      if (auto cfg = parse_config_file(env_path)) return *cfg;
      std::cerr << "LOG_CONFIG='" << env_path << "' could not be read or parsed -- falling back.\n";
    }

    if (auto cfg = parse_config_file(std::string(config_path))) return *cfg;

    return logger_config{
      .console_level = level::warn,
      .file_level    = level::warn}; // GCOVR_EXCL_BR_LINE -- std::string field allocation's own bad_alloc branch, not exercised on purpose
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
