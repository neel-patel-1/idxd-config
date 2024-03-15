// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include "accel_test.h"
#include "iaa.h"
#include "idxd.h"

#define IAA_TEST_SIZE 20000
#pragma GCC diagnostic ignored "-Wformat"

unsigned long msec_timeout = 1000; // 5 msec
unsigned long complete_notify_cycle = 0, complete_receive_cycle = 0;

static inline unsigned long rdtsc(void)
{
	uint32_t a, d;

	asm volatile("rdtsc" : "=a"(a), "=d"(d));
	return ((uint64_t)d << 32) | (uint64_t)a;
}

static inline void umonitor(void *addr)
{
	asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xf0" : : "a"(addr));
}

static inline int umwait(unsigned long timeout, unsigned int state)
{
	uint8_t r;
	uint32_t timeout_low = (uint32_t)timeout;
	uint32_t timeout_high = (uint32_t)(timeout >> 32);

	asm volatile(".byte 0xf2, 0x48, 0x0f, 0xae, 0xf1\t\n"
		"setc %0\t\n"
		: "=r"(r)
		: "c"(state), "a"(timeout_low), "d"(timeout_high));
	return r;
}

void *function(void *args){
	unsigned long timeout = (msec_timeout * 1000000UL) * 3;
	struct completion_record *comp = (struct completion_record *)args;
	uint32_t state = 0;
	umonitor((uint8_t *)comp);
	if (comp->status != 0)
			printf("Completion status is not 0\n");
	umwait(0, state);
	complete_receive_cycle = rdtsc();

	printf("Completion received after %lu cycles\n", complete_receive_cycle - complete_notify_cycle);
	return NULL;
}

int main(int argc, char *argv[])
{

	pthread_t core, ax;
	int status = 1;
	pthread_attr_t attr;
	struct sched_param param;


	status = pthread_attr_init(&attr);
	if (status != 0)
	{
			printf("%d\n", errno);
			return -1;
	}

	/* Setting scheduling parameter will fail for non root user,
		* as the default value of inheritsched is PTHREAD_EXPLICIT_SCHED in
		* POSIX. It is not required to set it explicitly before setting the
		* scheduling policy */


			status = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
			if (status != 0)
			{
					pthread_attr_destroy(&attr);
					printf("%d\n", errno);
					return -1;
			}

			status =
					pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
			if (status != 0)
			{
					pthread_attr_destroy(&attr);
					printf("%d\n", errno);
					return -1;
			}

			/* Set priority based on value in threadAttr */
			memset(&param, 0, sizeof(param));
			param.sched_priority = SCHED_OTHER;

			status = pthread_attr_setschedparam(&attr, &param);
			if (status != 0)
			{
					pthread_attr_destroy(&attr);
					printf("%d\n", errno);
					return -1;
			}

	status = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (status != 0)
	{
			printf("%d\n", errno);
			pthread_attr_destroy(&attr);
			return -1;
	}

	status = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
	if (status != 0)
	{
			printf("%d\n", errno);
			pthread_attr_destroy(&attr);
			return -1;
	}

	uint8_t *comp = (uint8_t *)malloc(sizeof(struct completion_record));
	memset(comp, 0, sizeof(struct completion_record));
	status = pthread_create(&core, &attr, (void *(*)(void *))function, (void *)comp);
	// barrier();
	usleep(100);
	// ((struct completion_record *)comp)->status = 1;
	complete_notify_cycle = rdtsc();
	pthread_join(core, NULL);

	if (status != 0)
	{
			printf("%d\n", errno);
			pthread_attr_destroy(&attr);
			return -1;
	}
	/*destroy the thread attributes as they are no longer required, this does
		* not affect the created thread*/
	pthread_attr_destroy(&attr);
	return 0;

}