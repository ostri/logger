#pragma once
/**
 * @file
 * @brief a small structured error type, used by demo.cpp to show how a
 * caller's own error type plugs into Logger::error<E>()/critical<E>()
 *
 * Not part of the logger library itself - logger stays free of any
 * dependency on a specific error type. This is the shape a caller (fsp's
 * error_info was the original inspiration) would give its own type: a
 * closed set of error codes, a message, and enough location context to find
 * the failure, all folded into one line by to_string().
 */

#include <cstdint>
#include <string>

namespace demo
{
  /**
   * @brief closed set of error codes this demo program can report
   *
   * unknown starts at 1, not the implicit 0 - 0 is deliberately left unused
   * so that a default-constructed or zero-initialized error_code value
   * reads as "not a valid code" rather than colliding with a real one.
   */
  enum class error_code : std::uint8_t
  {
    unknown = 1,
    file_not_found,
    parse_failed,
    validation_failed,
  };

  class error_info
  {
  public:
    error_info() = default;
    error_info(error_code code, std::string message, std::string path = "", std::size_t line = 0)
    : code_(code), message_(std::move(message)), path_(std::move(path)), line_(line)
    {
    }

    [[nodiscard]] std::string to_string() const;
  private:
    error_code  code_ = error_code::unknown;
    std::string message_;
    std::string path_;
    std::size_t line_ = 0;
  };
} // namespace demo
