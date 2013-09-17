/*
 * sem-pingpong.cpp, parallel sysv sem pingpong
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
#include <pthread.h>

#define SEM_PINGPONG_VERSION	"0.01"

#ifdef __sun
	 #include <sys/pset.h> /* P_PID, processor_bind() */
#endif

#undef VERBOSE

//////////////////////////////////////////////////////////////////////////////

static enum {
	WAITING,
	RUNNING,
	STOPPED,
} volatile g_state = WAITING;

unsigned long long *g_results;
int *g_svsem_ids;
int *g_svsem_nrs;
pthread_t *g_threads;

struct taskinfo {
	int svsem_id;
	int svsem_nr;
	int threadid;
	int cpubind;
	int sender;
};

void bind_cpu(int cpunr)
{
#if __sun
	int ret;
	ret = processor_bind(P_PID, getpid(), cpunr, NULL);
	if (ret == -1) {
		perror("bind_thread:processor_bind");
		printf(" Binding to cpu %d failed.\n", cpunr);
	}
#else
	int ret;
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
#define DATASIZE	8

void* worker_thread(void *arg)
{
	struct taskinfo *ti = (struct taskinfo*)arg;
	unsigned long long rounds;
	int ret;

	bind_cpu(ti->cpubind);
#ifdef VERBOSE
	printf("thread %d: sysvsem %8d, off %8d type %d bound to cpu %d\n",ti->threadid,
			ti->svsem_id, ti->svsem_nr,
			ti->sender, ti->cpubind);
#endif
	
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

	if (ti->sender) {
		struct sembuf sop[1];

		/* 1) insert token */
		sop[0].sem_num=ti->svsem_nr+0;
		sop[0].sem_op=1;
		sop[0].sem_flg=0;
		ret = semop(ti->svsem_id,sop,1);
	
		if (ret != 0) {
			printf("Initial semop failed, ret %d, errno %d.\n", ret, errno);
			exit(1);
		}
	}
	while(g_state == RUNNING) {
		struct sembuf sop[1];

		/* 1) retrieve token */
		sop[0].sem_num=ti->svsem_nr+ti->sender;
		sop[0].sem_op=-1;
		sop[0].sem_flg=0;
		ret = semop(ti->svsem_id,sop,1);
		if (ret != 0) {
			/* EIDRM can happen */
			if (errno == EIDRM)
				break;

			/* Some OS do not report EIDRM properly */
			if (g_state != RUNNING)
				break;
			printf("main semop failed, ret %d errno %d.\n", ret, errno);
			printf(" round %lld sop: num %d op %d flg %d.\n",
					rounds,
					sop[0].sem_num, sop[0].sem_op, sop[0].sem_flg);
			fflush(stdout);
			exit(1);
		}

		/* 2) reinsert token */
		sop[0].sem_num=ti->svsem_nr+1-ti->sender;
		sop[0].sem_op=1;
		sop[0].sem_flg=0;
		ret = semop(ti->svsem_id,sop,1);
		if (ret != 0) {
			/* EIDRM can happen */
			if (errno == EIDRM)
				break;
			/* Some OS do not report EIDRM properly */
			if (g_state != RUNNING)
				break;
			printf("main semop failed, ret %d errno %d.\n", ret, errno);
			printf(" round %lld sop: num %d op %d flg %d.\n",
					rounds,
					sop[0].sem_num, sop[0].sem_op, sop[0].sem_flg);
			fflush(stdout);
			exit(1);
		}
		rounds++;
	}
	g_results[ti->threadid] = rounds;

	pthread_exit(0);
	return NULL;
}

void init_threads(int cpu, int cpus, int sems, int shared)
{
	int ret;
	struct taskinfo *ti1, *ti2;

	ti1 = (struct taskinfo*)malloc(sizeof(struct taskinfo));
	ti2 = (struct taskinfo*)malloc(sizeof(struct taskinfo));
	if (!ti1 || !ti2) {
		printf("Could not allocate task info\n");
		exit(1);
	}
	if (cpu % sems == 0) {
		int i;
		g_svsem_ids[cpu] = semget(IPC_PRIVATE,2*sems,0777|IPC_CREAT);
		if(g_svsem_ids[cpu] == -1) {
			printf("sem array create failed.\n");
			exit(1);
		}
		for (i=0;i<sems;i++) {
			g_svsem_ids[cpu+i] = g_svsem_ids[cpu];
			g_svsem_nrs[cpu+i] = 2*i;
		}
	}

	g_results[cpu] = 0;
	g_results[cpu+cpus] = 0;

	ti1->svsem_id = g_svsem_ids[cpu];
	ti1->svsem_nr = g_svsem_nrs[cpu];
	ti1->threadid = cpu;
	ti1->cpubind = cpu;
	ti1->sender = 1;
	ti2->svsem_id = g_svsem_ids[cpu];
	ti2->svsem_nr = g_svsem_nrs[cpu];
	ti2->threadid = cpu+cpus;
	if (shared) {
		ti2->cpubind = cpu;
	} else {
		ti2->cpubind = cpus+cpu;
	}
	ti2->sender = 0;

	ret = pthread_create(&g_threads[ti1->threadid], NULL, worker_thread, ti1);
	if (ret) {
		printf(" pthread_create failed with error code %d\n", ret);
		exit(1);
	}
	ret = pthread_create(&g_threads[ti2->threadid], NULL, worker_thread, ti2);
	if (ret) {
		printf(" pthread_create failed with error code %d\n", ret);
		exit(1);
	}
}

//////////////////////////////////////////////////////////////////////////////

void do_psem(int queues, int timeout, int shared)
{
	unsigned long long totals;
	int i;
	int sems = queues; /* No need to test multiple arrays: that part scales linearly */

	g_state = WAITING;

	g_results = (unsigned long long*)malloc(sizeof(unsigned long long)*2*queues);
	g_svsem_ids = (int*)malloc(sizeof(int)*(queues+sems));
	g_svsem_nrs = (int*)malloc(sizeof(int)*(queues+sems));
	g_threads = (pthread_t*)malloc(sizeof(pthread_t)*2*queues);
	for (i=0;i<queues;i++) {
		init_threads(i, queues, sems, shared);
	}

	usleep(10000);
	g_state = RUNNING;
	sleep(timeout);
	g_state = STOPPED;
	usleep(10000);
	for (i=0;i<queues;i++) {
		int res;
		if (g_svsem_nrs[i] == 0) {
			res = semctl(g_svsem_ids[i],1,IPC_RMID,NULL);
			if (res < 0) {
				printf("semctl(IPC_RMID) failed for %d, errno%d.\n",
					g_svsem_ids[i], errno);
			}
		}
	}
	for (i=0;i<2*queues;i++)
		pthread_join(g_threads[i], NULL);

#ifdef VERBOSE
	printf("Result matrix:\n");
#endif
	totals = 0;
	for (i=0;i<queues;i++) {
#ifdef VERBOSE
		printf("  Thread %3d: %8lld     %3d: %8lld\n",
				i, g_results[i], i+queues, g_results[i+queues]);
#endif
		totals += g_results[i] + g_results[i+queues];
	}
	printf("Queues: %d (%s) %lld in %d secs\n", queues, shared ? "intra-cpu" : "inter-cpu",
			totals, timeout);

	free(g_results);
	free(g_svsem_ids);
	free(g_svsem_nrs);
	free(g_threads);
}

//////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
	int max_cpus;
	int timeout;
	int i;

	printf("sem-pingpong %s [max cpus] [timeout]\n",
			SEM_PINGPONG_VERSION);
	if (argc != 3) {
		printf(" Invalid parameters.\n");
		printf("\n");
		printf(" Sem-pingpong create pairs of threads that perform ping/pong with sysv semaphores.\n");
		printf(" The threads are cpu bound: either to the same cpu or to different cpus.\n");
		return 1;
	}
	max_cpus = atoi(argv[1]);
	timeout = atoi(argv[2]);
	/* Intra-cpu */
	for (i=1;i<=max_cpus;i++) {
		usleep(10000);
		do_psem(i, timeout, 1);
	}
	/* Inter-cpu */
	for (i=1;i<=max_cpus/2;i++) {
		usleep(10000);
		do_psem(i, timeout, 0);
	}

}
