# logger

A thin facade over [spdlog](https://github.com/gabime/spdlog) with some additional fnctionalities.

## main functionality

- **debug()/trace() compile out entirely in a release build.** Gated by `if constexpr
  (is_debug_build())` - not just filtered at runtime, the call site's formatting work is not
  present in the release binary at all. No macros.
- **active(level) lets a caller skip expensive work up front**, for the case where building the
  log message's *arguments* (not just formatting them) is the expensive part.
- **JSON configuration file**, loaded via `load_logger_config()` - from `LOG_CONFIG`, or
  `"logger.conf"` in the current working directory by default, with a self-explanatory,
  ready-to-paste fallback message on stderr if neither is found.
- **sync or async logging**, per `logger_config::run_mode`.
- **exception chain logging** and **terminate/signal handling**, with a captured `std::stacktrace`.
- **logical thread names**, independent of the OS thread id, printed via a custom `%*` pattern flag.
- **logs a caller's own structured error type** through `Logger::error<E>()`/`critical<E>()`,
  given any `E` with a `to_string()` - no dependency on a specific error type baked into the
  library. See `src/demo/error_info.hpp` for a worked example.
- **levels adjustable at runtime**, per sink (console/file) or both at once, independent of what
  `logger_config` set them to at construction time.
- **fixed-width level names** in the log pattern, via the custom `%L` flag - lines up in a column
  regardless of level (`"info "`/`"warn "`/`"crit "`, space-padded to 5 characters), unlike
  spdlog's own `%l`.

## integrating into another CMake project

Pull it in with [CPM.cmake](cmake/CPM.cmake) (the same tool this project uses for its own
dependencies) and link the `logger::logger` target:

```cmake
include(cmake/CPM.cmake)   # or your own copy of it
CPMAddPackage(
    NAME logger
    GITHUB_REPOSITORY ostri/logger
    GIT_TAG v0.1.12
)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE logger::logger)
```

Plain `FetchContent` or a git submodule work identically - all that matters is that this
project's `CMakeLists.txt` runs as a subdirectory of yours (`add_subdirectory(<path>)`).

As a subdirectory, `BUILD_TESTING` and `ENABLE_SANITIZERS` default to off regardless of your own
project's settings (`PROJECT_IS_TOP_LEVEL` decides, not the option's own default) - pulling this
in does not build its test suite, its demo program, or sanitize your own targets. Set
`-DBUILD_TESTING=ON`/`-DENABLE_SANITIZERS=ON` explicitly before the `add_subdirectory()` call if
you want those anyway.

`logger::logger` links `stdc++exp` (GCC's `<stacktrace>` runtime, needed by exception-chain and
signal/terminate logging) publicly, so a consumer gets it transitively without asking.

## using each property

### constructing a Logger

A `Logger` is built from a `logger_config` - either populated by hand, or loaded from a file (see
"configuration file" below) - via `Logger::create()`, never a constructor directly: `Logger` is
not allowed to throw, so anything that can go wrong while setting up its sinks (an unwritable
`log_folder`, a rotating file it cannot open, ...) is reported through the returned
`std::expected` instead, already logged to stderr by `create()` itself. `Logger` is neither
copyable nor movable, so `create()` hands back a `std::unique_ptr<Logger>`, not a `Logger` by
value - pass the `Logger` itself around by `const Logger&`/`Logger&` from there on, never by value.

```cpp
#include "logger/logger.hpp"
#include "logger/logger_config.hpp"

logger::logger_config cfg      = logger::load_logger_config();
auto                   log_ptr = logger::Logger::create(cfg);
if (! log_ptr)
{
  // cfg's sinks could not be built (create() already logged why, to
  // stderr) - decide what that means for this program: abort startup,
  // fall back to a different logger_config, or something else entirely.
  return 1;
}
logger::Logger& log = **log_ptr; // everything below takes a Logger by reference, same as any other function would
log.info("started, pid={}", getpid());
```

The rest of this section (and the examples below it) assume a successfully constructed `log` -
either the `Logger&` from the snippet above, or any function parameter taking one.

`Logger::create_or_exit(cfg)` collapses the snippet above into one call, for the common case at
the top of `main()`: a program with nowhere left to report a broken logger prints `create()`'s
error and calls `exit(1)` instead of handing every caller the same check-print-exit dance to
repeat. It also calls `make_log_name(cfg.app_name)` on success, so the returned `Logger` is
immediately ready for `%*` to read its logical thread name.

```cpp
logger::logger_config cfg     = logger::load_logger_config();
auto                   log_ptr = logger::Logger::create_or_exit(cfg); // exits the process on failure
logger::Logger&        log     = *log_ptr;
```

Not for a library or a worker thread - those should still get to decide for themselves what "no
logger" means, which is exactly what `create()` (returning `std::expected` instead of exiting) is for.

### debug()/trace() elimination in release

`debug()`/`trace()` calls compile to nothing (`if constexpr (is_debug_build())`) once the project
is built with `NDEBUG` defined (a Release build). There is no runtime flag to set - the elimination
happens at compile time, so a call site's arguments are never even formatted in a release binary.

```cpp
log.debug("connection pool size={}", pool.size()); // present only in a debug build
log.trace("raw payload: {}", payload);              // same
```

### active(level): skip expensive work, not just formatting

Every `info`/`warn`/`error`/`critical` call already checks the configured level internally before
formatting - `active()` exists for when *building the arguments themselves* (not just turning them
into a string) is the expensive part, e.g. serializing a large structure only meant for a trace line.

```cpp
if (log.active(logger::level::trace))
  log.trace("full request dump: {}", expensive_serialize(request));
```

### reading and changing levels at runtime

`logger_config` only sets the starting point - `console_level()`/`file_level()`/`level()` read
back what a `Logger` is currently doing, and `set_console_level()`/`set_file_level()`/`set_level()`
change it, independent of each other: the console sink and the file sink can run at different
levels, and `level()` (the one `active()` checks) is the more permissive of the two. `flush_on(l)`
moves the level at which a write triggers an immediate flush, same knob as `logger_config::flush_on`
but adjustable after construction.

```cpp
log.set_console_level(logger::level::error); // quiet down the console, keep the file at whatever it was
log.set_file_level(logger::level::debug);
log.flush_on(logger::level::error);          // flush immediately from error upward
```

### finding the file sink's actual path

`logger_config::log_folder` + `app_name` name a base path (e.g. `"logs/ach"`), not the file
actually being written to - spdlog's own daily rotation appends a `_YYYY-MM-DD` suffix that
changes at `rotation_hour:rotation_minute` every day the process stays up. `log_filename()`
returns that real, current path, without a caller having to duplicate spdlog's own date-naming
rule to predict it (e.g. to truncate/delete "this Logger's own log file" from a maintenance
command).

```cpp
const std::string path = log.log_filename(); // e.g. "logs/ach_2026-08-16.log"
```

### sync vs async logging

`logger_config::run_mode` picks the mode. `sync` (the default): the calling thread writes to the
sinks itself, a call returns only once the message reached them. `async`: a background thread
pool does the writing via spdlog's `async_logger`, so a call returns immediately - at the cost of
log ordering across threads being best-effort. Pick `async` when the calling thread cannot afford
sink latency (a slow disk, a full terminal).

```cpp
logger::logger_config cfg{.run_mode = logger::mode::async, .app_name = "worker"};
auto                   log_ptr = logger::Logger::create(cfg).value(); // see "constructing a Logger" above for error handling
logger::Logger&        log     = *log_ptr;
log.info("this call returns without waiting for the write to land");
log.flush(); // wait for the background thread to catch up, e.g. before shutdown
```

### logging a caller-defined error type

`Logger::error<E>()`/`critical<E>()` accept any type `E` with a `std::string
E::to_string() const` - nothing about `E` is part of the logger library. See
`src/demo/error_info.hpp` for the type the demo program below uses.

```cpp
struct my_error
{
  std::string reason;
  [[nodiscard]] std::string to_string() const { return "my_error: " + reason; }
};

log.error(my_error{.reason = "connection refused"});
```

### logical thread names

`Logger::make_log_name(parent, child = "")` sets a `thread_local` name that a `%*` pattern flag
prints - useful for tagging log lines by worker stage rather than an OS thread id that
means nothing to a reader. Call it once per thread, early (e.g. right after a worker thread
starts).
`parent` can be an empty string, but providing it shows thread hierarchy in the log. `Logger::log_name()`
reads back whatever the current thread's name is currently set to.

```cpp
logger::Logger::make_log_name("importer", "worker-3");
log.info("picked up next batch");         // prints [importer/worker-3] via a "%*" pattern
logger::Logger::log_name();               // -> "importer/worker-3"
```

### exception chain and crash logging

```cpp
log.setup_terminate_handler(); // routes std::terminate through this logger
log.setup_signal_handler();    // logs SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGTERM with a stack trace

try { risky(); }
catch (const std::exception& e) { log.log_exception_with_chain(e); } // walks std::nested_exception too

catch (...) { log.log_current_exception_with_chain(); } // same, for the exception currently being handled
```

Both take an optional `enum level` (default `level::critical`) to log at instead.

Only one `Logger` per process should call `setup_terminate_handler()`/`setup_signal_handler()` -
the OS-level signal handler has no way to know which `Logger` instance to reach, so the last one
to call it wins.

## configuration file

`load_logger_config()` resolves a `logger_config` in this order:

1. the `LOG_CONFIG` environment variable, if set and non-empty - its value is a path to a JSON
   config file;
2. the path passed to `load_logger_config()` (defaults to `def_logger_cfg_path`, i.e.
   `"logger.conf"` in the current working directory), if `LOG_CONFIG` was unset, empty, or
   unreadable;
3. a hardcoded fallback (console only, level `warn`) if neither file could be read or parsed.

It never throws - a missing or broken config file is a reason to fall back, not to fail startup.
Configuration is always available through one of two shapes: a **file name** (`LOG_CONFIG` or the
`config_path` argument, both resolved by `load_logger_config()` itself) or an already-populated
**`logger_config` struct**, built by hand or via `parse_logger_config()` from JSON text already in
hand. Only the file-name path involves reading anything off disk, so only it can fail with a
syntax error - see "syntax errors" below.

```cpp
auto cfg = logger::load_logger_config();                  // LOG_CONFIG, then ./logger.conf
auto cfg2 = logger::load_logger_config("conf/myapp.json"); // LOG_CONFIG, then this explicit path
auto cfg3 = logger::logger_config{.app_name = "myapp"};    // no file at all - built by hand
```

```bash
LOG_CONFIG=/etc/myapp/logging.json ./myapp   # picks the file up from the environment
```

### falling back to the console

When step 3 above is reached - neither `LOG_CONFIG` nor `config_path` led to a readable config
file - `load_logger_config()` prints an explanation to stderr before returning the hardcoded
fallback: that it looked for `LOG_CONFIG` and for `config_path` in the current working directory
and found neither, that it is logging to the console only until one becomes available, and a
ready-to-paste JSON rendering of the `logger_config` actually in effect:

```text
No logger configuration found - looked for the LOG_CONFIG environment variable and for
'logger.conf' in the current working directory. Logging to the console only, until one of those
is available. Point LOG_CONFIG at a JSON file, or place one at 'logger.conf', with content like:
{
  "app_name": "app",
  "console_level": "warn",
  "file_level": "warn",
  ...
}
```

### syntax errors

If `LOG_CONFIG` or `config_path` *does* point at a file that exists but fails to parse - malformed
JSON, or a field of the wrong type - `load_logger_config()` prints nlohmann::json's own error
(message, line and column) to stderr and falls through to the next step, same as a missing file:

```text
LOG_CONFIG='/etc/myapp/logging.json' does not parse as valid JSON: [json.exception.parse_error.101]
parse error at line 3, column 5: syntax error while parsing value - falling back.
```

This only applies to the file-name path (`LOG_CONFIG`/`config_path`) - a `logger_config` built by
hand, or `parse_logger_config()`'s already-in-hand JSON text, has nothing to open and so nothing
that can fail this way (malformed text there just falls back to `defaults`, silently, since it was
never a file in the first place).

`parse_logger_config(json_text, defaults = {})` covers the case where a JSON object is already in
hand rather than on disk - e.g. a `"log"` section pulled out of a larger config file already
parsed with `nlohmann::json`. It uses the same field mapping as `load_logger_config()`'s own
file-based path, but is expressed as raw JSON text rather than a parsed object, so this header
never needs to name `nlohmann::json` itself - that dependency stays private to logger's `.cpp`
side. Malformed text, or a value that is not an object, falls back to `defaults` entirely; any
field `json_text` does not mention falls back to `defaults` individually - every field is optional,
same as in a JSON config file.

```cpp
nlohmann::json full_config = /* ... */;
auto            cfg        = logger::parse_logger_config(full_config.at("log").dump());
```

### settings

| key               | type   | default                                      | meaning                                                                                          |
| ----------------- | ------ | -------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `app_name`        | string | `"app"`                                      | used to build the log file name (`<app_name>_YYYY-MM-DD.log`) and as spdlog's logger name (`%n`) |
| `mode`            | string | `"sync"`                                     | `"sync"`: the calling thread writes to sinks directly. `"async"`: a background thread pool does  |
| `console_level`   | string | `"warn"`                                     | minimum level printed to the console sink                                                        |
| `file_level`      | string | `"trace"`                                    | minimum level written to the rotating file sink                                                  |
| `rotation_hour`   | int    | `2`                                          | hour of day (0-23) the log file rotates                                                          |
| `rotation_minute` | int    | `0`                                          | minute of the rotation hour                                                                      |
| `keep_days`       | int    | `7`                                          | how many rotated log files are kept before the oldest is deleted                                 |
| `pattern`         | string | `"[%Y-%m-%d %H:%M:%S.%e] [%n] [%L] [%*] %v"` | spdlog pattern string; `%L` and `%*` are this library's own custom flags (see below)             |
| `log_folder`      | string | `"./logs"`                                   | directory the rotating file sink writes into (created if missing)                                |
| `flush_on`        | string | `"warn"`                                     | minimum level that triggers an immediate flush instead of a buffered write                       |

Level values (`console_level`, `file_level`, `flush_on`) accept `trace`, `debug`, `info`, `warn`
(or `warning`), `error` (or `err`), `critical`, `off`. An unrecognized value silently maps to
`off` - a typo in a config file disables that channel rather than failing to start.

`%L` (fixed 5-character level name, e.g. `"info "`/`"warn "`/`"crit "`) and `%*` (logical thread
name, see "logical thread names" below) are custom pattern flags this library registers on top of
spdlog's own - usable in `pattern` exactly like any built-in spdlog flag (`%l` remains available
too, but does not line up in a column across levels).

One example file per build type ships in [config/](config/) - `log.debug.conf`, `log.release.conf`,
`log.profile.conf`, `log.coverage.conf` - and CMake copies the one matching `CMAKE_BUILD_TYPE`
into that build directory as `logger.conf` (see "running the demo" below), so a binary run from
its own build directory picks up the right settings with no `LOG_CONFIG` needed. `log.debug.conf`
and `log.coverage.conf` are verbose and sync (`file_level: trace`); `log.release.conf` and
`log.profile.conf` are quieter and async, with a higher `file_level` than `console_level`.

## running the demo

`src/demo/demo.cpp` is a small program exercising every property above end to end: configuring from
a file, debug/trace elimination, `active()`, sync vs async, and a caller-defined error type.

```bash
cmake --preset ninja-debug
cmake --build build/debug -j8
cd build/debug && ./logger_demo
```

It reads `./logger.conf` relative to the current working directory - CMake copies
`config/log.debug.conf` there at configure time (see "settings" above) - and writes to `./logs/`;
run it from the build directory, or copy `logger.conf` alongside the binary elsewhere. Building
with the `ninja-release` preset and re-running shows the debug/trace lines disappear from the
output, and picks up `log.release.conf`'s settings instead.

## running the tests

```bash
cmake --preset ninja-debug
cmake --build build/debug -j8
ctest --test-dir build/debug --output-on-failure
```

or, equivalently, via the matching test preset (also available for `release`/`profile`):

```bash
ctest --preset debug
```

The test binary (`logger_test`) is only built when `BUILD_TESTING` is on, which is the default
for a standalone (top-level) build of this project.

`signal_terminate_helper` is a second, plain (Catch2-free) executable built alongside
`logger_test` - `setup_signal_handler()`/`setup_terminate_handler()` end in `std::exit()`/
`std::abort()`, which the Catch2 binary can't survive calling directly (Catch2 installs its own
fatal-condition handler for the same signals). Tests for those two exercise this helper via
fork()/exec() instead and only observe its exit status/log output - see
[test/helper/signal_terminate_helper.cpp](test/helper/signal_terminate_helper.cpp).

### dependencies

fmt, spdlog and nlohmann_json are fetched by [CPM.cmake](cmake/CPM.cmake); Catch2 too, when
`BUILD_TESTING` is on. No system packages needed.

```bash
export CPM_SOURCE_CACHE=$HOME/.cache/CPM
```

worth putting in `~/.bashrc` so every build directory shares one download.

## coverage

A coverage build instruments `logger` (the library, not the demo or the test code itself) with
gcov, runs `logger_test`, and renders an HTML report with [gcovr](https://gcovr.com/) - the
result is what's linked below.

```bash
cmake --preset ninja-coverage
cmake --build build/coverage --target coverage
```

**[open the coverage report](build/coverage/coverage/index.html)** once you've run the command
above - it is a local file path, not hosted anywhere, so the link only resolves after a coverage
build has actually produced it.

A few things worth knowing about this target:

- it requires GCC (`CXX=g++`) - gcov is GCC's own instrumentation format, and this isn't wired up
  for Clang's equivalent (`llvm-cov`);
- it is mutually exclusive with `ENABLE_SANITIZERS` - gcov instrumentation and ASan both hook
  allocation/branch behavior, so `ENABLE_COVERAGE=ON` takes precedence if both are somehow
  requested at once;
- the report only covers `include/` and `src/` (the library itself), minus `src/demo/` - `test/`
  is excluded too, since coverage is a question about the library, not about the demo program or
  the test code measuring it;
- `ninja coverage` (or the `coverage` build preset target above) can be re-run at any time and
  regenerates the report from a fresh test run.

## open source software used in this project

| component                                           | purpose                                                                              |
| --------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [spdlog](https://github.com/gabime/spdlog)          | the logging backend this project wraps (sinks, formatting, async logging)            |
| [fmt](https://github.com/fmtlib/fmt)                | string formatting, used both by spdlog and by this project's own public API          |
| [nlohmann/json](https://github.com/nlohmann/json)   | parsing the JSON configuration file                                                  |
| [Catch2](https://github.com/catchorg/Catch2)        | the test framework `logger_test` is written against                                  |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | fetches the above dependencies at configure time, no system packages needed          |
| [gcovr](https://gcovr.com/)                         | renders the coverage build's gcov output into the HTML report (see "coverage" below) |
