# logger

A thin facade over [spdlog](https://github.com/gabime/spdlog) with some additional fnctionalities.

## main functionality

- **debug()/trace() compile out entirely in a release build.** Gated by `if constexpr
  (is_debug_build())` - not just filtered at runtime, the call site's formatting work is not
  present in the release binary at all. No macros.
- **active(level) lets a caller skip expensive work up front**, for the case where building the
  log message's *arguments* (not just formatting them) is the expensive part.
- **JSON configuration file**, loaded via `load_logger_config()`.
- **sync or async logging**, per `logger_config::run_mode`.
- **exception chain logging** and **terminate/signal handling**, with a captured `std::stacktrace`.
- **logical thread names**, independent of the OS thread id, printed via a custom `%*` pattern flag.
- **logs a caller's own structured error type** through `Logger::error<E>()`/`critical<E>()`,
  given any `E` with a `to_string()` - no dependency on a specific error type baked into the
  library. See `demo/error_info.hpp` for a worked example.

## integrating into another CMake project

Pull it in with [CPM.cmake](cmake/CPM.cmake) (the same tool this project uses for its own
dependencies) and link the `logger::logger` target:

```cmake
include(cmake/CPM.cmake)   # or your own copy of it
CPMAddPackage(
    NAME logger
    GITHUB_REPOSITORY ostri/logger
    GIT_TAG v0.1.0
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
"configuration file" below). It is neither copyable nor movable: pass it around by `const
Logger&`/`Logger&`, never by value.

```cpp
#include "logger/logger.hpp"
#include "logger/logger_config.hpp"

logger::logger_config cfg = logger::load_logger_config();
logger::Logger         log(cfg);
log.info("started, pid={}", getpid());
```

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

### sync vs async logging

`logger_config::run_mode` picks the mode. `sync` (the default): the calling thread writes to the
sinks itself, a call returns only once the message reached them. `async`: a background thread
pool does the writing via spdlog's `async_logger`, so a call returns immediately - at the cost of
log ordering across threads being best-effort. Pick `async` when the calling thread cannot afford
sink latency (a slow disk, a full terminal).

```cpp
logger::logger_config cfg{.run_mode = logger::mode::async, .app_name = "worker"};
logger::Logger         log(cfg);
log.info("this call returns without waiting for the write to land");
log.flush(); // wait for the background thread to catch up, e.g. before shutdown
```

### logging a caller-defined error type

`Logger::error<E>()`/`critical<E>()` accept any type `E` with a `std::string
E::to_string() const` - nothing about `E` is part of the logger library. See
`demo/error_info.hpp` for the type the demo program below uses.

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
parent can be empty string, but providing parent log names shows thread hierachy in log.

```cpp
logger::Logger::make_log_name("importer", "worker-3");
log.info("picked up next batch"); // prints [importer/worker-3] via a "%*" pattern
```

### exception chain and crash logging

```cpp
log.setup_terminate_handler(); // routes std::terminate through this logger
log.setup_signal_handler();    // logs SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGTERM with a stack trace

try { risky(); }
catch (const std::exception& e) { log.log_exception_with_chain(e); } // walks std::nested_exception too
```

Only one `Logger` per process should call `setup_terminate_handler()`/`setup_signal_handler()` -
the OS-level signal handler has no way to know which `Logger` instance to reach, so the last one
to call it wins.

## configuration file

`load_logger_config()` resolves a `logger_config` in this order:

1. the `LOG_CONFIG` environment variable, if set and non-empty - its value is a path to a JSON
   config file;
2. the path passed to `load_logger_config()` (defaults to `config/log.debug.json`, relative to
   the current working directory), if `LOG_CONFIG` was unset, empty, or unreadable;
3. a hardcoded fallback (console only, level `warn`) if neither file could be read or parsed.

It never throws - a missing or broken config file is a reason to fall back, not to fail startup.

```cpp
auto cfg = logger::load_logger_config();                  // LOG_CONFIG, then config/log.debug.json
auto cfg2 = logger::load_logger_config("conf/myapp.json"); // LOG_CONFIG, then this explicit path
```

```bash
LOG_CONFIG=/etc/myapp/logging.json ./myapp   # picks the file up from the environment
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
| `pattern`         | string | `"[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%*] %v"` | spdlog pattern string; `%*` is the custom flag for the logical thread name (see above)           |
| `log_folder`      | string | `"./logs"`                                   | directory the rotating file sink writes into (created if missing)                                |
| `flush_on`        | string | `"warn"`                                     | minimum level that triggers an immediate flush instead of a buffered write                       |

Level values (`console_level`, `file_level`, `flush_on`) accept `trace`, `debug`, `info`, `warn`
(or `warning`), `error` (or `err`), `critical`, `off`. An unrecognized value silently maps to
`off` - a typo in a config file disables that channel rather than failing to start.

Two example files ship in [config/](config/): `log.debug.json` (verbose, sync) and
`log.release.json` (quieter console, async, higher `file_level` than `console_level`).

## running the demo

`demo/demo.cpp` is a small program exercising every property above end to end: configuring from
a file, debug/trace elimination, `active()`, sync vs async, and a caller-defined error type.

```bash
cmake --preset ninja-debug
cmake --build build/debug -j8
cd build/debug && ./logger_demo
```

It reads `config/log.debug.json` relative to the current working directory (copied there by
CMake at configure time) and writes to `./logs/` - run it from the build directory, or copy
`config/` alongside the binary elsewhere. Building with the `ninja-release` preset and re-running
shows the debug/trace lines disappear from the output.

## running the tests

```bash
cmake --preset ninja-debug
cmake --build build/debug -j8
ctest --test-dir build/debug --output-on-failure
```

The test binary (`logger_test`) is only built when `BUILD_TESTING` is on, which is the default
for a standalone (top-level) build of this project.

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
- the report only covers `include/` and `src/` (the library itself) - `demo/` and `test/` are
  excluded, since coverage is a question about the library, not about the demo program or the
  test code measuring it;
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
