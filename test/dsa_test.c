#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <linux/mman.h>
#include "accel_test.h"
#include "dsa.h"
#include "iaa.h"
#include "algorithms/iaa_filter.h"
#include <pthread.h>

#define DSA_TEST_SIZE 16384

static unsigned long buf_size = DSA_TEST_SIZE;
static struct acctest_context *dsa;


void *dsa_submit(void *arg);
void *dsa_wait(void *arg);

static int init_dsa(int num_desc) {
	struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

	info("testmemory: opcode %d len %#lx tflags %#x num_desc %ld transfer_size: %ld\n",
	     DSA_OPCODE_MEMMOVE, buf_size, tflags, num_desc, buf_size);

	dsa->is_batch = 0;

	rc = acctest_alloc_multiple_tasks(dsa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	/* allocate memory to src and dest buffers and fill in the desc for all the nodes*/
	dsa_tsk_node = dsa->multi_task_node;
	while (dsa_tsk_node) {
		dsa_tsk_node->tsk->xfer_size = buf_size;

		rc = dsa_init_task(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		dsa_tsk_node = dsa_tsk_node->next;
	}

	return rc;
}

void *dsa_submit(void *arg) {
	int rc = 0;
	rc = dsa_memcpy_submit_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		pthread_exit((void *)(intptr_t)rc);
	pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *dsa_wait(void *arg) {
    struct task_node *dsa_tsk_node = dsa->multi_task_node;
    int rc;
	// int itr = 0;

    while(dsa_tsk_node) {
		// printf("Wait for IAA itr: %d\n", itr++);
        rc = dsa_wait_memcpy(dsa, dsa_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        dsa_tsk_node = dsa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}


static int submit_and_wait() {
	int rc = ACCTEST_STATUS_OK;
	rc = dsa_memcpy_multi_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	// /* Verification of all the nodes*/
	// rc = task_result_verify_task_nodes(dsa, 0);
	// if (rc != ACCTEST_STATUS_OK)
	// 	return rc;
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int wq_type = SHARED;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int wq_id = ACCTEST_DEVICE_ID_NO_INPUT;
	int dev_id = ACCTEST_DEVICE_ID_NO_INPUT;
	unsigned int num_desc = 1;
	unsigned int num_iter = 1;
	pthread_t dsa_sub_thread, dsa_wait_thread;
	int rc0, rc1, rc2;
	struct timespec times[2];
	long long lat = 0;
	double avg_lat = 0;

	while ((opt = getopt(argc, argv, "w:l:i:t:n:vh")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		case 'i':
			num_iter = strtoul(optarg, NULL, 0);
			break;
		case 't':
			ms_timeout = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			num_desc = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			debug_logging = 1;
			break;
		default:
			break;
		}
	}

	buf_size = 1048576/num_desc;

	// DSA setup
	dsa = acctest_init(tflags);
	dsa->dev_type = ACCFG_DEVICE_DSA;

	if (!dsa)
		return -ENOMEM;

	rc = acctest_alloc(dsa, wq_type, dev_id, wq_id);
	if (rc < 0)
		return -ENOMEM;

	if (buf_size > dsa->max_xfer_size) {
		err("invalid transfer size: %lu\n", buf_size);
		return -EINVAL;
	}
	rc = init_dsa(num_desc);
	if (rc != ACCTEST_STATUS_OK)
		goto error;
	for(int i = 0; i < num_iter; i++) {
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = submit_and_wait();
		if (rc != ACCTEST_STATUS_OK)
			goto error;

		// pthread_create(&dsa_sub_thread, NULL, dsa_submit, NULL);
		// pthread_create(&dsa_wait_thread, NULL, dsa_wait, NULL);

		// pthread_join(dsa_sub_thread, (void **)&rc0);
    	// pthread_join(dsa_wait_thread, (void **)&rc1);

		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
						((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		// rc = task_result_verify_task_nodes(dsa, 0);
		// if (rc != ACCTEST_STATUS_OK)
		// 	return rc;
	}
	
	avg_lat = (double)lat/num_iter;
	printf("Num desc: %ld, transfer size: %ld\n", num_desc, buf_size);
	printf("Average Latency: %.4f ms\n", avg_lat/1000000.0);
	printf("Throughput: %.2f\n", (buf_size * num_desc)/(double)avg_lat);

    acctest_free_task(dsa);

 error:
	acctest_free(dsa);
	return rc;
}