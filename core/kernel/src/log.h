// log.h — kernel log sink and small formatting helpers.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rlk {

using LogFn = void (*)(const char* line);
void SetLogSink(LogFn fn);
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogV(const char* fmt, va_list ap);

// JSON string escaping for status replies.
std::string JsonEscape(std::string_view s);

// Errno name for the strace-style log ("ENOENT"); "E<num>" if unknown.
const char* ErrnoName(int err);

// Kernel-quality random bytes (arc4random_buf on Darwin, getentropy elsewhere).
void FillRandom(void* buf, size_t len);

// "//a/./b/../c" -> "/a/c". Lexical only: symlinks are resolved by the VFS
// at lookup, so "link/.." differs from Linux (it takes the link's parent).
std::string NormalizePath(std::string_view path);

// Wall clock in milliseconds (gettimeofday).
double NowMs();

}  // namespace rlk
