#pragma once
#include <jpeglib.h>
#include <setjmp.h>

static struct iaa_filter_aecs_t iaa_filter_aecs = {
	.rsvd = 0,
	.rsvd2 = 0,
	.rsvd3 = 0,
	.rsvd4 = 0,
	.rsvd5 = 0,
	.rsvd6 = 0
};

int init_scan_from_image(struct task *tsk, int tflags, int opcode, const char *image_path) {
	int status;
    tsk->opcode = opcode;
    tsk->test_flags = tflags;
    tsk->src1 = malloc(tsk->xfer_size);
    if (status != ACCTEST_STATUS_OK) {
        return status;
    }

	tsk->input = aligned_alloc(32, tsk->xfer_size);
	if (!tsk->input)
		return -ENOMEM;

	memcpy(tsk->input, tsk->src1, tsk->xfer_size);

    tsk->src2 = aligned_alloc(32, IAA_FILTER_MAX_SRC2_SIZE);
	if (!tsk->src2)
		return -ENOMEM;
	memset_pattern(tsk->src2, 0, IAA_FILTER_AECS_SIZE);
	iaa_filter_aecs.low_filter_param = 0x000000;
	iaa_filter_aecs.high_filter_param = 0xFFFFFF;
	memcpy(tsk->src2, (void *)&iaa_filter_aecs, IAA_FILTER_AECS_SIZE);
	tsk->iaa_src2_xfer_size = IAA_FILTER_AECS_SIZE;

	tsk->dst1 = aligned_alloc(ADDR_ALIGNMENT, IAA_FILTER_MAX_DEST_SIZE);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, IAA_FILTER_MAX_DEST_SIZE);

	tsk->iaa_max_dst_size = IAA_FILTER_MAX_DEST_SIZE;

	tsk->output = aligned_alloc(ADDR_ALIGNMENT, IAA_FILTER_MAX_DEST_SIZE);
	if (!tsk->output)
		return -ENOMEM;
	memset_pattern(tsk->output, 0, IAA_FILTER_MAX_DEST_SIZE);

	return ACCTEST_STATUS_OK;
}