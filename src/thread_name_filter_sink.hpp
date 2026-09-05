#pragma once
/**
 * @file
 * @brief thread_name_filter_sink - passes a record on only when its thread
 * name matches, used to narrow one sink without touching the others
 *
 * Wraps another sink rather than replacing it: everything about how the
 * inner sink formats and writes stays exactly as it was, and a sink that is
 * not wrapped (the file sink) keeps receiving every record regardless of
 * what is set here. That asymmetry is the point - a filter is a way to read
 * the console during development, never a way to lose what the log file
 * records.
 *
 * The thread name is read out of the payload rather than from
 * log_thread_name: an async Logger formats on spdlog's own backing thread,
 * where that thread_local was never set, so the name a record belongs to is
 * the one Logger::impl::_log() captured on the calling thread and carried
 * along inside the payload (see its own comment for the other half of this
 * scheme).
 */
#include <spdlog/sinks/sink.h>
#include <algorithm>
#include <spdlog/details/log_msg.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logger
{
  /**
   * @brief a sink that forwards to another one only for matching thread names
   *
   * With no prefixes set - the state every Logger starts in - every record is
   * forwarded, so wrapping a sink in this costs one string compare that never
   * fires. Setting prefixes narrows what reaches the inner sink to the
   * threads whose name starts with one of them; matching on a prefix rather
   * than the whole name is what makes "wd-doc" also cover "wd-doc/worker-3"
   * (see Logger::make_log_name()'s own parent/child form).
   */
  class thread_name_filter_sink : public spdlog::sinks::sink
  {
  public:
    /// @brief wraps @p inner - which keeps its own formatter, level and output
    /// @param inner the sink every passing record is forwarded to
    /// @param separator what _log() puts between the thread name and the
    ///        message (kThreadNamePayloadSep - passed in rather than included,
    ///        so this header does not depend on logger_impl.hpp)
    thread_name_filter_sink(std::shared_ptr<spdlog::sinks::sink> inner, char separator) noexcept
    : inner_(std::move(inner))
    , separator_(separator)
    {
    }

    /// @brief the thread-name prefixes let through; empty means "everything"
    void set_prefixes(std::vector<std::string> prefixes)
    {
      const std::scoped_lock lock(mutex_);
      prefixes_ = std::move(prefixes);
    }

    /// @brief what set_prefixes() last installed
    [[nodiscard]] std::vector<std::string> prefixes() const
    {
      const std::scoped_lock lock(mutex_);
      return prefixes_;
    }

    void log(const spdlog::details::log_msg& msg) override
    {
      if (! passes(msg)) return;
      inner_->log(msg);
    }

    void flush() override { inner_->flush(); }
    void set_pattern(const std::string& pattern) override { inner_->set_pattern(pattern); }
    void set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) override { inner_->set_formatter(std::move(sink_formatter)); }
  private:
    /// @brief whether @p msg's own thread name starts with one of prefixes_
    [[nodiscard]] bool passes(const spdlog::details::log_msg& msg) const
    {
      const std::scoped_lock lock(mutex_);
      if (prefixes_.empty()) return true;

      // the payload is "<thread name>\x1f<message>" - anything without the
      // separator was not logged through _log() and has no name to match on,
      // so it is let through rather than silently dropped
      const std::string_view payload(msg.payload.data(), msg.payload.size());
      const auto             sep = payload.find(separator_);
      if (sep == std::string_view::npos) return true;

      const std::string_view name = payload.substr(0, sep);
      return std::ranges::any_of(prefixes_, [name](const std::string& prefix) { return name.starts_with(prefix); });
    }

    std::shared_ptr<spdlog::sinks::sink> inner_;
    char                                 separator_;
    mutable std::mutex                   mutex_; ///< guards prefixes_ - set from any thread, read on every record
    std::vector<std::string>             prefixes_;
  };
} // namespace logger
