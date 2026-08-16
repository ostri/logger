#include "logger_impl.hpp"
#include <spdlog/async.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stacktrace>
#include <string_view>
#include <unordered_map>

namespace fs = std::filesystem;

namespace logger
{
  namespace
  {
    // Splits msg.payload into the thread name _log() prepended and the
    // actual message text, at the first kThreadNamePayloadSep - see
    // thread_name_formatter's own comment in logger_impl.hpp. A payload with
    // no separator (nothing today produces one - see _log() below - but
    // never say never) is treated as having no thread name prefix at all,
    // same as before this scheme existed: log_thread_name's own built-in
    // default ("unknown") for the name half, the whole payload for the
    // message half.
    struct split_payload
    {
      std::string_view thread_name;
      std::string_view message;
    };

    [[nodiscard]] split_payload split_thread_name(spdlog::string_view_t payload)
    {
      const std::string_view sv(payload.data(), payload.size());
      const auto             sep = sv.find(kThreadNamePayloadSep);
      if (sep == std::string_view::npos) return {.thread_name = log_thread_name, .message = sv};
      return {.thread_name = sv.substr(0, sep), .message = sv.substr(sep + 1)};
    }
  } // namespace

  void thread_name_formatter::format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/, spdlog::memory_buf_t& dest)
  {
    const auto name = split_thread_name(msg.payload).thread_name;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest.append(name.data(), name.data() + name.size());
  }

  [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> thread_name_formatter::clone() const
  { return std::make_unique<thread_name_formatter>(); }

  void message_body_formatter::format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/, spdlog::memory_buf_t& dest)
  {
    const auto text = split_thread_name(msg.payload).message;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest.append(text.data(), text.data() + text.size());
  }

  [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> message_body_formatter::clone() const
  { return std::make_unique<message_body_formatter>(); }

  namespace
  {
    // Fixed 5-character level names, space-padded - spdlog's own names ("info"=4,
    // "warning"=7, "critical"=8, ...) do not line up in a column; "critical" is
    // abbreviated to "crit" (not truncated to "criti") so it stays recognisable.
    constexpr std::array<std::string_view, 7> level_names_5{
      "trace",
      "debug",
      "info ",
      "warn ",
      "error",
      "crit ",
      "off  ",
    };
  } // namespace

  void level_name_formatter::format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/, spdlog::memory_buf_t& dest)
  {
    const auto             idx  = static_cast<std::size_t>(msg.level);
    const std::string_view name = idx < level_names_5.size()
                                    ? level_names_5.at(idx)
                                    : std::string_view("?????"); // GCOVR_EXCL_BR_LINE - spdlog never hands out a level outside its own enum
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest.append(name.data(), name.data() + name.size());
  }

  [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> level_name_formatter::clone() const
  { return std::make_unique<level_name_formatter>(); }

  // The null-checks below (console_sink_/file_sink_/logger_) never take
  // their false branch in practice: build() always sets all three before
  // anything else on this impl can be called (see build() further down),
  // and there is no public path to reach an impl before build() has run.
  // Kept as a defensive fallback, not as a tested path.
  // clang-format off
  enum level Logger::impl::console_level() const noexcept { return console_sink_ ? static_cast<enum level>(console_sink_->level()) : level::off; } // GCOVR_EXCL_BR_LINE
  enum level Logger::impl::file_level() const noexcept    { return file_sink_    ? static_cast<enum level>(file_sink_->level())    : level::off; } // GCOVR_EXCL_BR_LINE
  enum level Logger::impl::level() const noexcept         { return logger_      ? static_cast<enum level>(logger_->level())       : level::off; } // GCOVR_EXCL_BR_LINE
  // clang-format on

  // Delegates to daily_file_sink_mt::filename(), which already tracks the current
  // rotated name itself (base_filename_YYYY-MM-DD.ext, recomputed on every
  // rotation - see spdlog/sinks/daily_file_sink.h's own daily_filename_calculator)
  // - no need to duplicate that date/rotation logic here, spdlog already
  // maintains the one true answer.
  std::string Logger::impl::log_filename() const { return file_sink_ ? file_sink_->filename() : std::string{}; } // GCOVR_EXCL_BR_LINE

  void Logger::impl::set_console_level(enum level l)
  {
    if (console_sink_) console_sink_->set_level(static_cast<spdlog::level::level_enum>(l)); // GCOVR_EXCL_BR_LINE
  }

  void Logger::impl::set_file_level(enum level l)
  {
    if (file_sink_) file_sink_->set_level(static_cast<spdlog::level::level_enum>(l)); // GCOVR_EXCL_BR_LINE
  }

  void Logger::impl::set_level(enum level l)
  {
    if (logger_) logger_->set_level(static_cast<spdlog::level::level_enum>(l)); // GCOVR_EXCL_BR_LINE
  }

  void Logger::impl::flush() const
  {
    if (logger_) logger_->flush(); // GCOVR_EXCL_BR_LINE
  }

  void Logger::impl::flush_on(enum level l)
  {
    if (logger_) logger_->flush_on(static_cast<spdlog::level::level_enum>(l)); // GCOVR_EXCL_BR_LINE
  }

  void Logger::impl::_log(enum level l, std::string_view s) const
  {
    // Captures log_thread_name on this thread - the only one guaranteed to
    // have the value make_log_name() actually set on it - and carries it
    // along inside the payload itself, rather than leaving %* to read
    // log_thread_name at format time. An async Logger's pattern_formatter
    // runs on spdlog's own backing thread, where log_thread_name was never
    // set; a sync Logger formats on this same thread, so this changes
    // nothing observable there. See thread_name_formatter's own comment in
    // logger_impl.hpp for the other half of this scheme.
    logger_->log(static_cast<spdlog::level::level_enum>(l), fmt::format("{}{}{}", log_thread_name, kThreadNamePayloadSep, s));
  }

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
    flags['L'] = std::make_unique<level_name_formatter>();
    flags['v'] = std::make_unique<message_body_formatter>();
    return std::make_unique<spdlog::pattern_formatter>(
      std::string(pattern), spdlog::pattern_time_type::local, spdlog::details::os::default_eol, std::move(flags));
  }

  std::expected<std::unique_ptr<Logger::impl>, std::string> Logger::impl::create(const logger_config& cfg)
  {
    // impl() itself never throws (it default-constructs three null
    // shared_ptrs and nothing else) - only build() can, so it alone needs
    // catching. std::exception covers both this file's own
    // std::runtime_error (mkdir failure) and spdlog::spdlog_ex (sink
    // construction failure, e.g. an unwritable log file) - spdlog_ex derives
    // from std::exception, so there is no need to name it separately here.
    auto p = std::unique_ptr<impl>(
      new impl()); // GCOVR_EXCL_BR_LINE -- unique_ptr's own null-check branch, not something a caller-visible path can steer
    try
    {
      p->build(cfg);
    }
    // Same gcov branch-attribution quirk noted on logger_config.cpp's
    // catches - this one is exercised too (see "Logger::create returns an
    // error instead of throwing...").
    catch (const std::exception& e) // GCOVR_EXCL_BR_LINE
    {
      // No sink exists yet to log this through - stderr is the only
      // destination left, same as load_logger_config()'s own fallback
      // message (logger_config.cpp).
      std::cerr << "logger::Logger::create() failed: " << e.what() << '\n';
      return std::unexpected(e.what());
    }
    return p;
  }

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

    // truncate=false (spdlog's own default too, spelled out here rather than
    // left implicit): a caller building more than one Logger against the
    // same app_name/log_folder on the same day - e.g. a short-lived
    // bootstrap Logger followed by the real one, once its own config has
    // been read (see ach's tool.cpp for a worked example) - appends to
    // today's file instead of truncating away whatever the previous Logger
    // already wrote to it.
    auto log_filename = fmt::format("{}/{}.log", log_folder_abs, cfg.app_name);
    file_sink_ =
      std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_filename, cfg.rotation_hour, cfg.rotation_minute, false, cfg.keep_days);
    file_sink_->set_formatter(make_formatter(cfg.pattern));
    set_file_level(cfg.file_level);

    std::vector<spdlog::sink_ptr> sinks{console_sink_,
                                        file_sink_}; // GCOVR_EXCL_BR_LINE -- std::vector's own bad_alloc branch, not exercised on purpose

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
