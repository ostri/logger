#include "error_info.hpp"
#include <fmt/format.h>

namespace demo
{
  namespace
  {
    const char* to_string(error_code code)
    {
      switch (code)
      {
      case error_code::unknown: return "unknown";
      case error_code::file_not_found: return "file_not_found";
      case error_code::parse_failed: return "parse_failed";
      case error_code::validation_failed: return "validation_failed";
      }
      return "unknown";
    }
  } // namespace

  std::string error_info::to_string() const
  {
    if (path_.empty()) return fmt::format("[{}] {}", demo::to_string(code_), message_);
    return fmt::format("[{}] {} ({}:{})", demo::to_string(code_), message_, path_, line_);
  }
} // namespace demo
