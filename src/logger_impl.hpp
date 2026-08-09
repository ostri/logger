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
   * @brief custom spdlog format flag: prints log_thread_name of the thread
   * that produced the record. Register it as '%*' in a pattern string.
   */
  class thread_name_formatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
  };

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
