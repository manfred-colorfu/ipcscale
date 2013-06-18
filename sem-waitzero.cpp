/*
 * sem-waitzero.cpp - sysv scaling test
 *
* Copyright (C) 1999, 2001, 2005, 2008, 2013 by Manfred Spraul.
 *	All rights reserved except the rights granted by the GPL.
 *
 * Redistribution of this file is permitted under the terms of the GNU 
 * General Public License (GPL) version 3 or later.
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
int *g_svsem_nrs;
pthread_t *g_threads;

struct taskinfo {
	int svsem_id;
	int svsem_nr;
	int threadid;
	int cpubind;
	int interleave;
	int delay;
};

int get_cpunr(int cpunr, int interleave)
{
	int off = 0;
	int ret = 0;

	if (g_verbose >= VERBOSE_DEBUG) {
		printf("get_cpunr %p: cpunr %d max_cpu %d interleave %d.\n",
			(void*)pthread_self(), cpunr, g_max_cpus, interleave);
	}

	while (cpunr>0) {
		ret += interleave;
		if (ret >=g_max_cpus) {
			off++;
			ret = off;
		}
		cpunr--;
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
	int ret;
	int cpu = get_cpunr(ti->cpubind, ti->interleave);

	bind_cpu(cpu);
	if (g_verbose >= VERBOSE_NORMAL) {
		printf("thread %d: sysvsem %8d, sema %8d bound to cpu %d\n",ti->threadid,
				ti->svsem_id, ti->svsem_nr,cpu);
	}
	
	rounds = 0;
	while(g_state == WAITING) {
#ifdef __GNUC__
#if defined(__i386__) || defined (__x86_64__)
		__asm__ __volatile__("pause": : :"memory");
#else
		__asm__ __volatile__("": : :"memory");
#endif
#endif
	}

	while(g_state == RUNNING) {
		struct sembuf sop[1];

		/* 1) check if the semaphore value is 0 */
		sop[0].sem_num=ti->svsem_nr;
		sop[0].sem_op=0;
		sop[0].sem_flg=0;
		ret = semop(ti->svsem_id,sop,1);
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

	ti->svsem_id = g_svsem_id;
	ti->svsem_nr = g_svsem_nrs[cpu];
	ti->threadid = cpu;
	ti->cpubind = cpu;
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
			printf("  Thread %3d: %8lld utime %ld.%06ld systime %ld.%06ld vol cswitch %ld invol cswitch %ld.\n",
				i, g_results[i].ops,
				g_results[i].ru.ru_utime.tv_sec, g_results[i].ru.ru_utime.tv_usec,
				g_results[i].ru.ru_stime.tv_sec, g_results[i].ru.ru_stime.tv_usec,
				g_results[i].ru.ru_nvcsw, g_results[i].ru.ru_nivcsw);
		}
		totals += g_results[i].ops;
	}
	printf("Cpus %d, interleave %d delay %d: %lld in %d secs\n",
			cpus, interleave, delay,
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
	unsigned long long totals, max_totals;
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

	printf("sem-waitzero\n");

	while ((opt = getopt(argc, argv, "m:vt:i:c:d:f")) != -1) {
		switch(opt) {
			case 'f':
				forceall = 1;
			case 'v':
				g_verbose++;
				break;
			case 'i':
				interleaves = decode_commastring(optarg);
				break;
			case 'c':
				cpus = decode_commastring(optarg);
				break;
			case 't':
				timeout = atoi(optarg);
				break;
			case 'm':
				maxdelay = atoi(optarg);
				break;
			case 'd':
				g_sem_distance = atoi(optarg);
				break;
			default: /* '?' */
				printf(" sem-waitzero v0.10, (C) Manfred Spraul 2013\n");
				printf("\n");
				printf(" Sem-waitzero performs parallel 'test-for-zero' sysv semaphore operations.\n");
				printf(" Each thread has it's own semaphore in one large semaphore array.\n");
				printf(" The semaphores are always 0, i.e. the threads never sleep and no task\n");
				printf(" switching will occur.\n");
				printf(" This might be representative for a big-reader style lock. If the performance\n");
				printf(" goes down when more cores are added then user space operations are performed\n");
				printf(" until the maximum rate of semaphore operations is observed.\n");
				printf("\n");
				printf(" Usage:\n");
				printf("  -v: Verbose mode. Specify twice for more details\n");
				printf("  -t x: Test duration, in seconds. Default 5.\n");
				printf("  -c cpucount1,cpucount2: comma-separated list of cpu counts to use.\n");
				printf("  -i interleave1,interleave2: comma-separated list of interleaves.\n");
				printf("  -m: Max amount of user space operations (%s).\n", DELAY_ALGORITHM);
				printf("  -d: Difference between the used semaphores, default 1.\n");
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
	for (k = 0; interleaves[k] != 0; k++) {
		for (j=0;;) {
			max_totals = 0;
			fastest = 0;
			for (i=0; cpus[i] != 0; i++) {
				totals = do_psem(cpus[i], timeout, j, interleaves[k]);
				if (totals > max_totals) {
					max_totals = totals;
					fastest = cpus[i];
				} else {
					if (totals < 0.5*max_totals && cpus[i] > (2+1.5*fastest) && forceall == 0)
						break;
				}
			}
			printf("Interleave %d, delay %d: Max total: %lld with %d cpus\n",
					interleaves[k], j, max_totals, fastest);

			if (fastest == g_max_cpus || j >= maxdelay)
				break;

			/* increase delay in 30% steps */
			j += j * 0.3 + 1;
		}
	}
}
