/*
 * Copyright (C) 1999, 2001, 2005, 2008, 2013 by Manfred Spraul.
 *	All rights reserved except the rights granted by the GPL.
 *
 * Redistribution of this file is permitted under the terms of the GNU 
 * General Public License (GPL) version 3 or later.
 */

#include <sys/sem.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>

#define TRUE	1
#define FALSE	0

union semun {
	int val;
	struct semid_ds *buf;
	unsigned short int *array;
	struct seminfo* __buf;
};

#define barrier()	__asm__ __volatile__("": : : "memory")

int g_loops;
int g_busy_in;
int g_busy_out;
int g_sem;
int g_completedsem;

static void thread_fnc(int id)
{
	int i;
	volatile int j;
	int res;
	struct sembuf sop[1];

	for (i=0;i<g_loops;i++) {

		sop[0].sem_num=id;
		sop[0].sem_op=-1;
		sop[0].sem_flg=0;
		res = semop(g_sem,sop,1);
		if(res==-1) {
			printf("semop -1 failed, errno %d.\n", errno);
			return;
		}
		for(j=0;j<g_busy_in;j++);
			barrier();
		sop[0].sem_num=id;
		sop[0].sem_op=1;
		sop[0].sem_flg=0;
		res = semop(g_sem,sop,1);
		if(res==-1) {
			printf("semop +1 failed, errno %d.\n", errno);
			return;
		}
		for(j=0;j<g_busy_out;j++);
			barrier();
	}

	sop[0].sem_num=g_completedsem;
	sop[0].sem_op=-1;
	sop[0].sem_flg=IPC_NOWAIT;
	res = semop(g_sem,sop,1);
	if(res==-1) {
		printf("semop -1 on completedsem returned %d, errno %d.\n", res, errno);
		return;
	}
	return;
}

int main(int argc,char** argv)
{
	int nsems;
	int tasks;
	int res;
	pid_t *pids;
	unsigned short *psems;
	struct timeval t_before, t_after;
	unsigned long long delta;
	union semun arg;
	int i;

	printf("sem-lockunlock <sems> <tasks> <loops> <busy-in> <busy-out>\n");
	if(argc != 6) {
		printf("Invalid parameters.\n");
		printf("\n");
		printf(" Sem-lockunlock create threads that perform lock/unlock with multiple sysv semaphores\n");
		printf(" int one semaphore array. No cpu binding is performed.\n");
		return 1;
	}
	nsems=atoi(argv[1]);
	tasks=atoi(argv[2]);
	g_loops=atoi(argv[3]);
	g_loops = (g_loops+tasks-1)/tasks;
	g_busy_in=atoi(argv[4]);
	g_busy_out=atoi(argv[5]);
	g_completedsem = nsems;

	res = semget(IPC_PRIVATE, nsems+1, 0777 | IPC_CREAT);
	if(res == -1) {
		printf(" create failed.\n");
		return 1;
	}
	g_sem = res;
	fflush(stdout);

	pids = malloc(sizeof(pid_t)*tasks);
	for (i=0;i<tasks;i++) {
		res = fork();
		if (res == 0) {
			thread_fnc(i%nsems);
			exit(0);
		} 
		if (res == -1) {
			printf("fork() failed, errno now %d.\n", errno);
			return 1;
		}
		pids[i] = res;
	}

	printf("sem-lockunlock: using a semaphore array with %d semaphores.\n", nsems);
	printf("sem-lockunlock: using %d tasks.\n", tasks);
	printf("sem-lockunlock: each thread loops %d times\n", g_loops);
	printf("sem-lockunlock: each thread busyloops %d loops outside and %d loops inside.\n", g_busy_out, g_busy_in);
	fflush(stdout);

	psems = malloc(sizeof(unsigned short)*nsems);
	for (i=0;i<nsems;i++)
		psems[i] = 1;
	psems[i] = tasks;

	{
		struct sembuf sop[1];

		gettimeofday(&t_before, NULL);
		arg.array = psems;
		semctl(g_sem, 0, SETALL, arg);

		sop[0].sem_num=g_completedsem;
		sop[0].sem_op=0;
		sop[0].sem_flg=0;
		res = semop(g_sem,sop,1);
		if(res==-1) {
			printf("semop 0 failed, errno %d.\n", errno);
			return 1;
		}
		gettimeofday(&t_after, NULL);
	}
	for (i=0;i<tasks;i++) {
		res = waitpid(pids[i], NULL, 0);
		if (res != pids[i]) {
			printf("waitpid() failed, errno now %d.\n", errno);
			return 1;
		}
	}

	delta = t_after.tv_sec - t_before.tv_sec;
	delta = delta*1000000L;
	delta += t_after.tv_usec - t_before.tv_usec;

	printf("total execution time: %Ld.%03Ld%03Ld seconds for %d loops\n",
		(delta/1000000),
		(delta/1000)%1000,
		(delta)%1000,
		tasks*g_loops);

	delta = delta*1000;
	delta = delta/(tasks*g_loops);

	printf("per loop execution time: %Ld.%03Ld usec\n",
		(delta/1000),
		(delta)%1000);

	res = semctl(g_sem, 1, IPC_RMID, arg);
	if(res == -1) {
		printf(" semctl failed.\n");
		return 1;
	}
	return 0;
}
