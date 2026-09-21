/**
 * Structured logging. In Phase 0 this writes to stderr / Emscripten's
 * console bridge. Later phases route events through the trace buffer
 * (see docs/TRACE_FORMAT.md) for in-browser developer tooling.
 */
#ifndef SWITCH_COMMON_LOG_H
#define SWITCH_COMMON_LOG_H

/*
 * printf-style format-string checking. A GCC/Clang extension; MSVC has no
 * equivalent spelling in this position and rejects `__attribute__` as a
 * syntax error, so it expands to nothing there. Diagnostic-only: no effect
 * on ABI or behavior.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define SWITCH_PRINTF_FORMAT(format_index, first_arg_index) \
     __attribute__((format(printf, format_index, first_arg_index)))
#else
#  define SWITCH_PRINTF_FORMAT(format_index, first_arg_index)
#endif

typedef enum Log_Level
{
  LOG_LEVEL_TRACE = 0,
  LOG_LEVEL_DEBUG = 1,
  LOG_LEVEL_INFO = 2,
  LOG_LEVEL_WARN = 3,
  LOG_LEVEL_ERROR = 4,
} Log_Level;

void log_set_minimum_level(Log_Level level);

void log_message(Log_Level level, const char *format, ...)
    SWITCH_PRINTF_FORMAT(2, 3);

#define log_trace(...) log_message(LOG_LEVEL_TRACE, __VA_ARGS__)
#define log_debug(...) log_message(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define log_info(...) log_message(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_warn(...) log_message(LOG_LEVEL_WARN, __VA_ARGS__)
#define log_error(...) log_message(LOG_LEVEL_ERROR, __VA_ARGS__)

#endif /* SWITCH_COMMON_LOG_H */
