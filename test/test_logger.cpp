#include "logger/logger.hpp"
#include "logger/logger_config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
#include <sys/wait.h>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
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
  /// an argument whose fmt::formatter<>::format() always throws - the only
  /// realistic way to make fmt::format() throw from inside trace()/debug()/../
  /// critical()'s formatted overloads, since fmt::format_string<Args...> is
  /// checked at compile time and so cannot itself be malformed at runtime.
  /// Exists solely to exercise those overloads' `catch (...) {}` (a
  /// throwing formatter must not escape as an exception - see logger.hpp).
  struct throwing_arg
  {
  };
} // namespace

template <>
struct fmt::formatter<throwing_arg>
{
  static constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  static auto           format(const throwing_arg& /*unused*/, format_context& /*ctx*/) -> format_context::iterator
  { throw std::runtime_error("throwing_arg always throws"); }
};

namespace
{
  namespace fs = std::filesystem;

  // RAII guard for LOG_CONFIG so a test that sets it can never leak the value into
  // whichever test Catch2 happens to run next.
  class env_guard
  {
  public:
    explicit env_guard(std::string name)
    : name_(std::move(name))
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
    temp_dir_guard(const temp_dir_guard&)                     = delete;
    temp_dir_guard& operator=(const temp_dir_guard&)          = delete;
    temp_dir_guard(temp_dir_guard&&)                          = delete;
    temp_dir_guard&               operator=(temp_dir_guard&&) = delete;
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

  /// RAII helper that redirects std::cerr into an in-memory buffer for the
  /// duration of the test - load_logger_config()'s fallback/parse-error
  /// messages go to stderr rather than through a Logger (see its own header
  /// comment for why: no Logger exists yet at that point), so this is how
  /// tests observe them.
  class cerr_capture
  {
  public:
    cerr_capture()
    : old_buf_(std::cerr.rdbuf(buf_.rdbuf()))
    {
    }
    ~cerr_capture() { std::cerr.rdbuf(old_buf_); }
    cerr_capture(const cerr_capture&)                   = delete;
    cerr_capture& operator=(const cerr_capture&)        = delete;
    cerr_capture(cerr_capture&&)                        = delete;
    cerr_capture&             operator=(cerr_capture&&) = delete;
    [[nodiscard]] std::string str() const { return buf_.str(); }
  private:
    std::ostringstream buf_;
    std::streambuf*    old_buf_;
  };

  logger_config console_cfg(level lvl = level::info) { return logger_config{.console_level = lvl, .file_level = lvl, .log_folder = "."}; }

  /// Logger::create() returns std::expected<unique_ptr<Logger>, string> -
  /// this is the "cfg is known-good, get me a Logger" shorthand every test
  /// below that isn't itself testing create()'s failure path uses; a REQUIRE
  /// failure here means the test's own setup is broken, not the behaviour
  /// under test.
  std::unique_ptr<Logger> make_logger(const logger_config& cfg)
  {
    auto lg = Logger::create(cfg);
    REQUIRE(lg.has_value());
    return std::move(*lg);
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

TEST_CASE("Logger::create succeeds for a valid config", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           result = Logger::create(logger_config{.log_folder = "."});
  CHECK(result.has_value());
}

TEST_CASE("Logger reports the level it was configured with", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::error));
  CHECK(lg->level() == level::error);
}

TEST_CASE("Logger's level is the min of console and file level", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(logger_config{.console_level = level::warn, .file_level = level::trace, .log_folder = "."});
  CHECK(lg->level() == level::trace);
}

// ============================================================================
// active() - runtime pre-check before a message is formatted
// ============================================================================

TEST_CASE("active is true for a level at or above the configured threshold", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::info));
  CHECK(lg->active(level::info));
  CHECK(lg->active(level::warn));
  CHECK(lg->active(level::critical));
}

TEST_CASE("active is false for a level below the configured threshold", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::warn));
  CHECK_FALSE(lg->active(level::info));
}

TEST_CASE("set_level changes the active threshold", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::info));
  lg->set_level(level::critical);
  CHECK(lg->level() == level::critical);
  CHECK_FALSE(lg->active(level::error));
}

// ============================================================================
// per-sink level accessors/mutators
// ============================================================================

TEST_CASE("console_level/file_level report the levels each sink was configured with", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(logger_config{.console_level = level::error, .file_level = level::debug, .log_folder = "."});
  CHECK(lg->console_level() == level::error);
  CHECK(lg->file_level() == level::debug);
}

TEST_CASE("set_console_level/set_file_level change only their own sink's level", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(logger_config{.console_level = level::warn, .file_level = level::warn, .log_folder = "."});
  lg->set_console_level(level::critical);
  lg->set_file_level(level::trace);
  CHECK(lg->console_level() == level::critical);
  CHECK(lg->file_level() == level::trace);
}

TEST_CASE("flush_on does not throw and does not prevent further logging", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::info));
  CHECK_NOTHROW(lg->flush_on(level::error));
  CHECK_NOTHROW(lg->info("still logs after flush_on changed"));
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
  const auto           lg = make_logger(console_cfg(level::trace));
  CHECK_NOTHROW(lg->critical("critical message"));
  CHECK_NOTHROW(lg->error("error message"));
  CHECK_NOTHROW(lg->warn("warn message"));
  CHECK_NOTHROW(lg->info("info message"));
  CHECK_NOTHROW(lg->debug("debug message"));
  CHECK_NOTHROW(lg->trace("trace message"));
}

TEST_CASE("critical/error/warn/info/debug/trace do not throw when suppressed by level", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::off));
  CHECK_NOTHROW(lg->critical("suppressed"));
  CHECK_NOTHROW(lg->error("suppressed"));
  CHECK_NOTHROW(lg->warn("suppressed"));
  CHECK_NOTHROW(lg->info("suppressed"));
  CHECK_NOTHROW(lg->debug("suppressed"));
  CHECK_NOTHROW(lg->trace("suppressed"));
}

TEST_CASE("format-string overloads do not throw for a non-empty argument list", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::trace));
  CHECK_NOTHROW(lg->info("value={} name={}", 42, "x"));
  CHECK_NOTHROW(lg->error("code={}", -1));
}

// ============================================================================
// file sink actually writes
// ============================================================================

TEST_CASE("a message at or above file_level ends up in the log file", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{.app_name = "test_app", .console_level = level::off, .file_level = level::info, .log_folder = "."};
  {
    const auto lg = make_logger(cfg);
    lg->info("hello file sink");
    lg->flush();
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
  logger_config cfg{
    .app_name = "thread_name_app", .console_level = level::off, .file_level = level::info, .pattern = "[%*] %v", .log_folder = "."};
  {
    const auto lg = make_logger(cfg);
    lg->info("marker");
    lg->flush();
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
  const auto           lg = make_logger(console_cfg(level::critical));
  try
  {
    throw std::runtime_error("boom");
  }
  catch (const std::exception& e)
  {
    CHECK_NOTHROW(lg->log_exception_with_chain(e));
  }
}

TEST_CASE("log_exception_with_chain follows a nested_exception chain", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{.app_name = "chain_app", .console_level = level::off, .file_level = level::critical, .log_folder = "."};
  const auto           lg = make_logger(cfg);
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
    lg->log_exception_with_chain(e);
  }
  lg->flush();
  std::ifstream     in(daily_log_path(tmp.dir(), "chain_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("outer"));
  CHECK(content.contains("inner"));
}

TEST_CASE("log_current_exception_with_chain does not throw when called from a catch block", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::critical));
  try
  {
    throw std::runtime_error("current");
  }
  catch (...)
  {
    CHECK_NOTHROW(lg->log_current_exception_with_chain());
  }
}

TEST_CASE("log_current_exception_with_chain does nothing when there is no exception in flight", "[Logger][negative]")
{
  // Covers std::current_exception() returning an empty exception_ptr - the
  // "called outside any catch block" case, distinct from the test above.
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::critical));
  CHECK_NOTHROW(lg->log_current_exception_with_chain());
}

// ============================================================================
// mode: sync/async both build and log without throwing
// ============================================================================

TEST_CASE("an async Logger does not throw on construction or on logging", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{
    .app_name = "async_app", .run_mode = logger::mode::async, .console_level = level::off, .file_level = level::info, .log_folder = "."};
  const auto lg = make_logger(cfg);
  CHECK_NOTHROW(lg->info("async message"));
  CHECK_NOTHROW(lg->flush());
}

TEST_CASE("the thread name reaches the file sink through %* on an async Logger too", "[Logger][positive]")
{
  // %* is read on spdlog's own backing thread for an async Logger, not the
  // thread that called make_log_name()/info() - this is the case
  // thread_name_formatter's own payload-splitting scheme (see
  // logger_impl.hpp) exists to cover; the sync case above already covers
  // the simpler path.
  const temp_dir_guard tmp;
  Logger::make_log_name("async-worker-3");
  logger_config cfg{.app_name      = "async_thread_name_app",
                    .run_mode      = logger::mode::async,
                    .console_level = level::off,
                    .file_level    = level::info,
                    .pattern       = "[%*] %v",
                    .log_folder    = "."};
  {
    const auto lg = make_logger(cfg);
    lg->info("async marker");
    lg->flush();
  }
  std::ifstream     in(daily_log_path(tmp.dir(), "async_thread_name_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("[async-worker-3]"));
  CHECK(content.contains("async marker"));
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
  write_file(cfg_path, R"({"console_level":"err","file_level":"warning"})");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config();
  CHECK(cfg.console_level == level::error);
  CHECK(cfg.file_level == level::warn);
}

TEST_CASE("load_logger_config accepts \"info\" as a level", "[load_logger_config][positive]")
{
  // Rounds out level_from_string()'s remaining named branch - the other
  // named levels (trace/debug/warn/error/critical/off) are already exercised
  // above/elsewhere (warn/error via the defaults and the alias test, debug
  // via the "custom" LOG_CONFIG test, trace via file_level's own default).
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "info_level.json";
  write_file(cfg_path, R"({"console_level":"info"})");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config();
  CHECK(cfg.console_level == level::info);
}

TEST_CASE("load_logger_config falls back to the config_path argument when LOG_CONFIG is unset", "[load_logger_config][positive]")
{
  const env_guard guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "fallback.json";
  write_file(cfg_path, R"({"console_level":"critical"})");

  const auto cfg = logger::load_logger_config(cfg_path.string());
  CHECK(cfg.console_level == level::critical);
}

TEST_CASE("load_logger_config falls back to the config_path argument when LOG_CONFIG is set but empty", "[load_logger_config][positive]")
{
  // Covers load_logger_config()'s *env_path != '\0' check specifically -
  // distinct from "unset" (env_path == nullptr) above: here getenv()
  // succeeds and returns a non-null pointer to an empty string.
  const env_guard guard("LOG_CONFIG");
  ::setenv("LOG_CONFIG", "", 1); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "fallback_empty_env.json";
  write_file(cfg_path, R"({"console_level":"critical"})");

  const auto cfg = logger::load_logger_config(cfg_path.string());
  CHECK(cfg.console_level == level::critical);
}

TEST_CASE("load_logger_config falls back to the hardcoded default when nothing is readable", "[load_logger_config][negative]")
{
  const env_guard guard("LOG_CONFIG");
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

TEST_CASE("load_logger_config prints the JSON parser's own error for malformed LOG_CONFIG JSON", "[load_logger_config][negative]")
{
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "broken.json";
  write_file(cfg_path, "{ this is not valid json");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const cerr_capture cap;
  (void)logger::load_logger_config("does/not/exist.json");
  const std::string out = cap.str();
  CHECK(out.find(cfg_path.string()) != std::string::npos);
  CHECK(out.find("valid JSON") != std::string::npos);
  // nlohmann::json's own exception message names the line/column it failed at.
  CHECK(out.find("line") != std::string::npos);
  CHECK(out.find("column") != std::string::npos);
}

TEST_CASE("load_logger_config falls back to logger.conf in the current working directory by default", "[load_logger_config][positive]")
{
  const env_guard guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;
  write_file(tmp.dir() / "logger.conf", R"({"console_level":"critical"})");

  const auto cfg = logger::load_logger_config(); // default config_path == def_logger_cfg_path == "logger.conf"
  CHECK(cfg.console_level == level::critical);
}

TEST_CASE("load_logger_config prints a ready-to-paste JSON example when it falls back to the hardcoded default",
          "[load_logger_config][negative]")
{
  const env_guard guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;

  const cerr_capture cap;
  const auto         cfg = logger::load_logger_config("does/not/exist.json");
  const std::string  out = cap.str();
  CHECK(cfg.console_level == level::warn);
  // Explains where it looked and that it fell back to the console...
  CHECK(out.find("LOG_CONFIG") != std::string::npos);
  CHECK(out.find("does/not/exist.json") != std::string::npos);
  CHECK(out.find("console") != std::string::npos);
  // ...and prints the fallback logger_config's own field values back out, cut&paste-ready.
  CHECK(out.find(R"("console_level": "warn")") != std::string::npos);
  CHECK(out.find(R"("file_level": "warn")") != std::string::npos);
}

TEST_CASE("load_logger_config falls back when a value has the wrong JSON type", "[load_logger_config][negative]")
{
  // console_level is read as a string (level_from_string(j.value<std::string>(...))) --
  // giving it a JSON number instead makes nlohmann::json::value() throw a type_error,
  // exercising the outer catch (logger_config.cpp's "wrong type" branch) rather than the
  // "malformed JSON" branch above (which fails earlier, at parse time).
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto           cfg_path = tmp.dir() / "wrong_type.json";
  write_file(cfg_path, R"({"console_level": 42})");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = logger::load_logger_config("does/not/exist.json");
  CHECK(cfg.console_level == level::warn);
  CHECK(cfg.file_level == level::warn);
}

TEST_CASE("load_logger_config prints a plain 'could not be read' message when LOG_CONFIG points at a missing file",
          "[load_logger_config][negative]")
{
  // Distinct from the "malformed JSON"/"wrong type" cases above: here the
  // file at LOG_CONFIG does not exist at all, so parse_config_file() never
  // gets as far as nlohmann::json - no parse_error to report, just "could
  // not be read".
  const env_guard      guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  ::setenv("LOG_CONFIG", (tmp.dir() / "does_not_exist.json").string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const cerr_capture cap;
  const auto         cfg = logger::load_logger_config("also/does/not/exist.json");
  const std::string  out = cap.str();
  CHECK(cfg.console_level == level::warn);
  CHECK(out.find("could not be read") != std::string::npos);
  CHECK(out.find("does_not_exist.json") != std::string::npos);
}

// ============================================================================
// parse_logger_config - already-in-hand JSON text, e.g. a section pulled out
// of a larger config file
// ============================================================================

TEST_CASE("parse_logger_config reads settings from valid JSON text", "[parse_logger_config][positive]")
{
  const auto cfg = logger::parse_logger_config(R"({"app_name":"custom","mode":"async","console_level":"error"})");
  CHECK(cfg.app_name == "custom");
  CHECK(cfg.run_mode == logger::mode::async);
  CHECK(cfg.console_level == level::error);
}

TEST_CASE("parse_logger_config falls back to defaults for malformed JSON text", "[parse_logger_config][negative]")
{
  const logger_config defaults{.app_name = "fallback_app"};
  const auto          cfg = logger::parse_logger_config("{ not valid json", defaults);
  CHECK(cfg.app_name == "fallback_app");
}

TEST_CASE("parse_logger_config falls back to defaults for JSON that isn't an object", "[parse_logger_config][negative]")
{
  const logger_config defaults{.app_name = "fallback_app"};
  const auto          cfg = logger::parse_logger_config("[1, 2, 3]", defaults);
  CHECK(cfg.app_name == "fallback_app");
}

TEST_CASE("parse_logger_config only overrides fields present in the JSON text, keeping the rest of defaults",
          "[parse_logger_config][positive]")
{
  const logger_config defaults{.app_name = "base_app", .console_level = level::error, .keep_days = 30};
  const auto          cfg = logger::parse_logger_config(R"({"console_level":"trace"})", defaults);
  CHECK(cfg.app_name == "base_app");        // untouched by json_text, kept from defaults
  CHECK(cfg.console_level == level::trace); // overridden by json_text
  CHECK(cfg.keep_days == 30);               // untouched by json_text, kept from defaults
}

// ============================================================================
// formatted trace()/debug()/warn()/critical() overloads with arguments -
// "format-string overloads do not throw" above only exercises info()/error(),
// this rounds out the remaining template instantiations.
// ============================================================================

TEST_CASE("formatted trace/debug/warn/critical do not throw for a non-empty argument list", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::trace));
  CHECK_NOTHROW(lg->trace("trace value={}", 1));
  CHECK_NOTHROW(lg->debug("debug value={}", 2));
  CHECK_NOTHROW(lg->warn("warn value={}", 3));
  CHECK_NOTHROW(lg->critical("critical value={}", 4));
}

TEST_CASE("trace/debug/info/warn/error/critical swallow an exception thrown while formatting", "[Logger][negative]")
{
  // Exercises the catch (...) {} in each formatted overload (logger.hpp):
  // a formatter that throws must not let the exception escape the logging
  // call - a log statement is never allowed to be the reason a caller's own
  // code fails.
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::trace));
  CHECK_NOTHROW(lg->trace("{}", throwing_arg{}));
  CHECK_NOTHROW(lg->debug("{}", throwing_arg{}));
  CHECK_NOTHROW(lg->info("{}", throwing_arg{}));
  CHECK_NOTHROW(lg->warn("{}", throwing_arg{}));
  CHECK_NOTHROW(lg->error("{}", throwing_arg{}));
  CHECK_NOTHROW(lg->critical("{}", throwing_arg{}));
}

// ============================================================================
// error<E>()/critical<E>() - any type with a to_string()
// ============================================================================

namespace
{
  struct dummy_error
  {
    std::string               reason;
    [[nodiscard]] std::string to_string() const { return "dummy_error: " + reason; }
  };
} // namespace

TEST_CASE("error(E) logs to_string() of a caller-defined error type when active", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{.app_name = "err_e_app", .console_level = level::off, .file_level = level::error, .log_folder = "."};
  const auto           lg = make_logger(cfg);
  lg->error(dummy_error{.reason = "disk full"});
  lg->flush();
  std::ifstream     in(daily_log_path(tmp.dir(), "err_e_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("dummy_error: disk full"));
}

TEST_CASE("error(E) does not log when the level is suppressed", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::off));
  CHECK_NOTHROW(lg->error(dummy_error{.reason = "suppressed"}));
}

TEST_CASE("critical(E) logs to_string() of a caller-defined error type when active", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{.app_name = "crit_e_app", .console_level = level::off, .file_level = level::critical, .log_folder = "."};
  const auto           lg = make_logger(cfg);
  lg->critical(dummy_error{.reason = "out of memory"});
  lg->flush();
  std::ifstream     in(daily_log_path(tmp.dir(), "crit_e_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("dummy_error: out of memory"));
}

TEST_CASE("critical(E) does not log when the level is suppressed", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const auto           lg = make_logger(console_cfg(level::off));
  CHECK_NOTHROW(lg->critical(dummy_error{.reason = "suppressed"}));
}

// ============================================================================
// exception chain logging - non-std::exception branches
// ============================================================================

TEST_CASE("log_current_exception_with_chain logs a fallback message for a non-std::exception", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{.app_name = "nonstd_app", .console_level = level::off, .file_level = level::critical, .log_folder = "."};
  const auto           lg = make_logger(cfg);
  try
  {
    throw 42; // NOLINT(hicpp-exception-baseclass) -- deliberately not a std::exception
  }
  catch (...)
  {
    CHECK_NOTHROW(lg->log_current_exception_with_chain());
  }
  lg->flush();
  std::ifstream     in(daily_log_path(tmp.dir(), "nonstd_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("Unknown exception (not std::exception)"));
}

TEST_CASE("log_exception_with_chain logs a fallback line for a non-std::exception nested cause", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  logger_config        cfg{.app_name = "nonstd_nested_app", .console_level = level::off, .file_level = level::critical, .log_folder = "."};
  const auto           lg = make_logger(cfg);
  try
  {
    try
    {
      throw 7; // NOLINT(hicpp-exception-baseclass) -- deliberately not a std::exception
    }
    catch (...)
    {
      std::throw_with_nested(std::runtime_error("outer"));
    }
  }
  catch (const std::exception& e)
  {
    lg->log_exception_with_chain(e);
  }
  lg->flush();
  std::ifstream     in(daily_log_path(tmp.dir(), "nonstd_nested_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("outer"));
  CHECK(content.contains("[unknown nested exception]"));
}

// ============================================================================
// Logger::create() error path - not allowed to throw, reports failure
// through std::expected instead (mkdir() failing here: no -p, so an
// intermediate path component missing - not just the leaf directory - is
// enough to trigger it)
// ============================================================================

TEST_CASE("Logger::create returns an error instead of throwing when log_folder cannot be created", "[Logger][negative]")
{
  const temp_dir_guard                                tmp;
  const logger_config                                 cfg{.log_folder = "no/such/parent/logs"};
  std::expected<std::unique_ptr<Logger>, std::string> result;
  CHECK_NOTHROW(result = Logger::create(cfg));
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().empty());
}

TEST_CASE("Logger::create succeeds when log_folder does not exist yet but can be created", "[Logger][positive]")
{
  // Covers build()'s mkdir() success path - every other test's log_folder
  // is "." (always exists, mkdir() is never even called), this is the only
  // one where log_folder is missing but its parent exists, so mkdir()
  // actually runs and succeeds.
  const temp_dir_guard tmp;
  const logger_config  cfg{.log_folder = "fresh_subdir"};
  const auto           result = Logger::create(cfg);
  CHECK(result.has_value());
  CHECK(fs::exists(tmp.dir() / "fresh_subdir"));
}

// ============================================================================
// Logger::create_or_exit() - create() plus make_log_name(), or print+exit(1)
// ============================================================================

TEST_CASE("Logger::create_or_exit succeeds for a valid config and sets the log name", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const logger_config  cfg{.app_name = "create_or_exit_app", .log_folder = "."};
  const auto           lg = Logger::create_or_exit(cfg);
  REQUIRE(lg != nullptr);
  CHECK(Logger::log_name() == cfg.app_name);
}

// ============================================================================
// signal handler / terminate handler - both ultimately call std::exit()/
// std::abort(), so they cannot run inside this Catch2 binary itself: Catch2
// installs its own fatal-condition handler for SIGABRT/SIGSEGV/etc, which
// survives fork() into the child and intercepts the signal before Logger's
// own handler would run, and a test body's exception is caught by Catch2's
// own per-test try/catch before it could reach std::terminate(). Instead,
// each of these forks and execs signal_terminate_helper (a plain, Catch2-free
// executable - see test/helper/signal_terminate_helper.cpp) and only
// observes the child's exit status/log output.
// ============================================================================

namespace
{
  /// forks and execs signal_terminate_helper with the given scenario/log_folder/app_name,
  /// waits for it, and returns its raw wait status (see waitpid(2)) for the
  /// caller to interpret with WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG.
  int run_helper(const char* scenario, const fs::path& log_folder, const char* app_name)
  {
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
      const std::string helper_path = SIGNAL_TERMINATE_HELPER_PATH;
      const std::string folder_arg  = log_folder.string();
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) -- execv's argv isn't actually mutated
      execl(helper_path.c_str(), helper_path.c_str(), scenario, folder_arg.c_str(), app_name, static_cast<char*>(nullptr));
      _exit(127); // NOLINT(concurrency-mt-unsafe) -- only reached if execl itself failed
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return status;
  }
} // namespace

TEST_CASE("setup_signal_handler logs and exits with 128+signal on SIGTERM", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const int            status = run_helper("sigterm", tmp.dir(), "sig_app");
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 128 + SIGTERM);

  std::ifstream     in(daily_log_path(tmp.dir(), "sig_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("SIGNAL SIGTERM"));
  CHECK(content.contains("BACKTRACE at signal:"));
}

TEST_CASE("a signal with no registered Logger target exits with 128+signal", "[Logger][negative]")
{
  // Covers Logger::signal_handler's self == nullptr branch: the helper
  // installs the handler, then destroys its Logger (clearing
  // signal_target_) before raising the signal.
  const temp_dir_guard tmp;
  const int            status = run_helper("sigterm_no_target", tmp.dir(), "sig_no_target_app");
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 128 + SIGTERM);
}

TEST_CASE("setup_signal_handler logs and exits with 128+signal, for every named signal", "[Logger][positive]")
{
  // Rounds out Logger::signal_name()'s named cases (SIGTERM is covered by
  // "setup_signal_handler logs and exits with 128+signal on SIGTERM" above).
  struct case_t
  {
    const char* scenario;
    int         sig;
    const char* sig_name;
  };
  const auto [scenario, sig, sig_name] = GENERATE(case_t{"sigfpe", SIGFPE, "SIGFPE"},
                                                  case_t{"sigsegv", SIGSEGV, "SIGSEGV"},
                                                  case_t{"sigabrt", SIGABRT, "SIGABRT"},
                                                  case_t{"sigill", SIGILL, "SIGILL"});
  INFO("scenario=" << scenario);

  const temp_dir_guard tmp;
  const int            status = run_helper(scenario, tmp.dir(), "sig_named_app");
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 128 + sig);

  std::ifstream     in(daily_log_path(tmp.dir(), "sig_named_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains(fmt::format("SIGNAL {}", sig_name)));
}

TEST_CASE("setup_terminate_handler logs an uncaught exception's chain and aborts", "[Logger][positive]")
{
  const temp_dir_guard tmp;
  const int            status = run_helper("terminate_with_exc", tmp.dir(), "term_app");
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);

  std::ifstream     in(daily_log_path(tmp.dir(), "term_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("EXCEPTION: uncaught"));
  CHECK(content.contains("BACKTRACE at terminate():"));
}

TEST_CASE("setup_terminate_handler logs a fixed message for an uncaught non-std::exception", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const int            status = run_helper("terminate_nonstd_exc", tmp.dir(), "term_nonstd_app");
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);

  std::ifstream     in(daily_log_path(tmp.dir(), "term_nonstd_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("UNKNOWN EXCEPTION - terminating!"));
}

TEST_CASE("setup_terminate_handler logs a fixed message when terminate is reached without an exception", "[Logger][negative]")
{
  const temp_dir_guard tmp;
  const int            status = run_helper("terminate_no_exc", tmp.dir(), "term_noexc_app");
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);

  std::ifstream     in(daily_log_path(tmp.dir(), "term_noexc_app"));
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("std::terminate() called without exception"));
}

TEST_CASE("a terminate with no registered Logger target aborts without logging", "[Logger][negative]")
{
  // Covers the installed terminate handler's self == nullptr branch: the
  // helper installs the handler, then destroys its Logger (clearing
  // signal_target_) before std::terminate() fires.
  const temp_dir_guard tmp;
  const int            status = run_helper("terminate_no_target", tmp.dir(), "term_no_target_app");
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
}

TEST_CASE("Logger::create_or_exit exits with 1 when the logger cannot be built", "[Logger][negative]")
{
  // Covers create_or_exit()'s own exit(1) path, which - like the signal/
  // terminate handlers above - ends the process and so cannot run inside
  // this Catch2 binary itself. "no/such/parent/logs" is the same unusable
  // relative path "Logger::create returns an error instead of throwing when
  // log_folder cannot be created" uses above: the child inherits this
  // process's cwd (tmp.dir(), set by temp_dir_guard) via fork(), so the path
  // resolves the same way there too. run_create_or_exit_fail() falls back to
  // exit(3) if create_or_exit() ever returned instead of exiting - WEXITSTATUS
  // == 1, not 3, is what proves the exit(1) path itself actually ran.
  const temp_dir_guard tmp;
  const int            status = run_helper("create_or_exit_fail", "no/such/parent/logs", "create_or_exit_fail_app");
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 1);
}
