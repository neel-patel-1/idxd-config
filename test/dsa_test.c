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

#define DSA_TEST_SIZE 4098
#define IAA_COMPRESS_AECS_SIZE (1568)
#define IAA_COMPRESS_SRC2_SIZE (IAA_COMPRESS_AECS_SIZE * 2)
#define IAA_COMPRESS_MAX_DEST_SIZE (2097152 * 2)
#

struct acctest_context *dsa, *iaa;
int dsa_opcode = DSA_OPCODE_MEMMOVE;
int iaa_opcode = IAX_OPCODE_COMPRESS;

static int init_compress(struct task *tsk, int tflags, int opcode, 
						unsigned long src1_xfer_size)
{
	tsk->pattern = 0x98765432abcdef01;
	tsk->opcode = opcode;
	tsk->test_flags = tflags;
	tsk->xfer_size = src1_xfer_size;

	tsk->src2 = aligned_alloc(32, IAA_COMPRESS_SRC2_SIZE);
	if (!tsk->src2)
		return -ENOMEM;
	memset_pattern(tsk->src2, 0, IAA_COMPRESS_SRC2_SIZE);

	tsk->dst1 = aligned_alloc(32, IAA_COMPRESS_MAX_DEST_SIZE);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, IAA_COMPRESS_MAX_DEST_SIZE);

	tsk->output = aligned_alloc(32, IAA_COMPRESS_MAX_DEST_SIZE);
	if (!tsk->output)
		return -ENOMEM;
	memset_pattern(tsk->output, 0, IAA_COMPRESS_MAX_DEST_SIZE);

	return ACCTEST_STATUS_OK;
}

static int setup_dsa_iaa(int buf_size, int num_desc) {
	struct task_node *dsa_tsk_node, *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

	info("testmemory: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     dsa_opcode, buf_size, tflags, num_desc);

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

		rc = dsa_init_task(dsa_tsk_node->tsk, tflags, dsa_opcode, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		rc = init_compress(iaa_tsk_node->tsk, tflags, iaa_opcode, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		iaa_tsk_node = iaa_tsk_node->next;
		dsa_tsk_node = dsa_tsk_node->next;
	}
	return rc;
}

static int restruc_func(void) {
	struct task_node *dsa_tsk_node, *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int itr = 0;
	dsa_tsk_node = dsa->multi_task_node;
	iaa_tsk_node = iaa->multi_task_node;
	while (dsa_tsk_node) {
		rc = dsa_wait_memcpy(dsa, dsa_tsk_node->tsk);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		iaa_tsk_node->tsk->src1 = dsa_tsk_node->tsk->dst1;
		iaa_prep_sub_task_node(iaa, iaa_tsk_node);
		dsa_tsk_node = dsa_tsk_node->next;
		iaa_tsk_node = iaa_tsk_node->next;
	}
	iaa_tsk_node = iaa->multi_task_node;
	while(iaa_tsk_node) {
		rc = iaa_wait_compress(iaa, iaa_tsk_node->tsk);
		if (rc != ACCTEST_STATUS_OK)
			info("Desc: %p failed with ret: %d\n",
			     iaa_tsk_node->tsk->desc, iaa_tsk_node->tsk->comp->status);
		iaa_tsk_node = iaa_tsk_node->next;
	}

	/* Verification of all the nodes*/
	rc = task_result_verify_task_nodes(dsa, 0);
	if (rc != ACCTEST_STATUS_OK)
		return rc;
	/* Verification of all the nodes*/
	rc = task_result_verify_task_nodes(iaa, 0);
	if (rc != ACCTEST_STATUS_OK)
		return rc;
	acctest_free_task(dsa);
	return rc;
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
	unsigned int num_iter = 1;
	struct task_node *dsa_tsk_node;

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
	rc = dsa_memcpy_submit_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		goto error;
	rc =  restruc_func();
	if (rc != ACCTEST_STATUS_OK)
		goto error;

 error:
	acctest_free(dsa);
	acctest_free(iaa);
	return rc;
}