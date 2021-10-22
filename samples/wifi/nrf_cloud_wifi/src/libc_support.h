#ifndef _LIBC_SUPPORT_H_
#define _LIBC_SUPPORT_H_

#define TEST_LIBC

#if defined(CONFIG_SOC_ESP32)
double fabs(double x);

#if defined(CONFIG_EXTERNAL_LIBC)
int close(int fd);
int _close_r(void *r, int fd);

int fstat(int fd, void *sbuf);
int _fstat_r(void *r, int fd, void *sbuf);

_off_t lseek(int fd, _off_t size, int m);
_off_t _lseek_r(void *r, int fd, _off_t size, int m);

int read(int fd, char *buf, size_t len);
int _read_r(void *r, int fd, char *buf, size_t len);

int write(int fd, char *buf, size_t len);
int _write_r(void *r, int fd, char *buf, size_t len);

static int _stdout_hook_default(int c);
void __stdout_hook_install(int (*hook)(int val));

int pthread_setcancelstate(int state, int *oldstate);

struct _reent * __getreent(void);

int getpid(void);
int _getpid_r(void *r);

int kill(int pid, int sig);
int _kill_r(void *r, int pid, int sig);

int end(void);

#else
#define RAND_MAX 32768
int rand(void);
#endif
#endif

#if defined(TEST_LIBC)
void test_libc(void);
#endif

#endif
