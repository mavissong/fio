/*
 * Clock functions
 */

#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "fio.h"
#include "smalloc.h"

#include "hash.h"

#ifdef ARCH_HAVE_CPU_CLOCK
static unsigned long cycles_per_usec;
static unsigned long last_cycles;
#endif
static struct timeval last_tv;
static int last_tv_valid;

enum fio_cs fio_clock_source = FIO_PREFERRED_CLOCK_SOURCE;

#ifdef FIO_DEBUG_TIME

#define HASH_BITS	8
#define HASH_SIZE	(1 << HASH_BITS)

static struct flist_head hash[HASH_SIZE];
static int gtod_inited;

struct gtod_log {
	struct flist_head list;
	void *caller;
	unsigned long calls;
};

static struct gtod_log *find_hash(void *caller)
{
	unsigned long h = hash_ptr(caller, HASH_BITS);
	struct flist_head *entry;

	flist_for_each(entry, &hash[h]) {
		struct gtod_log *log = flist_entry(entry, struct gtod_log,
									list);

		if (log->caller == caller)
			return log;
	}

	return NULL;
}

static struct gtod_log *find_log(void *caller)
{
	struct gtod_log *log = find_hash(caller);

	if (!log) {
		unsigned long h;

		log = malloc(sizeof(*log));
		INIT_FLIST_HEAD(&log->list);
		log->caller = caller;
		log->calls = 0;

		h = hash_ptr(caller, HASH_BITS);
		flist_add_tail(&log->list, &hash[h]);
	}

	return log;
}

static void gtod_log_caller(void *caller)
{
	if (gtod_inited) {
		struct gtod_log *log = find_log(caller);

		log->calls++;
	}
}

static void fio_exit fio_dump_gtod(void)
{
	unsigned long total_calls = 0;
	int i;

	for (i = 0; i < HASH_SIZE; i++) {
		struct flist_head *entry;
		struct gtod_log *log;

		flist_for_each(entry, &hash[i]) {
			log = flist_entry(entry, struct gtod_log, list);

			printf("function %p, calls %lu\n", log->caller,
								log->calls);
			total_calls += log->calls;
		}
	}

	printf("Total %lu gettimeofday\n", total_calls);
}

static void fio_init gtod_init(void)
{
	int i;

	for (i = 0; i < HASH_SIZE; i++)
		INIT_FLIST_HEAD(&hash[i]);

	gtod_inited = 1;
}

#endif /* FIO_DEBUG_TIME */

#ifdef FIO_DEBUG_TIME
void fio_gettime(struct timeval *tp, void *caller)
#else
void fio_gettime(struct timeval *tp, void fio_unused *caller)
#endif
{
#ifdef FIO_DEBUG_TIME
	if (!caller)
		caller = __builtin_return_address(0);

	gtod_log_caller(caller);
#endif
	if (fio_tv) {
		memcpy(tp, fio_tv, sizeof(*tp));
		return;
	}

	switch (fio_clock_source) {
	case CS_GTOD:
		gettimeofday(tp, NULL);
		break;
	case CS_CGETTIME: {
		struct timespec ts;

#ifdef FIO_HAVE_CLOCK_MONOTONIC
		if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
#else
		if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
#endif
			log_err("fio: clock_gettime fails\n");
			assert(0);
		}

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		break;
		}
#ifdef ARCH_HAVE_CPU_CLOCK
	case CS_CPUCLOCK: {
		unsigned long long usecs, t;

		t = get_cpu_clock();
		if (t < last_cycles) {
			dprint(FD_TIME, "CPU clock going back in time\n");
			t = last_cycles;
		}

		usecs = t / cycles_per_usec;
		tp->tv_sec = usecs / 1000000;
		tp->tv_usec = usecs % 1000000;
		last_cycles = t;
		break;
		}
#endif
	default:
		log_err("fio: invalid clock source %d\n", fio_clock_source);
		break;
	}

	/*
	 * If Linux is using the tsc clock on non-synced processors,
	 * sometimes time can appear to drift backwards. Fix that up.
	 */
	if (last_tv_valid) {
		if (tp->tv_sec < last_tv.tv_sec)
			tp->tv_sec = last_tv.tv_sec;
		else if (last_tv.tv_sec == tp->tv_sec &&
			 tp->tv_usec < last_tv.tv_usec)
			tp->tv_usec = last_tv.tv_usec;
	}
	last_tv_valid = 1;
	memcpy(&last_tv, tp, sizeof(*tp));
}

#ifdef ARCH_HAVE_CPU_CLOCK
static unsigned long get_cycles_per_usec(void)
{
	struct timeval s, e;
	unsigned long long c_s, c_e;

	gettimeofday(&s, NULL);
	c_s = get_cpu_clock();
	do {
		unsigned long long elapsed;

		gettimeofday(&e, NULL);
		elapsed = utime_since(&s, &e);
		if (elapsed >= 10) {
			c_e = get_cpu_clock();
			break;
		}
	} while (1);

	return c_e - c_s;
}

static void calibrate_cpu_clock(void)
{
	double delta, mean, S;
	unsigned long avg, cycles[10];
	int i, samples;

	cycles[0] = get_cycles_per_usec();
	S = delta = mean = 0.0;
	for (i = 0; i < 10; i++) {
		cycles[i] = get_cycles_per_usec();
		delta = cycles[i] - mean;
		if (delta) {
			mean += delta / (i + 1.0);
			S += delta * (cycles[i] - mean);
		}
	}

	S = sqrt(S / (10 - 1.0));

	samples = avg = 0;
	for (i = 0; i < 10; i++) {
		double this = cycles[i];

		if ((fmax(this, mean) - fmin(this, mean)) > S)
			continue;
		samples++;
		avg += this;
	}

	S /= 10.0;
	mean /= 10.0;

	for (i = 0; i < 10; i++)
		dprint(FD_TIME, "cycles[%d]=%lu\n", i, cycles[i] / 10);

	avg /= (samples * 10);
	dprint(FD_TIME, "avg: %lu\n", avg);
	dprint(FD_TIME, "mean=%f, S=%f\n", mean, S);

	cycles_per_usec = avg;

}
#else
static void calibrate_cpu_clock(void)
{
}
#endif

void fio_clock_init(void)
{
	last_tv_valid = 0;
	calibrate_cpu_clock();
}

unsigned long long utime_since(struct timeval *s, struct timeval *e)
{
	long sec, usec;
	unsigned long long ret;

	sec = e->tv_sec - s->tv_sec;
	usec = e->tv_usec - s->tv_usec;
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	/*
	 * time warp bug on some kernels?
	 */
	if (sec < 0 || (sec == 0 && usec < 0))
		return 0;

	ret = sec * 1000000ULL + usec;

	return ret;
}

unsigned long long utime_since_now(struct timeval *s)
{
	struct timeval t;

	fio_gettime(&t, NULL);
	return utime_since(s, &t);
}

unsigned long mtime_since(struct timeval *s, struct timeval *e)
{
	long sec, usec, ret;

	sec = e->tv_sec - s->tv_sec;
	usec = e->tv_usec - s->tv_usec;
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	if (sec < 0 || (sec == 0 && usec < 0))
		return 0;

	sec *= 1000UL;
	usec /= 1000UL;
	ret = sec + usec;

	return ret;
}

unsigned long mtime_since_now(struct timeval *s)
{
	struct timeval t;
	void *p = __builtin_return_address(0);

	fio_gettime(&t, p);
	return mtime_since(s, &t);
}

unsigned long time_since_now(struct timeval *s)
{
	return mtime_since_now(s) / 1000;
}
