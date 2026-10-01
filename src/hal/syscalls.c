// newlib system call stubs (_write is in console.c). Nothing here uses stdio files; providing them avoids nosys.specs'
// link-time "is not implemented and will always fail" warnings.
#include <errno.h>
#include <sys/stat.h>

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _fstat(int fd, struct stat* st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int dir) { (void)fd; (void)off; (void)dir; return 0; }
int _read(int fd, char* p, int n) { (void)fd; (void)p; (void)n; return 0; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
