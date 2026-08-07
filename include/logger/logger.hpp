#pragma once
/**
 * @file
 * @brief logging facade - no spdlog type or header ever appears here
 *
 * A thin wrapper around spdlog, hidden behind a Pimpl (Logger::impl, defined
 * in logger_impl.hpp/.cpp) so that every other header in a project that
 * needs a Logger only ever sees this file.
 *
 * No singleton: a Logger is built once (typically at startup, from a
 * logger_config, via the create() factory below) and handed down by
 * reference to whatever needs to log - constructors, worker threads,
 * whatever. This is what lets two independent loggers coexist in one
 * process (e.g. one per subsystem) without fighting over global state, and
 * it is why Logger is neither copyable nor movable: ownership is meant to
 * stay exactly where it was constructed, everyone else only ever sees a
 * reference.
 *
 * Never throws: create() reports anything that can go wrong building a
 * Logger's sinks through its std::expected return instead - see its own
 * comment below.
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
#include <expected>
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
    /**
     * @brief builds a Logger from cfg, or an error describing why it could
     * not be built
     *
     * A Logger is not allowed to throw - anything that goes wrong while
     * setting up its sinks (an unwritable log_folder, a rotating file it
     * cannot open, ...) is reported through the returned std::expected
     * instead, already logged to stderr before create() returns, and
     * otherwise left for the caller to decide what to do (a broken log
     * destination is not this library's call to make - abort startup,
     * fall back to some other logger_config, or something else entirely).
     * Wrapped in a unique_ptr rather than returned by value: Logger itself
     * stays neither copyable nor movable (see the class comment above), so
     * std::expected<Logger, ...> is not an option.
     */
    [[nodiscard]] static std::expected<std::unique_ptr<Logger>, std::string> create(const logger_config& cfg);

    /**
     * @brief create(cfg), or print the error and exit(1) if it fails
     *
     * The common case at the top of main(): a program that cannot get a
     * Logger built has nowhere left to report anything, so this prints
     * create()'s error to stdout and terminates the process rather than
     * handing every caller the same three lines (check the std::expected,
     * print, exit) to repeat. Also calls make_log_name(cfg.app_name) on
     * success, so the returned Logger is immediately ready for %* to read
     * its logical thread name - the other half of what a caller normally
     * does right after building one.
     *
     * Not for anything other than a program's own startup: a library or a
     * worker thread that fails to build a Logger should still get to decide
     * for itself what "no logger" means, which is exactly what create()
     * (returning std::expected instead of exiting) is for.
     */
    [[nodiscard]] static std::unique_ptr<Logger> create_or_exit(const logger_config& cfg);

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
    static void                      make_log_name(std::string_view parent, std::string_view child = "");
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

    void _log(enum level l, std::string_view s) const;

    class impl;
    // Takes ownership of an already-built impl - never fails, so Logger's
    // own constructor stays exception-free. All the ways building an impl
    // can go wrong are handled in create() below, which builds the impl
    // first and only constructs a Logger once that succeeded.
    explicit Logger(std::unique_ptr<impl> p) noexcept;
    std::unique_ptr<impl> pimpl_;

    // The signal handler is a free function (see signal_handler above) with
    // no way to receive `this`, so it reaches back through this pointer -
    // set in setup_signal_handler(), cleared in ~Logger(). Only ever set on
    // the one Logger a process calls setup_signal_handler() on.
    static Logger* signal_target_; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  // The six formatted overloads below are templates on Args... - each
  // distinct argument-type combination a caller instantiates them with is
  // its own separate function as far as the compiler (and gcov) are
  // concerned. test_logger.cpp's own tests exercise both branches of every
  // `if (!active(...)) return;` and both outcomes of every `catch (...)`
  // for the specific instantiations it uses (int, throwing_arg, ...) - but
  // other translation units linked into the same coverage run (this
  // library's own log_backtrace(), which calls critical() with a
  // std::string argument) instantiate these same templates again with
  // *their* argument types, and gcov reports branch coverage per
  // instantiation, not merged across them. Getting every instantiation
  // anywhere in the program to hit both branches isn't practical, so branch
  // coverage is excluded here; line/function coverage (which report "was
  // this template instantiated and run at all", not "every branch of every
  // instantiation") are not.
  // GCOVR_EXCL_BR_START
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
  // GCOVR_EXCL_BR_STOP

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
  // critical(string_view)'s active()==false branch and make_log_name()'s
  // child.empty()==false branch are both fully exercised - but only in
  // test_logger.cpp's own translation unit (see "critical/error/warn/info/
  // debug/trace do not throw when suppressed by level" and "make_log_name
  // (parent, child) joins parent and non-empty child with a slash"). Being
  // `inline`, each translation unit that calls these gets its own copy;
  // logger.cpp's copy of critical(string_view) is only ever reached with
  // level::critical already active (log_backtrace()/signal_handler()/the
  // terminate handler all call it on a path that's already decided to
  // log), and logger.cpp never calls make_log_name() at all. gcovr reports
  // branch coverage per source line across every translation unit's copy,
  // not the best one, so the line shows as partially covered overall
  // despite being fully covered where it's actually under test.
  inline void Logger::critical(std::string_view sv) const
  {
    if (active(level::critical)) _log(level::critical, sv); // GCOVR_EXCL_BR_LINE
  }

  inline std::string Logger::log_name() { return log_thread_name; }
  inline void        Logger::make_log_name(std::string_view parent, std::string_view child)
  {
    log_thread_name = child.empty() ? std::string(parent) : fmt::format("{}/{}", parent, child); // GCOVR_EXCL_BR_LINE
  }
} // namespace logger
