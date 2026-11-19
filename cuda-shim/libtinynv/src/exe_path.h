// Where the running executable is, for the files a prebuilt release carries beside it. Header-only, because
// tools/tinynv_bmclear.c is built from its one source file and needs the same lookup as the library.
//
// THE RELEASE LAYOUT (cuda-shim/Makefile: make stage). For a program at <prefix>/bin/X:
//   <prefix>/bin/tinynv-bmclear, then <prefix>/libexec/macuda/tinynv-bmclear   the emergency bus-master clear
//   <prefix>/share/macuda/firmware                                              GSP firmware (nvidia/<chip>/gsp/...)
//   <prefix>/share/macuda/tinygpu-server.sh                                     the server script the helper starts through
// Every lookup goes: its environment override (terminal, even when it names nothing), then these paths relative to
// the executable, then the build tree's absolute path the Makefile baked in - so a developer build finds what it
// always found, and a release unpacked anywhere finds its own copies.
//
// Never from a signal handler: realpath and stat are not on the async-signal-safe list. The library resolves once,
// at open or init, and the crash path reads what was stored.
#ifndef TINYNV_EXE_PATH_H
#define TINYNV_EXE_PATH_H
// glibc declares readlink and PATH_MAX under -std=c11 only with a feature-test macro, and that has to come before the
// first system header of the source file - too late to set here, so say so instead of failing on readlink.
#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE) && !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#error "exe_path.h: #define _DEFAULT_SOURCE before the first #include of this source file (glibc: readlink, PATH_MAX)"
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdint.h>
#endif

// The directory of the running executable, symlinks resolved; 0, or -1 when it cannot be had.
static inline int tinynv_exe_dir(char *out, size_t n) {
  char real[PATH_MAX];
#ifdef __APPLE__
  char raw[PATH_MAX];
  uint32_t sz = sizeof(raw);
  if (_NSGetExecutablePath(raw, &sz) || !realpath(raw, real)) return -1;
#else
  ssize_t k = readlink("/proc/self/exe", real, sizeof(real) - 1);
  if (k <= 0) return -1;
  real[k] = 0;
#endif
  char *s = strrchr(real, '/');
  if (!s) return -1;
  if (s == real) s[1] = 0; else *s = 0;
  return snprintf(out, n, "%s", real) < (int)n ? 0 : -1;
}

// The prefix of a release layout: the executable's directory without its last component (bin/ -> <prefix>), or
// without "/libexec/macuda" for the helper, which may sit there.
static inline int tinynv_exe_prefix(const char *exe_dir, char *out, size_t n) {
  if (snprintf(out, n, "%s", exe_dir) >= (int)n) return -1;
  size_t l = strlen(out), sl = strlen("/libexec/macuda");
  if (l > sl && !strcmp(out + l - sl, "/libexec/macuda")) { out[l - sl] = 0; return 0; }
  char *s = strrchr(out, '/');
  if (!s) return -1;
  if (s == out) s[1] = 0; else *s = 0;
  return 0;
}

// a/b into out, or -1 when it does not fit: a cut path names some other file, so a caller drops that candidate
static inline int tinynv_join(char *out, size_t n, const char *a, const char *b) {
  int k = snprintf(out, n, "%s%s", a, b);
  return k >= 0 && (size_t)k < n ? 0 : -1;
}

static inline int tinynv_is_exec(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode) && !access(p, X_OK); }
static inline int tinynv_is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static inline int tinynv_is_readable(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode) && !access(p, R_OK); }
// The same file, however it was spelled: the baked default is $(abspath), which keeps symlinks; the executable's
// directory is a realpath. A developer build whose binary sits beside the baked path counts as the baked default.
static inline int tinynv_same_file(const char *a, const char *b) {
  struct stat x, y;
  return a && b && *a && *b && !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

#endif
