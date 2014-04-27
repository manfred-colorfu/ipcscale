/*
 * sem-scalebench.cpp - sysv scaling test
 *
* Copyright (C) 1999, 2001, 2005, 2008, 2013 by Manfred Spraul.
 *	All rights reserved except the rights granted by the GPL.
 *
 * Redistribution of this file is permitted under the terms of the GNU 
 * General Public License (GPL) version 3 or later.
 */

/*
 * The file supports multiple operating modes:
 * - WAIT_FOR_ZERO: Check that a semaphore value that is 0 is really 0.
 *   Each thread has it's own semaphore value.
 *   This problem can scale 100% linear.
 *   For Linux, it does scale linear, at least to 8 sockets/80 cores.
 * - Check that a semaphore value that is 0 is readlly 0.
 *   Multiple threads share the same semaphore.
 *   Not yet implemented.
 * - PING_PONG: Each thread has it's own semaphore, but it needs to be
 *   returned by a partner thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#define SEM_SCALEBENCH_VERSION	"0.20"

#ifdef __sun
	 #include <sys/pset.h> /* P_PID, processor_bind() */
#endif

#define VERBOSE_DEBUG	2
#define	VERBOSE_NORMAL	1
#define VERBOSE_OFF	0

int g_verbose = 0;

//////////////////////////////////////////////////////////////////////////////

#define DELAY_BUBBLESORT

#ifdef DELAY_LOOP
#define DELAY_ALGORITHM	"integer divisions"

#define DELAY_LOOPS	20

static volatile int g_numerator = 12345678;
static volatile int g_denominator = 123456;

unsigned long long do_delay(int loops)
{
	unsigned long long sum;
	int i, j;

	sum = loops;
	for (i=0;i<loops;i++) {
		for (j=0;j<DELAY_LOOPS;j++) {
			sum += g_numerator/g_denominator;
		}
	}
	return sum;
}

#elif defined (DELAY_BUBBLESORT)

#define DELAY_ALGORITHM	"bubblesort"

#define BUF_SIZE	12
static volatile int g_BUF_SIZE	= BUF_SIZE;
int do_delay(int loops)
{
	int sum;
	int data[64];
	int i, j, k;

	sum = 0;
	for (i=0;i<loops;i++) {
		/* init with reverse order */
		for(j=0;j<g_BUF_SIZE;j++)
			data[j]=g_BUF_SIZE-j;

		for(j=g_BUF_SIZE;j>1;j=j-1) {
			for(k=0;k<j;k++) {
				if (data[k] > data[k+1]) {
					int tmp;
					tmp = data[k];
					data[k] = data[k+1];
					data[k+1] = tmp;
				}
			}
		}
		sum = sum + data[0];
	}
	return sum;
}
#else

#error Unknown delay operation

#endif

//////////////////////////////////////////////////////////////////////////////

#define DELAY_10MS	(10000)

static enum {
	WAITING,
	RUNNING,
	STOPPED,
} volatile g_state = WAITING;

struct tres {
	unsigned long long ops;
	struct rusage ru;
};

struct tres *g_results;
int g_svsem_id;
int g_max_cpus;
int g_sem_distance = 1;
int g_threadspercore = 1;
int *g_svsem_nrs;
pthread_t *g_threads;

struct taskinfo {
	int svsem_id;
	int threadid;
	int interleave;
	int delay;
};

//////////////////////////////////////////////////////////////////////////////

#define WAIT_FOR_ZERO	1

unsigned long long wait_for_zero(struct taskinfo *ti)
{
	unsigned long long rounds = 0;
	int sem_own;
	int ret;

	sem_own = g_svsem_nrs[ti->threadid];

	if (g_verbose >= VERBOSE_NORMAL) {
		printf("thread %d: wait-for-zero, sema %8d\n",ti->threadid,
				sem_own);
	}

	while(g_state == RUNNING) {
		struct sembuf sop[1];

		/* 1) check if the semaphore value is 0 */
		sop[0].sem_num=sem_own;
		sop[0].sem_op=0;
		sop[0].sem_flg=0;
		ret = semop(g_svsem_id,sop,1);
		if (ret != 0) {
			/* EIDRM can happen */
			if (errno == EIDRM)
				break;

			printf("main semop failed, ret %d errno %d.\n", ret, errno);

			/* Some OS do not report EIDRM properly */
			if (g_state != RUNNING)
				break;
			printf(" round %lld sop: num %d op %d flg %d.\n",
					rounds,
					sop[0].sem_num, sop[0].sem_op, sop[0].sem_flg);
			fflush(stdout);
			exit(1);
		}
		if (ti->delay)
			do_delay(ti->delay);
		rounds++;
	}
	return rounds;
}

//////////////////////////////////////////////////////////////////////////////

#define PING_PONG 2

unsigned long long ping_pong(struct taskinfo *ti)
{
	unsigned long long rounds = 0;
	bool sender;
	int sem_own;
	int sem_partner;
	int ret;

	sender = ti->threadid % 2;
	sem_own = g_svsem_nrs[ti->threadid];
	sem_partner = g_svsem_nrs[ti->threadid + 1 - 2*(ti->threadid%2)];

	if (g_verbose >= VERBOSE_NORMAL) {
		printf("thread %d: ping-pong, sema %8d, partner %8d, sender %d\n",ti->threadid,
				sem_own, sem_partner, sender);
	}

	if (sender) {
		struct sembuf sop[1];

		/* 1) insert token */
		sop[0].sem_num=sem_own;
		sop[0].sem_op=1;
		sop[0].sem_flg=0;
		ret = semop(g_svsem_id,sop,1);
	
		if (ret != 0) {
			printf("Initial semop failed, ret %d, errno %d.\n", ret, errno);
			exit(1);
		}
	}

	while(g_state == RUNNING) {
		struct sembuf sop[1];

		/* 1) decrease the own semaphore */
		sop[0].sem_num=sem_own;
		sop[0].sem_op=-1;
		sop[0].sem_flg=0;
		ret = semop(g_svsem_id,sop,1);
		if (ret != 0) {
			/* EIDRM can happen */
			if (errno == EIDRM)
				break;

			printf("main semop failed, ret %d errno %d.\n", ret, errno);

			/* Some OS do not report EIDRM properly */
			if (g_state != RUNNING)
				break;
			printf(" round %lld sop: num %d op %d flg %d.\n",
					rounds,
					sop[0].sem_num, sop[0].sem_op, sop[0].sem_flg);
			fflush(stdout);
			exit(1);
		}
		if (ti->delay)
			do_delay(ti->delay);
		rounds++;

		/* 2) increase the partner's semaphore */
		sop[0].sem_num=sem_partner;
		sop[0].sem_op=1;
		sop[0].sem_flg=0;
		ret = semop(g_svsem_id,sop,1);
		if (ret != 0) {
			/* EIDRM can happen */
			if (errno == EIDRM)
				break;

			printf("main semop failed, ret %d errno %d.\n", ret, errno);

			/* Some OS do not report EIDRM properly */
			if (g_state != RUNNING)
				break;
			printf(" round %lld sop: num %d op %d flg %d.\n",
					rounds,
					sop[0].sem_num, sop[0].sem_op, sop[0].sem_flg);
			fflush(stdout);
			exit(1);
		}
		if (ti->delay)
			do_delay(ti->delay);

		rounds++;
	}
	return rounds;
}

//////////////////////////////////////////////////////////////////////////////

int g_operation = WAIT_FOR_ZERO;


int get_cpunr(int cpunr, int interleave)
{
	int off = 0;
	int ret = 0;

	if (g_verbose >= VERBOSE_DEBUG) {
		printf("get_cpunr %p: cpunr %d max_cpu %d interleave %d threadspercore %d.\n",
			(void*)pthread_self(), cpunr, g_max_cpus, interleave, g_threadspercore);
	}

	while (cpunr > g_threadspercore - 1) {
		ret += interleave;
		if (ret >=g_max_cpus) {
			off++;
			ret = off;
		}
		cpunr -= g_threadspercore;
	}
	if (g_verbose >= VERBOSE_DEBUG) {
		printf("get_cpunr %p: result %d.\n", (void*)pthread_self(), ret);
	}

	return ret;
}

void bind_cpu(int cpunr)
{
	int ret;
#if __sun
	ret = processor_bind(P_PID, getpid(), cpunr, NULL);
	if (ret == -1) {
		perror("bind_thread:processor_bind");
		printf(" Binding to cpu %d failed.\n", cpunr);
	}
#else
	cpu_set_t cpus;
	cpu_set_t v;
	CPU_ZERO(&cpus);
	CPU_SET(cpunr, &cpus);
	pthread_t self;

	self = pthread_self();

	ret = pthread_setaffinity_np(self, sizeof(cpus), &cpus);
	if (ret < 0) {
		printf("pthread_setaffinity_np failed for thread %p with errno %d.\n",
				(void*)self, errno);
	}

	ret = pthread_getaffinity_np(self, sizeof(v), &v);
	if (ret < 0) {
		printf("pthread_getaffinity_np() failed for thread %p with errno %d.\n",
				(void*)self, errno);
		fflush(stdout);
	}
	if (memcmp(&v, &cpus, sizeof(cpus) != 0)) {
		printf("Note: Actual affinity does not match intention: got 0x%08lx, expected 0x%08lx.\n",
			(unsigned long)v.__bits[0], (unsigned long)cpus.__bits[0]);
	}
	fflush(stdout);
#endif
}

void* worker_thread(void *arg)
{
	struct taskinfo *ti = (struct taskinfo*)arg;
	unsigned long long rounds;
	int cpu = get_cpunr(ti->threadid, ti->interleave);

	bind_cpu(cpu);
	if (g_verbose >= VERBOSE_NORMAL) {
		printf("thread %d: bound to cpu %d\n",ti->threadid, cpu);
	}
	
	while(g_state == WAITING) {
#ifdef __GNUC__
#if defined(__i386__) || defined (__x86_64__)
		__asm__ __volatile__("pause": : :"memory");
#else
		__asm__ __volatile__("": : :"memory");
#endif
#endif
	}
	if (g_operation == WAIT_FOR_ZERO) {
		rounds = wait_for_zero(ti);
	} else if (g_operation == PING_PONG) {
		rounds = ping_pong(ti);
	} else {
		printf("Unknown operation.\n");
		fflush(stdout);
		exit(1);
	}

	g_results[ti->threadid].ops = rounds;
	if (getrusage(RUSAGE_THREAD, &g_results[ti->threadid].ru)) {
		printf("thread %p: getrusage failed, errno %d.\n",
			(void*)pthread_self(), errno);
	}

	pthread_exit(0);
	return NULL;
}

void init_threads(int cpu, int cpus, int delay, int interleave)
{
	int ret;
	struct taskinfo *ti;

	ti = (struct taskinfo*)malloc(sizeof(struct taskinfo));
	if (!ti) {
		printf("Could not allocate task info\n");
		exit(1);
	}
	if (cpu == 0) {
		int i;
		g_svsem_id = semget(IPC_PRIVATE,
				g_sem_distance*cpus,0777|IPC_CREAT);
		if(g_svsem_id == -1) {
			printf("sem array create failed.\n");
			exit(1);
		}
		for (i=0;i<cpus;i++)
			g_svsem_nrs[i] = g_sem_distance - 1 +
						g_sem_distance*i;
	}

	g_results[cpu].ops = 0;

	ti->threadid = cpu;
	ti->interleave = interleave;
	ti->delay = delay;

	ret = pthread_create(&g_threads[ti->threadid], NULL, worker_thread, ti);
	if (ret) {
		printf(" pthread_create failed with error code %d\n", ret);
		exit(1);
	}
}

//////////////////////////////////////////////////////////////////////////////

unsigned long long do_psem(int cpus, int timeout, int delay, int interleave)
{
	unsigned long long totals;
	int i;
	int res;

	g_state = WAITING;

	g_results = (struct tres *)malloc(sizeof(struct tres)*cpus);
	g_svsem_nrs = (int*)malloc(sizeof(int)*cpus);
	g_threads = (pthread_t*)malloc(sizeof(pthread_t)*cpus);

	for (i=0;i<cpus;i++)
		init_threads(i, cpus, delay, interleave);

	usleep(DELAY_10MS);
	g_state = RUNNING;
	sleep(timeout);
	g_state = STOPPED;
	usleep(DELAY_10MS);

	res = semctl(g_svsem_id,1,IPC_RMID,NULL);
	if (res < 0) {
		printf("semctl(IPC_RMID) failed for %d, errno%d.\n",
			g_svsem_id, errno);
	}

	for (i=0;i<cpus;i++)
		pthread_join(g_threads[i], NULL);

	if (g_verbose >= VERBOSE_NORMAL) {
		printf("Result matrix:\n");
	}
	totals = 0;
	for (i=0;i<cpus;i++) {
		if (g_verbose >= VERBOSE_NORMAL) {
			printf("  Thread %3d: %8lld utime %ld.%06ld systime %ld.%06ld vol cswitch %ld invol cswitch %ld tot %ld.\n",
				i, g_results[i].ops,
				g_results[i].ru.ru_utime.tv_sec, g_results[i].ru.ru_utime.tv_usec,
				g_results[i].ru.ru_stime.tv_sec, g_results[i].ru.ru_stime.tv_usec,
				g_results[i].ru.ru_nvcsw, g_results[i].ru.ru_nivcsw,
				g_results[i].ru.ru_nvcsw + g_results[i].ru.ru_nivcsw);
		}
		totals += g_results[i].ops;
	}
	printf("Cpus %d, interleave %d threadspercore %d delay %d: %lld in %d secs\n",
			cpus, interleave, g_threadspercore, delay,
			totals, timeout);

	free(g_results);
	free(g_svsem_nrs);
	free(g_threads);

	return totals;
}

//////////////////////////////////////////////////////////////////////////////

int *decode_commastring(const char *str)
{
	int i, len, count, pos;
	int *ret;

	len = strlen(str);
	count = 1;
	for (i=1;i<len;i++) {
		if (str[i] == ',')
			count++;
	}
	ret = (int*)malloc(sizeof(int)*(count+1));
	if (!ret) {
		printf("Could not allocate memory for decoding parameters.\n");
		exit(1);
	}

	pos = 0;
	for (i=0;i<count;i++) {
		ret[i] = 0;
		while (str[pos] != ',') {
			ret[i] = ret[i]*10 + str[pos]-'0';
			pos++;
			if (pos >= len)
				break;
		}
		pos++;
	}
	ret[count] = 0;
	return ret;
}
//////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
	int timeout;
	unsigned long long totals; // Sum of loops over all cpus
	unsigned long long max_totals; // Max total regardless of cpu count
	unsigned long long max_abs; // Max, regardless of delay & cpu count
	int *interleaves;
	int *cpus;
	int fastest;
	int i, j, k;
	int opt;
	int maxdelay;
	int forceall;

	timeout = 5;
	interleaves = NULL;
	cpus = NULL;
	maxdelay = 512;
	forceall = 0;

	printf("sem-scalebench\n");

	while ((opt = getopt(argc, argv, "m:vt:i:c:d:p:o:f")) != -1) {
		switch(opt) {
			case 'f':
				forceall = 1;
				break;
			case 'v':
				g_verbose++;
				break;
			case 'i':
				interleaves = decode_commastring(optarg);
				break;
			case 'p':
				g_threadspercore = atoi(optarg);
				if (g_threadspercore <= 0) {
					printf(" Invalid number of threads per core specified.\n");
					return 1;
				}
				break;
			case 'c':
				cpus = decode_commastring(optarg);
				break;
			case 't':
				timeout = atoi(optarg);
				if (timeout <= 0) {
					printf(" Invalid timeout specified.\n");
					return 1;
				}
				break;
			case 'm':
				maxdelay = atoi(optarg);
				if (maxdelay < 0) {
					printf(" Invalid maxdelay specified.\n");
					return 1;
				}
				break;
			case 'd':
				g_sem_distance = atoi(optarg);
				if (g_sem_distance < 0) {
					printf(" Invalid semaphore distance specified.\n");
					return 1;
				}
				break;
			case 'o':
				g_operation = atoi(optarg);
				switch (g_operation) {
					case WAIT_FOR_ZERO:
					case PING_PONG:
						break;
					default:
						printf(" Invalid operation requested.\n");
						return 1;
				}
				break;
			default: /* '?' */
				printf(" sem-scalebench-%s, (C) Manfred Spraul 1999-2014\n", SEM_SCALEBENCH_VERSION);
				printf("\n");
				printf(" Sem-scalebench performs parallel sysv semaphore operations.\n");
				printf(" Each thread has it's own semaphore in one large semaphore array.\n");
				printf(" The benchmark supports two tests:\n");
				printf(" 1) Wait-for-zero:\n");
				printf("    The semaphores are always 0, i.e. the threads never sleep and no task\n");
				printf("    switching will occur.\n");
				printf("    This might be representative for a big-reader style lock. If the performance\n");
				printf("    goes down when more cores are added then user space operations are performed\n");
				printf("    until the maximum rate of semaphore operations is observed.\n");
				printf(" 2) Ping-Pong:\n");
				printf("    Pairs of threads pass a token to each other. Each token passing forces\n");
				printf("    a task switch.\n");
				printf("\n");
				printf(" Usage:\n");
				printf("  -v: Verbose mode. Specify twice for more details\n");
				printf("  -t x: Test duration, in seconds. Default 5.\n");
				printf("  -c cpucount1,cpucount2: comma-separated list of cpu counts to use.\n");
				printf("  -i interleave1,interleave2: comma-separated list of interleaves.\n");
				printf("  -p threads per core: Number of threads that should run on one core.\n");
				printf("  -m: Max amount of user space operations (%s).\n", DELAY_ALGORITHM);
				printf("  -d: Difference between the used semaphores, default 1.\n");
				printf("  -o 1/2: Operation, either 1 (wait-for-zero) or 2 (ping-pong). Default 1.\n");
				printf("  -f: Force to evaluate all cpu values.\n");
				return 1;
		}
	}
	if (cpus) {
		g_max_cpus = cpus[0];
		i = 1;
		while(cpus[i] != 0) {
			if (cpus[i] > g_max_cpus)
				g_max_cpus = cpus[i];
			i++;
		}
	} else {
		cpu_set_t cpuset;
		int ret;

		ret = pthread_getaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
		if (ret < 0) {
			printf("pthread_getaffinity_np() failed with errno %d.\n", errno);
			return 1;
		} else {
			g_max_cpus = 0;
			while (CPU_ISSET(g_max_cpus, &cpuset))
				g_max_cpus++;
		}
		if (g_max_cpus == 0) {
			printf("Autodetection of the number of cpus failed.\n");
			return 1;
		}
		j = 1;
		i = 0;
		while (j < g_max_cpus) {
			j+=j*0.2+1;
			i++;
		}
		i = i + 2;
		cpus = (int*)malloc(sizeof(int)*(i+2));
		if (!cpus) {
			printf("Could not allocate memory for decoding parameters.\n");
			exit(1);
		}
		j = 1;
		i = 0;
		while (j < g_max_cpus) {
			cpus[i] = j;
			j+=j*0.2+1;
			i++;
		}
		cpus[i] = g_max_cpus;
		cpus[i+1] = 0;
	}
	if (!interleaves) {
		j=g_max_cpus-1;
		if (j==0)
			j = 1;

		i = 0;
		while (j > 0) {
			j = j/2;
			i++;
		}
		interleaves = (int*)malloc(sizeof(int)*(i+1));
		if (!interleaves) {
			printf("Could not allocate memory for decoding parameters.\n");
			exit(1);
		}
		for (j = 0; j < i; j++)
			interleaves[j] = 1<<j;
		interleaves[i] = 0;
	}
	if (g_verbose >= VERBOSE_NORMAL) {
		for (k = 0; interleaves[k] != 0; k++) {
			printf("  Interleave %d: %d.\n", k, interleaves[k]);
		}
		for (k = 0; cpus[k] != 0; k++) {
			printf("  Cpu count %d: %d.\n", k, cpus[k]);
		}
	}
	if (g_operation == WAIT_FOR_ZERO)
		printf("Performing WAIT_FOR_ZERO operations.\n");
	else
		printf("Performing PING_PONG operations.\n");

	for (k = 0; interleaves[k] != 0; k++) {
		max_abs = 0;
		for (j=0;;) {
			max_totals = 0;
			fastest = 0;
			for (i=0; cpus[i] != 0; i++) {
				int cur_cpus;
				
				/*
				 * PING_PONG is impossible with just 1 cpu and
				 * needs an even thread count.
				 */
				cur_cpus = cpus[i];
				if (g_operation == PING_PONG && cur_cpus == 1) {
					totals = 0;
				} else {
					if (g_operation == PING_PONG && cur_cpus%2 != 0)
						cur_cpus--;	
					totals = do_psem(cur_cpus, timeout, j, interleaves[k]);
				}
				if (totals > max_totals) {
					max_totals = totals;
					fastest = cur_cpus;
				} else {
					if (totals < 0.5*max_totals && cpus[i] > (2+1.5*fastest) && forceall == 0)
						break;
				}
			}
			printf("Interleave %d, delay %d: Max total: %lld with %d cpus\n",
					interleaves[k], j, max_totals, fastest);

			if (max_abs < max_totals)
				max_abs = max_totals;

			if (fastest == g_max_cpus)
				break;
			if (j >= maxdelay)
				break;
			if (max_totals < 0.1*max_abs)
				break;

			/* increase delay in 30% steps */
			j += j * 0.3 + 1;
		}
	}
}
