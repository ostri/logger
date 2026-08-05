#include "logger/logger.hpp"
#include "logger/logger_config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
// setenv/unsetenv are POSIX extensions that glibc exposes via <cstdlib> (included above)
// without a dedicated standard header of their own -- misc-include-cleaner has no
// POSIX-aware mapping for them, so its complaint on those two call sites is suppressed below.

using logger::level;
using logger::Logger;
using logger::logger_config;

namespace
{
  namespace fs = std::filesystem;

  // RAII guard for LOG_CONFIG so a test that sets it can never leak the value into
  // whichever test Catch2 happens to run next.
  class env_guard
  {
  public:
    explicit env_guard(std::string name) : name_(std::move(name))
    {
      if (const char* v = std::getenv(name_.c_str()); v != nullptr) old_value_ = v; // NOLINT(concurrency-mt-unsafe)
    }
    ~env_guard()
    {
      if (old_value_) ::setenv(name_.c_str(), old_value_->c_str(), 1); // NOLINT(concurrency-mt-unsafe, misc-include-cleaner)
      else ::unsetenv(name_.c_str());                                  // NOLINT(concurrency-mt-unsafe, misc-include-cleaner)
    }
    env_guard(const env_guard&)            = delete;
    env_guard& operator=(const env_guard&) = delete;
    env_guard(env_guard&&)                 = delete;
    env_guard& operator=(env_guard&&)      = delete;
  private:
    std::string                name_;
    std::optional<std::string> old_value_;
  };

  class temp_dir_guard
  {
  public:
    temp_dir_guard()
    : prev_(fs::current_path())
    , dir_(fs::temp_directory_path() / ("logger_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++)))
    {
      fs::create_directory(dir_);
      fs::current_path(dir_);
    }
    ~temp_dir_guard()
    {
      std::error_code ec;
      fs::current_path(prev_, ec);
      fs::remove_all(dir_, ec);
    }
    temp_dir_guard(const temp_dir_guard&)            = delete;
    temp_dir_guard& operator=(const temp_dir_guard&) = delete;
    temp_dir_guard(temp_dir_guard&&)                 = delete;
    temp_dir_guard& operator=(temp_dir_guard&&)      = delete;
    [[nodiscard]] const fs::path& dir() const { return dir_; }
  private:
    fs::path                    prev_;
    fs::path                    dir_;
    static inline std::uint32_t counter_ = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  void write_file(const fs::path& p, std::string_view content)
  {
    std::ofstream out(p, std::ios::binary);
    out << content;
  }

  logger_config console_cfg(level lvl = level::info)
  {
    return logger_config{.console_level = lvl, .file_level = lvl, .log_folder = "."};
  }

  /// spdlog::sinks::daily_file_sink inserts "_YYYY-MM-DD" before the
  /// extension (basename.log -> basename_2026-08-05.log) - a test that wants
  /// to read what it just wrote has to predict that name, not the plain
  /// app_name.log it configured.
  fs::path daily_log_path(const fs::path& dir, std::string_view app_name)
  {
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
    localtime_r(&now, &tm); // NOLINT(concurrency-mt-unsafe)
    return dir / fmt::format("{}_{:04d}-{:02d}-{:02d}.log", app_name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  }
} // namespace

// ============================================================================
// construction
// ============================================================================

TEST_CASE("Logger construction with default config does not throw", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  CHECK_NOTHROW(Logger(logger_config{.log_folder = "."}));
}

TEST_CASE("Logger reports the level it was configured with", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::error));
  CHECK(lg.level() == level::error);
}

TEST_CASE("Logger's level is the min of console and file level", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(logger_config{.console_level = level::warn, .file_level = level::trace, .log_folder = "."});
  CHECK(lg.level() == level::trace);
}

// ============================================================================
// active() - runtime pre-check before a message is formatted
// ============================================================================

TEST_CASE("active is true for a level at or above the configured threshold", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::info));
  CHECK(lg.active(level::info));
  CHECK(lg.active(level::warn));
  CHECK(lg.active(level::critical));
}

TEST_CASE("active is false for a level below the configured threshold", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::warn));
  CHECK_FALSE(lg.active(level::info));
}

TEST_CASE("set_level changes the active threshold", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  Logger                lg(console_cfg(level::info));
  lg.set_level(level::critical);
  CHECK(lg.level() == level::critical);
  CHECK_FALSE(lg.active(level::error));
}

// ============================================================================
// logging calls - none of these should throw regardless of whether the
// level is active; debug()/trace() compile out entirely in a release build
// (if constexpr on is_debug_build()), so "does not throw" here only proves
// "safe to call", not "definitely reached a sink" in that configuration.
// ============================================================================

TEST_CASE("critical/error/warn/info do not throw when active", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::trace));
  CHECK_NOTHROW(lg.critical("critical message"));
  CHECK_NOTHROW(lg.error("error message"));
  CHECK_NOTHROW(lg.warn("warn message"));
  CHECK_NOTHROW(lg.info("info message"));
  CHECK_NOTHROW(lg.debug("debug message"));
  CHECK_NOTHROW(lg.trace("trace message"));
}

TEST_CASE("critical/error/warn/info do not throw when suppressed by level", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::off));
  CHECK_NOTHROW(lg.critical("suppressed"));
  CHECK_NOTHROW(lg.error("suppressed"));
  CHECK_NOTHROW(lg.warn("suppressed"));
  CHECK_NOTHROW(lg.info("suppressed"));
}

TEST_CASE("format-string overloads do not throw for a non-empty argument list", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::trace));
  CHECK_NOTHROW(lg.info("value={} name={}", 42, "x"));
  CHECK_NOTHROW(lg.error("code={}", -1));
}

// ============================================================================
// file sink actually writes
// ============================================================================

TEST_CASE("a message at or above file_level ends up in the log file", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config         cfg{.app_name = "test_app", .console_level = level::off, .file_level = level::info, .log_folder = "."};
  {
    const Logger lg(cfg);
    lg.info("hello file sink");
    lg.flush();
  }
  const auto log_path = daily_log_path(tmp.dir(), "test_app");
  REQUIRE(fs::exists(log_path));
  std::ifstream     in(log_path);
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("hello file sink"));
}

// ============================================================================
// thread name (%* / make_log_name)
// ============================================================================

TEST_CASE("make_log_name(name) sets the thread-local log name to that name", "[Logger][positive]")
{
  Logger::make_log_name("solo-name");
  CHECK(Logger::log_name() == "solo-name");
}

TEST_CASE("make_log_name(parent, child) joins parent and non-empty child with a slash", "[Logger][positive]")
{
  Logger::make_log_name("parent", "child");
  CHECK(Logger::log_name() == "parent/child");
}

TEST_CASE("make_log_name(parent, child) with an empty child uses only the parent", "[Logger][negative]")
{
  Logger::make_log_name("only-parent", "");
  CHECK(Logger::log_name() == "only-parent");
}

TEST_CASE("the thread name reaches the file sink through the %* pattern flag", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  Logger::make_log_name("worker-7");
  logger_config cfg{.app_name      = "thread_name_app",
                     .console_level = level::off,
                     .file_level   = level::info,
                     .pattern       = "[%*] %v",
                     .log_folder   = "."};
  {
    const Logger lg(cfg);
    lg.info("marker");
    lg.flush();
  }
  std::ifstream     in(daily_log_path(tmp.dir(), "thread_name_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("[worker-7]"));
  CHECK(content.contains("marker"));
}

// ============================================================================
// exception chain logging
// ============================================================================

TEST_CASE("log_exception_with_chain does not throw for a plain exception", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::critical));
  try
  {
    throw std::runtime_error("boom");
  }
  catch (const std::exception& e)
  {
    CHECK_NOTHROW(lg.log_exception_with_chain(e));
  }
}

TEST_CASE("log_exception_with_chain follows a nested_exception chain", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config         cfg{.app_name = "chain_app", .console_level = level::off, .file_level = level::critical, .log_folder = "."};
  const Logger          lg(cfg);
  try
  {
    try
    {
      throw std::runtime_error("inner");
    }
    catch (...)
    {
      std::throw_with_nested(std::runtime_error("outer"));
    }
  }
  catch (const std::exception& e)
  {
    lg.log_exception_with_chain(e);
  }
  lg.flush();
  std::ifstream     in(daily_log_path(tmp.dir(), "chain_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("outer"));
  CHECK(content.contains("inner"));
}

TEST_CASE("log_current_exception_with_chain does not throw when called from a catch block", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const Logger          lg(console_cfg(level::critical));
  try
  {
    throw std::runtime_error("current");
  }
  catch (...)
  {
    CHECK_NOTHROW(lg.log_current_exception_with_chain());
  }
}

// ============================================================================
// mode: sync/async both build and log without throwing
// ============================================================================

TEST_CASE("an async Logger does not throw on construction or on logging", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config         cfg{.app_name = "async_app", .run_mode = logger::mode::async, .console_level = level::off, .file_level = level::info, .log_folder = "."};
  Logger                lg(cfg);
  CHECK_NOTHROW(lg.info("async message"));
  CHECK_NOTHROW(lg.flush());
}

// ============================================================================
// load_logger_config
// ============================================================================

TEST_CASE("load_logger_config reads settings from a valid LOG_CONFIG file", "[load_logger_config][positive]")
{
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "custom.json";
  write_file(cfg_path, R"({"app_name":"custom","mode":"async","console_level":"error","file_level":"debug"})");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config();
  CHECK(cfg.app_name == "custom");
  CHECK(cfg.run_mode == logger::mode::async);
  CHECK(cfg.console_level == level::error);
  CHECK(cfg.file_level == level::debug);
}

TEST_CASE("load_logger_config accepts the warning/err short/long aliases for a level", "[load_logger_config][positive]")
{
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "aliases.json";
  write_file(cfg_path, R"({"console_level":"err"})");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config();
  CHECK(cfg.console_level == level::error);
}

TEST_CASE("load_logger_config falls back to the config_path argument when LOG_CONFIG is unset", "[load_logger_config][positive]")
{
  const env_guard      guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "fallback.json";
  write_file(cfg_path, R"({"console_level":"critical"})");

  const auto cfg = logger::load_logger_config(cfg_path.string());
  CHECK(cfg.console_level == level::critical);
}

TEST_CASE("load_logger_config falls back to the hardcoded default when nothing is readable", "[load_logger_config][negative]")
{
  const env_guard      guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;

  const auto cfg = logger::load_logger_config("does/not/exist.json");
  CHECK(cfg.console_level == level::warn);
  CHECK(cfg.file_level == level::warn);
}

TEST_CASE("load_logger_config falls back to level off for an unrecognized level value", "[load_logger_config][negative]")
{
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "bad_level.json";
  write_file(cfg_path, R"({"console_level":"not_a_real_level"})");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config();
  CHECK(cfg.console_level == level::off);
}

TEST_CASE("load_logger_config falls back when LOG_CONFIG points at malformed JSON", "[load_logger_config][negative]")
{
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "broken.json";
  write_file(cfg_path, "{ this is not valid json");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config("does/not/exist.json");
  CHECK(cfg.console_level == level::warn);
}
