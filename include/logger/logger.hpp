#pragma once
/**
 * @file
 * @brief logging facade - no spdlog type or header ever appears here
 *
 * A thin wrapper around spdlog, hidden behind a Pimpl (Logger::impl, defined
 * in logger_impl.hpp/.cpp) so that every other header in a project that
 * needs a Logger only ever sees this file.
 *
 * No singleton: a Logger is a value you construct once (typically at
 * startup, from a logger_config) and hand down by reference to whatever
 * needs to log - constructors, worker threads, whatever. This is what lets
 * two independent loggers coexist in one process (e.g. one per subsystem)
 * without fighting over global state, and it is why Logger is neither
 * copyable nor movable: ownership is meant to stay exactly where it was
 * constructed, everyone else only ever sees a reference.
 *
 * debug()/trace() are compiled out entirely in a release build (`if
 * constexpr` on is_debug_build(), evaluated at compile time) rather than
 * just filtered at runtime - the call site's formatting work never makes it
 * into the release binary. info/warn/error/critical always compile in and
 * are filtered at runtime through active(), which is checked *before* the
 * message is formatted - a call at a level that will not be printed never
 * pays for fmt::format.
 */

#include "build_type.hpp"
#include "logger_config.hpp"
#include <fmt/format.h>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>

namespace logger
{
  // ---------------------------------------------------------------------
  // thread_local logical name of the current thread, read by the '%*'
  // custom format flag (ThreadNameFormatter, see logger_impl.hpp) - lets a
  // log line name the worker/pipeline stage that produced it independent of
  // the OS thread id, which is neither stable nor meaningful to a reader.
  // Defined inline so it is visible in every translation unit without an
  // ODR violation.
  // ---------------------------------------------------------------------
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp, bugprone-throwing-static-initialization)
  inline thread_local std::string log_thread_name = "unknown";

  class Logger
  {
  public:
    explicit Logger(const logger_config& cfg);
    ~Logger();

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;

    [[nodiscard]] enum level level() const noexcept;
    [[nodiscard]] enum level console_level() const noexcept;
    [[nodiscard]] enum level file_level() const noexcept;
    void                     set_console_level(enum level l);
    void                     set_file_level(enum level l);
    void                     set_level(enum level l);

    /// true if a message at this level would actually reach a sink - check
    /// this before doing expensive work to build a message by hand
    [[nodiscard]] bool active(enum level l) const noexcept;

    // clang-format off
    template <typename... Args> void trace   (fmt::format_string<Args...> fmt, Args&&... args) const noexcept;
    template <typename... Args> void debug   (fmt::format_string<Args...> fmt, Args&&... args) const noexcept;
    template <typename... Args> void info    (fmt::format_string<Args...> fmt, Args&&... args) const noexcept;
    template <typename... Args> void warn    (fmt::format_string<Args...> fmt, Args&&... args) const noexcept;
    template <typename... Args> void error   (fmt::format_string<Args...> fmt, Args&&... args) const noexcept;
    template <typename... Args> void critical(fmt::format_string<Args...> fmt, Args&&... args) const noexcept;
    // clang-format on
    void trace(std::string_view sv) const;
    void debug(std::string_view sv) const;
    void info(std::string_view sv) const;
    void warn(std::string_view sv) const;
    void error(std::string_view sv) const;
    void critical(std::string_view sv) const;

    /**
     * @brief logs any type E that offers `std::string E::to_string() const`,
     * e.g. a caller's own structured error type
     *
     * Nothing about E is baked into logger - it stays a general-purpose
     * facade rather than growing a dependency on any one project's error
     * type. A caller wanting to log e.g. an error code, a path and a line
     * number together defines its own type with a to_string() and gets
     * error()/critical() for free through these overloads; see demo/ for a
     * worked example (an error_info type with a code/message/location).
     *
     * Constrained with `requires` rather than left as an unconstrained
     * template: an unconstrained `template<typename E> void error(const
     * E&)` is an exact match for a string literal argument (E deduced as
     * char[N]), which overload resolution prefers over the non-template
     * error(std::string_view) that would otherwise take it via an implicit
     * conversion - silently routing every plain string call through here
     * and failing to compile the moment it tried e.to_string(). The
     * requires-clause excludes anything without a to_string(), so a bare
     * string keeps going to the string_view overload above.
     */
    template <typename E>
      requires requires(const E& e) {
        { e.to_string() } -> std::convertible_to<std::string>;
      }
    void error(const E& e) const
    {
      if (active(level::error)) error(std::string_view(e.to_string()));
    }
    template <typename E>
      requires requires(const E& e) {
        { e.to_string() } -> std::convertible_to<std::string>;
      }
    void critical(const E& e) const
    {
      if (active(level::critical)) critical(std::string_view(e.to_string()));
    }

    void flush() const;
    void flush_on(enum level l);

    /// @brief sets the logical thread name %* reads - see log_thread_name above
    static void make_log_name(std::string_view parent, std::string_view child = "");
    [[nodiscard]] static std::string log_name();

    /// @brief logs e.what(), a stack trace captured at the call site, and -
    /// recursively - every exception in e's std::nested_exception chain
    void log_exception_with_chain(const std::exception& e, enum level l = level::critical) const;
    /// @brief same as above, for the exception currently being handled
    /// (call from inside a catch block, or a std::terminate handler)
    void log_current_exception_with_chain(enum level l = level::critical) const;

    /// @brief routes std::terminate through this Logger (stack trace, cause
    /// chain if terminate was reached via an uncaught exception) before
    /// aborting. Installs a process-wide handler - call once, from main().
    void setup_terminate_handler() const;
    /// @brief logs SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGTERM with a stack trace
    /// before exiting. Installs process-wide signal handlers - call once,
    /// from main(). Only one Logger in a process should call this: the
    /// signal handler has no way to know which Logger instance to reach.
    void setup_signal_handler() const;
  private:
    static void        signal_handler(int sig);
    static const char* signal_name(int sig);
    void               log_backtrace(const std::string& title) const;
    void               log_nested_chain(const std::exception& e, int depth) const;

    void _log(enum level l, std::string_view s) const;

    class impl;
    std::unique_ptr<impl> pimpl_;

    // The signal handler is a free function (see signal_handler above) with
    // no way to receive `this`, so it reaches back through this pointer -
    // set in setup_signal_handler(), cleared in ~Logger(). Only ever set on
    // the one Logger a process calls setup_signal_handler() on.
    static Logger* signal_target_; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  template <typename... Args>
  inline void Logger::trace([[maybe_unused]] fmt::format_string<Args...> fmt, [[maybe_unused]] Args&&... args) const noexcept
  {
    if constexpr (is_debug_build())
    {
      if (! active(level::trace)) return;
      try
      {
        _log(level::trace, fmt::format(fmt, std::forward<Args>(args)...));
      }
      catch (...) // NOLINT(bugprone-empty-catch)
      {
      }
    }
  }

  template <typename... Args>
  inline void Logger::debug([[maybe_unused]] fmt::format_string<Args...> fmt, [[maybe_unused]] Args&&... args) const noexcept
  {
    if constexpr (is_debug_build())
    {
      if (! active(level::debug)) return;
      try
      {
        _log(level::debug, fmt::format(fmt, std::forward<Args>(args)...));
      }
      catch (...) // NOLINT(bugprone-empty-catch)
      {
      }
    }
  }

  template <typename... Args>
  inline void Logger::info(fmt::format_string<Args...> fmt, Args&&... args) const noexcept
  {
    if (! active(level::info)) return;
    try
    {
      _log(level::info, fmt::format(fmt, std::forward<Args>(args)...));
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
  }

  template <typename... Args>
  inline void Logger::warn(fmt::format_string<Args...> fmt, Args&&... args) const noexcept
  {
    if (! active(level::warn)) return;
    try
    {
      _log(level::warn, fmt::format(fmt, std::forward<Args>(args)...));
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
  }

  template <typename... Args>
  inline void Logger::error(fmt::format_string<Args...> fmt, Args&&... args) const noexcept
  {
    if (! active(level::error)) return;
    try
    {
      _log(level::error, fmt::format(fmt, std::forward<Args>(args)...));
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
  }

  template <typename... Args>
  inline void Logger::critical(fmt::format_string<Args...> fmt, Args&&... args) const noexcept
  {
    if (! active(level::critical)) return;
    try
    {
      _log(level::critical, fmt::format(fmt, std::forward<Args>(args)...));
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
  }

  inline void Logger::trace([[maybe_unused]] std::string_view sv) const
  {
    if constexpr (is_debug_build())
      if (active(level::trace)) _log(level::trace, sv);
  }
  inline void Logger::debug([[maybe_unused]] std::string_view sv) const
  {
    if constexpr (is_debug_build())
      if (active(level::debug)) _log(level::debug, sv);
  }
  inline void Logger::info(std::string_view sv) const
  {
    if (active(level::info)) _log(level::info, sv);
  }
  inline void Logger::warn(std::string_view sv) const
  {
    if (active(level::warn)) _log(level::warn, sv);
  }
  inline void Logger::error(std::string_view sv) const
  {
    if (active(level::error)) _log(level::error, sv);
  }
  inline void Logger::critical(std::string_view sv) const
  {
    if (active(level::critical)) _log(level::critical, sv);
  }

  inline std::string Logger::log_name() { return log_thread_name; }
  inline void        Logger::make_log_name(std::string_view parent, std::string_view child)
  {
    log_thread_name = child.empty() ? std::string(parent) : fmt::format("{}/{}", parent, child);
  }
} // namespace logger
