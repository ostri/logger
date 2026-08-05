#include "logger_impl.hpp"
#include <spdlog/async.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <filesystem>
#include <sstream>
#include <stacktrace>
#include <unordered_map>

namespace fs = std::filesystem;

namespace logger
{
  void thread_name_formatter::format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest)
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest.append(log_thread_name.data(), log_thread_name.data() + log_thread_name.size());
  }

  [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> thread_name_formatter::clone() const
  {
    return std::make_unique<thread_name_formatter>();
  }

  // clang-format off
  enum level Logger::impl::console_level() const noexcept { return console_sink_ ? static_cast<enum level>(console_sink_->level()) : level::off; }
  enum level Logger::impl::file_level() const noexcept    { return file_sink_    ? static_cast<enum level>(file_sink_->level())    : level::off; }
  enum level Logger::impl::level() const noexcept         { return logger_      ? static_cast<enum level>(logger_->level())       : level::off; }
  // clang-format on

  void Logger::impl::set_console_level(enum level l)
  {
    if (console_sink_) console_sink_->set_level(static_cast<spdlog::level::level_enum>(l));
  }

  void Logger::impl::set_file_level(enum level l)
  {
    if (file_sink_) file_sink_->set_level(static_cast<spdlog::level::level_enum>(l));
  }

  void Logger::impl::set_level(enum level l)
  {
    if (logger_) logger_->set_level(static_cast<spdlog::level::level_enum>(l));
  }

  void Logger::impl::flush() const
  {
    if (logger_) logger_->flush();
  }

  void Logger::impl::flush_on(enum level l)
  {
    if (logger_) logger_->flush_on(static_cast<spdlog::level::level_enum>(l));
  }

  void Logger::impl::_log(enum level l, std::string_view s) const { logger_->log(static_cast<spdlog::level::level_enum>(l), s); }

  void Logger::impl::log_exception_with_chain(const std::exception& e, enum level lvl) const
  {
    std::ostringstream oss;
    oss << "EXCEPTION: " << e.what() << "\nBACKTRACE:\n";
    for (const auto& entry : std::stacktrace::current()) oss << "  " << entry << "\n";
    oss << "CAUSE CHAIN:\n";
    _log(lvl, oss.str());
    log_nested_chain(e, 1);
  }

  void Logger::impl::log_current_exception_with_chain(enum level lvl) const
  {
    if (auto ex = std::current_exception())
    {
      try
      {
        std::rethrow_exception(ex);
      }
      catch (const std::exception& e)
      {
        log_exception_with_chain(e, lvl);
      }
      catch (...)
      {
        _log(lvl, "Unknown exception (not std::exception)");
      }
    }
  }

  void Logger::impl::log_nested_chain(const std::exception& e, int depth) const // NOLINT(misc-no-recursion)
  {
    try
    {
      std::rethrow_if_nested(e);
    }
    catch (const std::exception& nested)
    {
      std::ostringstream oss;
      oss << std::string(depth * 2UL, ' ') << "└─ " << nested.what() << "\n";
      _log(level::critical, oss.str());
      log_nested_chain(nested, depth + 1);
    }
    catch (...)
    {
      std::ostringstream oss;
      oss << std::string((depth + 1UL) * 2, ' ') << "└─ [unknown nested exception]";
      _log(level::critical, oss.str());
    }
  }

  void Logger::impl::log_backtrace(const Logger& self, const std::string& title)
  {
    std::ostringstream oss;
    oss << title << "\n";
    for (const auto& entry : std::stacktrace::current()) oss << "  " << entry << "\n";
    self.critical("{}", oss.str());
  }

  std::unique_ptr<spdlog::pattern_formatter> Logger::impl::make_formatter(std::string_view pattern)
  {
    std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
    flags['*'] = std::make_unique<thread_name_formatter>();
    return std::make_unique<spdlog::pattern_formatter>(
      std::string(pattern), spdlog::pattern_time_type::local, spdlog::details::os::default_eol, std::move(flags));
  }

  Logger::impl::impl(const logger_config& cfg) { build(cfg); }

  void Logger::impl::build(const logger_config& cfg)
  {
    auto log_folder_abs = fs::absolute(fs::path(cfg.log_folder)).string();
    if (! fs::exists(log_folder_abs))
    {
      // NOLINTNEXTLINE(concurrency-mt-unsafe, readability-magic-numbers)
      auto sts = mkdir(log_folder_abs.c_str(), 0755); /// NOLINT
      if (sts != 0) throw std::runtime_error(fmt::format("Can't create folder '{}'", log_folder_abs));
    }

    console_sink_ = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink_->set_formatter(make_formatter(cfg.pattern));
    set_console_level(cfg.console_level);

    auto log_filename = fmt::format("{}/{}.log", log_folder_abs, cfg.app_name);
    file_sink_         = std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_filename, cfg.rotation_hour, cfg.rotation_minute, true, cfg.keep_days);
    file_sink_->set_formatter(make_formatter(cfg.pattern));
    set_file_level(cfg.file_level);

    std::vector<spdlog::sink_ptr> sinks{console_sink_, file_sink_};

    if (cfg.run_mode == mode::async)
    {
      spdlog::init_thread_pool(8192, 1); // NOLINT(readability-magic-numbers)
      logger_ = std::make_shared<spdlog::async_logger>(
        cfg.app_name, sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    }
    else
    {
      logger_ = std::make_shared<spdlog::logger>(cfg.app_name, sinks.begin(), sinks.end());
    }

    flush_on(cfg.flush_on);
    set_level(std::min(cfg.console_level, cfg.file_level));
  }
} // namespace logger
