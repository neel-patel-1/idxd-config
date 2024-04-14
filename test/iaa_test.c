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
#include "iaa.h"
#include "algorithms/iaa_filter.h"
#include <pthread.h>

#define IAA_TEST_SIZE 16384
#define IAA_COMPRESS_AECS_SIZE (1568)
#define IAA_COMPRESS_SRC2_SIZE (IAA_COMPRESS_AECS_SIZE * 2)
#define IAA_COMPRESS_MAX_DEST_SIZE (2097152 * 2)

static unsigned long buf_size = IAA_TEST_SIZE;
static struct acctest_context *iaa;

static int init_iaa(int num_desc) {
	struct task_node *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

	info("testcompress: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     IAX_OPCODE_COMPRESS, buf_size, tflags, num_desc);

	rc = acctest_alloc_multiple_tasks(iaa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	/* allocate memory to src and dest buffers and fill in the desc for all the nodes*/
	iaa_tsk_node = iaa->multi_task_node;
	while (iaa_tsk_node) {
		iaa_tsk_node->tsk->iaa_compr_flags = 0;
		rc = iaa_init_task(iaa_tsk_node->tsk, tflags, IAX_OPCODE_COMPRESS, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		iaa_tsk_node = iaa_tsk_node->next;
	}
	
	return rc;
}

static int submit_and_wait() {
	int rc = ACCTEST_STATUS_OK;
	rc = iaa_compress_multi_task_nodes(iaa);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	// // /* Verification of all the nodes*/
	// rc = iaa_task_result_verify_task_nodes(iaa, 0);
	// if (rc != ACCTEST_STATUS_OK) {
	// 	printf("Error verifying compress\n");
	// }
	return rc;
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
	pthread_t iaa_sub_thread, iaa_wait_thread;
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

	// iaa setup
	iaa = acctest_init(tflags);
	iaa->dev_type = ACCFG_DEVICE_IAX;

	if (!iaa)
		return -ENOMEM;

	rc = acctest_alloc(iaa, wq_type, dev_id, wq_id);
	if (rc < 0)
		return -ENOMEM;

	if (buf_size > iaa->max_xfer_size) {
		err("invalid transfer size: %lu\n", buf_size);
		return -EINVAL;
	}

	rc = init_iaa(num_desc);
	if (rc != ACCTEST_STATUS_OK)
		goto error;
	for(int i = 0; i < num_iter; i++) {
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = submit_and_wait();
		if (rc != ACCTEST_STATUS_OK)
			goto error;

		// pthread_create(&iaa_sub_thread, NULL, iaa_submit, NULL);
		// pthread_create(&iaa_wait_thread, NULL, iaa_wait, NULL);

		// pthread_join(iaa_sub_thread, (void **)&rc0);
    	// pthread_join(iaa_wait_thread, (void **)&rc1);

		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
						((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		// rc = task_result_verify_task_nodes(iaa, 0);
		// if (rc != ACCTEST_STATUS_OK)
		// 	return rc;
	}
	
	avg_lat = (double)lat/num_iter;
	printf("Num desc: %ld, transfer size: %ld\n", num_desc, buf_size);
	printf("Average Latency: %.2f ms\n", avg_lat/1000000.0);
	printf("Throughput: %.2f\n", (buf_size * num_desc)/(double)avg_lat);

    acctest_free_task(iaa);

 error:
	acctest_free(iaa);
	return rc;
}