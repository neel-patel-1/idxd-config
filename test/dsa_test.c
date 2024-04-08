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
// #include "qpl/qpl.h"

//To run -> sudo ./test/dsa_test -w 1 -i 1000 -l <size>

#define DSA_TEST_SIZE 1024
long long scan_lat = 0;
long long select_lat = 0;
long long shuffle_lat = 0;
long long memcpy_lat = 0;
struct timespec times[2];
bool print_contents = true;

void *memcpy_src1;
uint64_t memcpy_size;

static int memcpy_init(struct task *tsk, int tflags, int opcode, unsigned long xfer_size) {
	unsigned long force_align = PAGE_SIZE;

	tsk->pattern = 0x0123456789abcdef;
	tsk->pattern2 = 0xfedcba9876543210;
	tsk->opcode = opcode;
	tsk->test_flags = tflags;
	tsk->xfer_size = xfer_size;

	tsk->src1 = memcpy_src1;
	// tsk->src1 = aligned_alloc(force_align, xfer_size);
	// if (!tsk->src1)
	// 	return -ENOMEM;
	// memset_pattern(tsk->src1, tsk->pattern, xfer_size);

	tsk->dst1 = aligned_alloc(force_align, xfer_size);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, xfer_size);

	return ACCTEST_STATUS_OK;
}

static void shuffle_elements(void *array, size_t size) {
    uint32_t *arr = (uint32_t *)array;
    size_t n = size / sizeof(uint32_t); // Number of elements
    if (n > 1) {
        srand((unsigned)time(NULL)); // Seed the random number generator
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = rand() % (i + 1); // Random index from 0 to i
            // Swap arr[i] and arr[j]
            uint32_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }
    }
}

static void print_elements(void *array, size_t size) {
	for (long unsigned int i = 0; i < (size / sizeof(uint32_t)); i++) {
		printf("%10u ", ((uint32_t *)array)[i]);

		if ((i + 1) % 8 == 0)
			printf("\n");
	}
	printf("\n");
}


static int test_filter(struct acctest_context *ctx, size_t buf_size, int tflags,
		       int extra_flags_2, int extra_flags_3, uint32_t opcode, int num_desc)
{
	struct task_node *tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int itr = num_desc, i = 0, range = 0;

	info("test filter: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     opcode, buf_size, tflags, num_desc);

	ctx->is_batch = 0;

	if (ctx->dedicated == ACCFG_WQ_SHARED)
		range = ctx->threshold;
	else
		range = ctx->wq_size;

	while (itr > 0 && rc == ACCTEST_STATUS_OK) {
		i = (itr < range) ? itr : range;
		/* Allocate memory to all the task nodes, desc, completion record*/
		rc = acctest_alloc_multiple_tasks(ctx, i);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		/* allocate memory to src and dest buffers and fill in the desc for all the nodes*/
		tsk_node = ctx->multi_task_node;
		while (tsk_node) {
			tsk_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;
			tsk_node->tsk->iaa_num_inputs = (uint32_t)extra_flags_3;

			rc = init_task(tsk_node->tsk, tflags, opcode, buf_size, 0);
			if (rc != ACCTEST_STATUS_OK)
				return rc;
			if(print_contents) {
				printf("Filter source:\n");
				print_elements(tsk_node->tsk->src1, tsk_node->tsk->xfer_size);
			}
			tsk_node = tsk_node->next;
		}
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = iaa_scan_multi_task_nodes(ctx);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		scan_lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
			((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		/* Verification of all the nodes*/
		rc = iaa_task_result_verify_task_nodes(ctx, 0);
		if (rc != ACCTEST_STATUS_OK){
			return rc;
		}
		//init select
		tsk_node = ctx->multi_task_node;
		while(tsk_node) {
			memset_pattern(tsk_node->tsk->src2, 0, IAA_FILTER_MAX_SRC2_SIZE);
			memset_pattern(tsk_node->tsk->output, 0, IAA_FILTER_MAX_SRC2_SIZE);
			memcpy(tsk_node->tsk->src2, tsk_node->tsk->dst1,
		       tsk_node->tsk->comp->iax_output_size);
			//memset_pattern(tsk_node->tsk->src2, 0xa5a5a5a55a5a5a5a, IAA_FILTER_MAX_SRC2_SIZE);
			tsk_node->tsk->iaa_src2_xfer_size = IAA_FILTER_MAX_SRC2_SIZE;
			memset_pattern(tsk_node->tsk->dst1, 0, IAA_FILTER_MAX_DEST_SIZE);
			tsk_node->tsk->dflags &= ~IDXD_OP_FLAG_RD_SRC2_AECS;
			tsk_node->tsk->opcode = IAX_OPCODE_SELECT;
			tsk_node = tsk_node->next;
		}
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = iaa_select_multi_task_nodes(ctx);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		select_lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
			((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		if (rc != ACCTEST_STATUS_OK)
			return rc;		
		tsk_node = ctx->multi_task_node;
		while(tsk_node) {
			if(print_contents) {
				printf("Filter destination:\n");
				print_elements(tsk_node->tsk->dst1, tsk_node->tsk->comp->iax_output_size);
			}
			memcpy_src1 = tsk_node->tsk->dst1;
			memcpy_size = tsk_node->tsk->comp->iax_output_size;
			// printf("memcpy_size: %d\n", memcpy_size);
			tsk_node = tsk_node->next;
		}

		/* Verification of all the nodes*/
		rc = iaa_task_result_verify_task_nodes(ctx, 0);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		//filter_free_task(ctx);
		ctx->multi_task_node = NULL;
		itr = itr - range;
	}
	return rc;
}

static int test_memcpy(struct acctest_context *dsa, size_t buf_size,
					int tflags, uint32_t dsa_opcode, int num_desc)
{
	struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int itr = num_desc, i = 0, range = 0;

	info("testmemory: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     dsa_opcode, buf_size, tflags, num_desc);

	dsa->is_batch = 0;

	if (dsa->dedicated == ACCFG_WQ_SHARED)
		range = dsa->threshold;
	else
		range = dsa->wq_size;

	while (itr > 0 && rc == ACCTEST_STATUS_OK) {
		i = (itr < range) ? itr : range;
		/*DSA: Allocate memory to all the task nodes, desc, completion record*/
		rc = acctest_alloc_multiple_tasks(dsa, i);
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		/* DSA: allocate memory to src and dest buffers and fill in the desc for all the nodes*/
		dsa_tsk_node = dsa->multi_task_node;

		while (dsa_tsk_node) {
			dsa_tsk_node->tsk->xfer_size = memcpy_size;
			rc = memcpy_init(dsa_tsk_node->tsk, tflags, dsa_opcode, memcpy_size);
			if (rc != ACCTEST_STATUS_OK)
				return rc;

			dsa_tsk_node = dsa_tsk_node->next;
		}
		dsa_tsk_node = dsa->multi_task_node;

		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = dsa_memcpy_multi_task_nodes(dsa);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		memcpy_lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
					((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		/* Verification of all the nodes*/
		rc = task_result_verify_task_nodes(dsa, 0);
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		dsa_tsk_node = dsa->multi_task_node;
		while(dsa_tsk_node) {
			if(print_contents) {
				printf("Memcpy destination:\n");
				print_elements(dsa_tsk_node->tsk->dst1, memcpy_size);
			}
			dsa_tsk_node = dsa_tsk_node->next;
		}

		acctest_free_task(dsa);
		itr = itr - range;
	}

	return rc;
}


int main(int argc, char *argv[])
{
	struct acctest_context *dsa, *iaa;
	int rc = 0;
	unsigned long buf_size = DSA_TEST_SIZE;
	int wq_type = DEDICATED;
	int dsa_opcode = DSA_OPCODE_MEMMOVE;
	int iaa_opcode = IAX_OPCODE_SCAN;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int wq_id = ACCTEST_DEVICE_ID_NO_INPUT;
	int dev_id = ACCTEST_DEVICE_ID_NO_INPUT;
	unsigned int num_desc = 1;
	unsigned int num_iter = 1;
	// int extra_flags_1 = 0;
	int extra_flags_2 = 0x7c;
	int extra_flags_3 = 0;

	while ((opt = getopt(argc, argv, "w:l:i:t:vh")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		case 'l':
			buf_size = strtoul(optarg, NULL, 0);
			break;
		case 'i':
			num_iter = strtoul(optarg, NULL, 0);
			break;
		case 't':
			ms_timeout = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			debug_logging = 1;
			break;
		default:
			break;
		}
	}
	extra_flags_3 = buf_size/4;
	printf("size = %ld, num_iter = %u, num_elements = %d\n", buf_size, num_iter, extra_flags_3);

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

	for(unsigned int i = 0; i < num_iter; i++) {
		rc = test_filter(iaa, buf_size, tflags, extra_flags_2,
			extra_flags_3, iaa_opcode, num_desc);
		if (rc != ACCTEST_STATUS_OK)
			goto error;
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		shuffle_elements(memcpy_src1, memcpy_size);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		shuffle_lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
		   ((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		rc = test_memcpy(dsa, buf_size, tflags, dsa_opcode, num_desc);
		if (rc != ACCTEST_STATUS_OK)
			goto error;
	}
	printf("Scan latency: %llu\n", scan_lat/num_iter);
	printf("Select latency: %llu\n", select_lat/num_iter);
	printf("Shuffle latency: %llu\n", shuffle_lat/num_iter);
	printf("Memcpy latency: %llu\n", memcpy_lat/num_iter);


 error:
	acctest_free(dsa);
	acctest_free(iaa);
	return rc;
}