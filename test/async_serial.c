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

#define DSA_TEST_SIZE 16384
#define IAA_COMPRESS_AECS_SIZE (1568)
#define IAA_COMPRESS_SRC2_SIZE (IAA_COMPRESS_AECS_SIZE * 2)
#define IAA_COMPRESS_MAX_DEST_SIZE (2097152 * 2)

struct acctest_context *dsa, *iaa;

static int setup_dsa_iaa(int buf_size, int num_desc) {
	struct task_node *dsa_tsk_node, *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

	info("testmemory: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     DSA_OPCODE_MEMMOVE, buf_size, tflags, num_desc);

	dsa->is_batch = 0;

	rc = acctest_alloc_multiple_tasks(dsa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;
	rc = acctest_alloc_multiple_tasks(iaa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	/* allocate memory to src and dest buffers and fill in the desc for all the nodes*/
	dsa_tsk_node = dsa->multi_task_node;
	iaa_tsk_node = iaa->multi_task_node;
	while (dsa_tsk_node) {
		dsa_tsk_node->tsk->xfer_size = buf_size;

		rc = dsa_init_task(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		rc = iaa_init_task(iaa_tsk_node->tsk, tflags, IAX_OPCODE_COMPRESS, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
        iaa_tsk_node->tsk->src1 = dsa_tsk_node->tsk->dst1;
		iaa_tsk_node = iaa_tsk_node->next;
		dsa_tsk_node = dsa_tsk_node->next;
	}

	return rc;
}

static int test_dsa(void) {
	int rc = ACCTEST_STATUS_OK;
	rc = dsa_memcpy_multi_task_nodes(dsa);
	return rc;
}

static int test_iaa(void) {
	int rc = ACCTEST_STATUS_OK;
	rc = iaa_compress_multi_task_nodes(iaa);
	return rc;
}

static int host_op(void *buffer, size_t size) {
	uint32_t *ptr;
	size_t count;
	size_t num_elements;
    if (buffer == NULL || size % sizeof(uint32_t) != 0) {
        return -1; 
    }

    ptr = (uint32_t *) buffer;
    count = 0;
    num_elements = size / sizeof(uint32_t);

    for (size_t i = 0; i < num_elements; ++i) {
        if (ptr[i] >= 10000) {
            count++;
        }
    }
	// printf("Count is : %d\n", count);
    return count;
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int wq_type = SHARED;
	unsigned long buf_size = DSA_TEST_SIZE;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int wq_id = ACCTEST_DEVICE_ID_NO_INPUT;
	int dev_id = ACCTEST_DEVICE_ID_NO_INPUT;
	unsigned int num_desc = 1;
	struct timespec times[2];
	long long lat = 0;
	struct task_node *dsa_tsk_node;
	int num_iter = 1;
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

	rc = setup_dsa_iaa(buf_size, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		goto error;
	for(int i = 0; i < num_iter; i++) {
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
	
		rc = test_dsa();
		if (rc != ACCTEST_STATUS_OK)
			goto error;

		dsa_tsk_node = dsa->multi_task_node;
		while (dsa_tsk_node) {
			host_op(dsa_tsk_node->tsk->dst1, buf_size);
			dsa_tsk_node = dsa_tsk_node->next;
		}

		rc = test_iaa();
		if (rc != ACCTEST_STATUS_OK)
			goto error;

		clock_gettime(CLOCK_MONOTONIC, &times[1]);

		lat = ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
						((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));

		// Final verification and cleanup
		// rc = task_result_verify_task_nodes(dsa, 0);
		// if (rc != ACCTEST_STATUS_OK)
		// 	goto error;
		// rc = task_result_verify_task_nodes(iaa, 0);
		// if (rc != ACCTEST_STATUS_OK)
		// 	goto error;
	}
	

	avg_lat = (double)lat/num_iter;
	printf("Num desc: %ld, transfer size: %ld\n", num_desc, buf_size);
	printf("Average Latency: %.2f ms\n", avg_lat/1000000.0);
	printf("Throughput: %.2f\n", (buf_size * num_desc)/(double)avg_lat);

    acctest_free_task(dsa);
	acctest_free_task(iaa);

 error:
	acctest_free(dsa);
	// acctest_free(iaa);
	return rc;
}