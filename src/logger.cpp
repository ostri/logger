#include "logger_impl.hpp"
#include <csignal>
#include <sstream>
#include <stacktrace>

namespace logger
{
  Logger* Logger::signal_target_ = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

  Logger::Logger(const logger_config& cfg) : pimpl_(std::make_unique<impl>(cfg)) {}

  Logger::~Logger()
  {
    if (signal_target_ == this) signal_target_ = nullptr;
  }

  enum level Logger::level() const noexcept { return pimpl_->level(); }
  enum level Logger::console_level() const noexcept { return pimpl_->console_level(); }
  enum level Logger::file_level() const noexcept { return pimpl_->file_level(); }
  void       Logger::set_console_level(enum level l) { pimpl_->set_console_level(l); }
  void       Logger::set_file_level(enum level l) { pimpl_->set_file_level(l); }
  void       Logger::set_level(enum level l) { pimpl_->set_level(l); }
  bool       Logger::active(enum level l) const noexcept { return l >= pimpl_->level(); }

  void Logger::flush() const { pimpl_->flush(); }
  void Logger::flush_on(enum level l) { pimpl_->flush_on(l); }

  void Logger::_log(enum level l, std::string_view s) const { pimpl_->_log(l, s); }

  void Logger::log_exception_with_chain(const std::exception& e, enum level l) const { pimpl_->log_exception_with_chain(e, l); }
  void Logger::log_current_exception_with_chain(enum level l) const { pimpl_->log_current_exception_with_chain(l); }

  void Logger::log_nested_chain(const std::exception& e, int depth) const { pimpl_->log_nested_chain(e, depth); }

  void Logger::log_backtrace(const std::string& title) const { impl::log_backtrace(*this, title); }

  void Logger::setup_terminate_handler() const
  {
    signal_target_ = const_cast<Logger*>(this); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    std::set_terminate(
      []()
      {
        const Logger* const self = signal_target_;
        if (self == nullptr) { std::abort(); }

        if (auto e = std::current_exception())
        {
          try
          {
            std::rethrow_exception(e);
          }
          catch (const std::exception& ex)
          {
            self->log_exception_with_chain(ex);
          }
          catch (...)
          {
            self->critical("UNKNOWN EXCEPTION - terminating!");
          }
        }
        else
        {
          self->critical("std::terminate() called without exception");
        }

        std::ostringstream oss;
        oss << "BACKTRACE at terminate():\n";
        for (const auto& entry : std::stacktrace::current()) oss << "  " << entry << "\n";
        self->critical("{}", oss.str());

        self->flush();
        std::abort();
      });
  }

  void Logger::setup_signal_handler() const
  {
    signal_target_ = const_cast<Logger*>(this); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    const std::array<int, 5> signals = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGTERM};
    // NOLINTNEXTLINE(cert-err33-c)
    for (const int sig : signals) std::signal(sig, Logger::signal_handler);
  }

  void Logger::signal_handler(int sig)
  {
    const Logger* const self = signal_target_;
    if (self == nullptr) std::exit(128 + sig); // NOLINT(concurrency-mt-unsafe, readability-magic-numbers)

    const char* name = signal_name(sig);
    self->critical("SIGNAL {} ({}) - application terminating!", name, sig);
    self->log_backtrace("BACKTRACE at signal:");
    self->flush();
    // NOLINTNEXTLINE(concurrency-mt-unsafe, readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers)
    std::exit(128 + sig);
  }

  const char* Logger::signal_name(int sig)
  {
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGTERM: return "SIGTERM";
    default: return "UNKNOWN";
    }
  }
} // namespace logger
