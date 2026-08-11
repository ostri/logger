#pragma once
#include "logger/logger.hpp" // IWYU pragma: keep

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <expected>
#include <memory>
#include <string>

namespace logger
{
  /**
   * @brief custom spdlog format flag: prints the logical thread name that
   * produced the record. Register it as '%*' in a pattern string.
   *
   * Reads it from msg.payload, not from the thread_local log_thread_name
   * (see logger.hpp) directly: with an async Logger, the pattern_formatter
   * that calls this runs on spdlog's own backing thread, not the thread that
   * called Logger::info()/etc., so log_thread_name there is whatever that
   * backing thread's own (never set) copy holds - always "unknown". _log()
   * (see logger_impl.cpp) works around this by capturing log_thread_name on
   * the calling thread and prepending it to the payload, separated by
   * kThreadNamePayloadSep; this formatter (and message_body_formatter below,
   * which strips it back off for '%v') is the other half of that scheme.
   */
  class thread_name_formatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
  };

  /**
   * @brief custom spdlog format flag: prints the record's actual message
   * text, with the thread_name_formatter's own prefix (see its comment
   * above) stripped back off. Register it as '%v' in a pattern string,
   * overriding spdlog's own built-in "%v" handler (pattern_formatter checks
   * custom flags before built-in ones - see handle_flag_()).
   */
  class message_body_formatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
  };

  /// @brief separates the thread name prefix _log() adds to a message's
  /// payload from the actual message text - see thread_name_formatter's own
  /// comment above. Unit Separator (0x1F): not something a thread name
  /// (make_log_name() arguments) or a formatted log message is ever
  /// expected to contain, unlike '[', ']', or ' '.
  inline constexpr char kThreadNamePayloadSep = '\x1f';

  /**
   * @brief custom spdlog format flag: prints the record's level, fixed-width
   * (5 characters, left-padded with spaces), so every level lines up in one
   * column. spdlog's own "%l" varies in width (info=4, warning=7,
   * critical=8, ...) and cannot be shortened via a pattern spec alone;
   * "critical" (spdlog's only name over 5 characters) is abbreviated to
   * "crit" here rather than left to overflow the column. Register it as
   * '%L' in a pattern string.
   */
  class level_name_formatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
  };

  class Logger::impl
  {
  public:
    /// @brief builds an impl from cfg, or an error describing why it could
    /// not be built - never throws (see Logger::create()'s own comment).
    [[nodiscard]] static std::expected<std::unique_ptr<impl>, std::string> create(const logger_config& cfg);

    [[nodiscard]] enum level console_level() const noexcept;
    [[nodiscard]] enum level file_level() const noexcept;
    [[nodiscard]] enum level level() const noexcept;
    void                     set_console_level(enum level l);
    void                     set_file_level(enum level l);
    void                     set_level(enum level l);
    void                     flush() const;
    void                     flush_on(enum level l);

    void _log(enum level l, std::string_view s) const;

    void        log_exception_with_chain(const std::exception& e, enum level lvl) const;
    void        log_current_exception_with_chain(enum level lvl) const;
    void        log_nested_chain(const std::exception& e, int depth) const;
    static void log_backtrace(const Logger& self, const std::string& title);
  private:
    impl() = default;

    static std::unique_ptr<spdlog::pattern_formatter> make_formatter(std::string_view pattern);
    // Populates this impl's sinks/logger from cfg. May throw - anything it
    // throws (its own log_folder creation failure, or spdlog's own sink
    // constructors failing to open a file) is caught in create() above,
    // which is the only caller.
    void build(const logger_config& cfg);

    std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> console_sink_;
    std::shared_ptr<spdlog::sinks::daily_file_sink_mt>   file_sink_;
    std::shared_ptr<spdlog::logger>                      logger_;
  };
} // namespace logger
