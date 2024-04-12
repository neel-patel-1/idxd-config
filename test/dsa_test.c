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
#define IAA_COMPRESS_AECS_SIZE (1568)
#define IAA_COMPRESS_SRC2_SIZE (IAA_COMPRESS_AECS_SIZE * 2)
#define IAA_COMPRESS_MAX_DEST_SIZE (2097152 * 2)

static unsigned long buf_size = DSA_TEST_SIZE;
static struct acctest_context *dsa, *iaa;

// Function prototypes
void *memcpy_and_submit(void *arg);
void *wait_for_iaa(void *arg);
void *dsa_submit(void *arg);

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
		iaa_tsk_node = iaa_tsk_node->next;
		dsa_tsk_node = dsa_tsk_node->next;
	}
	return rc;
}

static int host_op(void *buffer, size_t size) {
    if (buffer == NULL || size % sizeof(uint32_t) != 0) {
        return -1; 
    }

    uint32_t *ptr = (uint32_t *) buffer;
    size_t count = 0;
    size_t num_elements = size / sizeof(uint32_t);

    for (size_t i = 0; i < num_elements; ++i) {
        if (ptr[i] >= 10000) {
            count++;
        }
    }
	// printf("Count is : %d\n", count);
    return count;
}

void *dsa_submit(void *arg) {
	int rc = 0;
	rc = dsa_memcpy_submit_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		pthread_exit((void *)rc);
	pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *memcpy_and_submit(void *arg) {
	struct task_node *dsa_tsk_node, *iaa_tsk_node;
    int rc;
	// int itr = 0;
	dsa_tsk_node = dsa->multi_task_node;
	iaa_tsk_node = iaa->multi_task_node;

    while (dsa_tsk_node) {
		// printf("memcpy and submit itr: %d\n", itr++);
        rc = dsa_wait_memcpy(dsa, dsa_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)rc);
		host_op(dsa_tsk_node->tsk->dst1, buf_size);
        iaa_tsk_node->tsk->src1 = dsa_tsk_node->tsk->dst1;
        iaa_prep_sub_task_node(iaa, iaa_tsk_node);
        dsa_tsk_node = dsa_tsk_node->next;
        iaa_tsk_node = iaa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *wait_for_iaa(void *arg) {
    struct task_node *iaa_tsk_node = iaa->multi_task_node;
    int rc;
	// int itr = 0;

    while(iaa_tsk_node) {
		// printf("Wait for IAA itr: %d\n", itr++);
        rc = iaa_wait_compress(iaa, iaa_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)rc);
        iaa_tsk_node = iaa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
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
	// unsigned int num_iter = 1;
	pthread_t dsa_wait_thread, iaa_wait_thread;
	pthread_t dsa_submit_thread;
	int rc0, rc1, rc2;
	struct timespec times[2];
	long long lat = 0;

	while ((opt = getopt(argc, argv, "w:l:i:t:n:vh")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		// case 'i':
		// 	num_iter = strtoul(optarg, NULL, 0);
		// 	break;
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
	clock_gettime(CLOCK_MONOTONIC, &times[0]);
	pthread_create(&dsa_submit_thread, NULL, dsa_submit, NULL);
	pthread_create(&dsa_wait_thread, NULL, memcpy_and_submit, NULL);
    pthread_create(&iaa_wait_thread, NULL, wait_for_iaa, NULL);

	    // Wait for threads to finish
	pthread_join(dsa_submit_thread, (void **)&rc0);
    pthread_join(dsa_wait_thread, (void **)&rc1);
    pthread_join(iaa_wait_thread, (void **)&rc2);
	clock_gettime(CLOCK_MONOTONIC, &times[1]);

	lat = ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
					((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));

	if (rc0 != ACCTEST_STATUS_OK || rc1 != ACCTEST_STATUS_OK 
		|| rc2 != ACCTEST_STATUS_OK)
		goto error;
	// Final verification and cleanup
    rc = task_result_verify_task_nodes(dsa, 0);
	if (rc != ACCTEST_STATUS_OK)
		return rc;
    rc = task_result_verify_task_nodes(iaa, 0);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	printf("Total Latency: %lld ns\n", lat);
	printf("Throughput: %f\n", (buf_size * num_desc)/(double)lat);

    acctest_free_task(dsa);
	acctest_free_task(iaa);

 error:
	acctest_free(dsa);
	acctest_free(iaa);
	return rc;
}