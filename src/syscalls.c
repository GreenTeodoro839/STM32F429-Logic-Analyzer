// Minimal newlib system call stubs. Only snprintf is used, so there is no
// real file I/O; defining these replaces the warning-emitting nosys versions.
#include <errno.h>
#include <sys/stat.h>

int _close(int fd) {
  (void)fd;
  errno = EBADF;
  return -1;
}

int _lseek(int fd, int offset, int whence) {
  (void)fd;
  (void)offset;
  (void)whence;
  return 0;
}

int _read(int fd, char *buf, int len) {
  (void)fd;
  (void)buf;
  (void)len;
  return 0;
}

int _write(int fd, const char *buf, int len) {
  (void)fd;
  (void)buf;
  return len;
}
