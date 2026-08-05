#pragma once
/**
 * @file
 * @brief compile-time build type (debug/release), used to gate debug()/trace() out of release
 */

#include <cstdint>

namespace logger
{
  /// @brief current build configuration
  enum class build_type_enum : std::uint8_t
  {
    debug,
    release
  };

#ifndef NDEBUG
  /// @brief current build type (compile-time constant)
  constexpr static const build_type_enum build_type = build_type_enum::debug;
#else
  constexpr static const build_type_enum build_type = build_type_enum::release;
#endif

  /**
   * @brief true for a debug build (NDEBUG not defined), consteval so it also
   * gates `if constexpr` - the branch not taken is not compiled at all, not
   * just skipped at runtime
   */
  consteval bool is_debug_build() { return build_type == build_type_enum::debug; }

  /// @brief "debug" or "release", for log messages/filenames that want it spelled out
  consteval const char* build_type_name() { return is_debug_build() ? "debug" : "release"; }
} // namespace logger
