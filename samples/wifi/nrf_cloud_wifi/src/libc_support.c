#include <zephyr.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <random/rand32.h>

#include <logging/log.h>

#include "libc_support.h"

LOG_MODULE_DECLARE(esp32_wifi_sta, CONFIG_NRF_CLOUD_WIFI_SAMPLE_LOG_LEVEL);

#if defined(CONFIG_SOC_ESP32)

double fabs(double x)
{
	return x < 0 ? -x : x;
}

#if defined(CONFIG_EXTERNAL_LIBC)

int _close_r(void *r, int fd)
{
	return close(fd);
}

int _fstat_r(void *r, int fd, void *sbuf)
{
	return fstat(fd, sbuf);
}

_off_t _lseek_r(void *r, int fd, _off_t size, int m)
{
	return lseek(fd, size, m);
}

int _read_r(void *r, int fd, char *buf, size_t len)
{
	return read(fd, buf, len);
}

int _write_r(void *r, int fd, char *buf, size_t len)
{
	return write(fd, buf, len);
}

static int _stdout_hook_default(int c)
{
	(void)(c);  /* Prevent warning about unused argument */
	return EOF;
}

static int (*_stdout_hook)(int) = _stdout_hook_default;

void __stdout_hook_install(int (*hook)(int val))
{
	_stdout_hook = hook;
}

int pthread_setcancelstate(int state, int *oldstate)
{
	return -1;
}

struct _reent * __getreent(void)
{
	return NULL;
}

int _getpid_r(void *r)
{
	return getpid();
}

int _kill_r(void *r, int pid, int sig)
{
	return kill(pid, sig);
}

int end(void)
{
	return 0;
}
#else
int rand(void)
{
	return (int)sys_rand32_get() / (0xffffffff / (RAND_MAX - 1));
}

#endif /* CONFIG_EXTERNAL_LIBC */
#endif

#if defined(TEST_LIBC)
void test_libc(void)
{
	char *test_strtod = "3.14159";
	char *test_sscanf = "apple 12 47.5";
	char *test_json = "{\"appId\": \"HUMID\", \"messageType\": \"DATA\",\"data\": \"70.0\"}";
	float value;
	char tmp[100];
	int err;
	char *null_ptr;
	int num;

	LOG_INF("**************");
	value = strtod(test_strtod, &null_ptr);
	LOG_INF("strtod(%s) returned: %d.%03d", log_strdup(test_strtod),
		(int)value, (int)((value - (int)value) * 1000.0));

	memset(tmp, 0, sizeof(tmp));
	value = 0;
	num = 0;
	err = sscanf(test_sscanf, "%s %d %g", tmp, &num, &value);
	LOG_INF("sscanf() returned %d; string: %s, int:%d, double:%d.%03d",
		err, log_strdup(tmp), num, (int)value, (int)((value - (int)value) * 1000.0));

	LOG_INF("Parsing %s...", log_strdup(test_json));
	cJSON *root = cJSON_Parse(test_json);
	if (root) {
		char *out = cJSON_Print(root);
		if (out) {
			LOG_INF("Result: %s", log_strdup(out));
			/* crashes: cJSON_FreeString(out); */
		}
		cJSON *data_obj = cJSON_GetObjectItem(root, "data");
		if (data_obj) {
			char *out = cJSON_Print(data_obj);
			if (out) {
				LOG_INF("Data str: %s", log_strdup(out));
			}
			if (cJSON_IsNumber(data_obj)) {
				double data = cJSON_GetNumberValue(data_obj);
				LOG_INF("data: %d.%03d", 
					(int)data, (int)((data - (int)data) * 1000.0));
			}
		}

		LOG_INF("deleting object...");
		cJSON_Delete(root);
	}
	LOG_INF("**************");
}
#endif

