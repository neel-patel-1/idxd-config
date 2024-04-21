static inline void dsa_streaming_submission(struct acctest_context *dsa, int numSubs, int wq_depth){
  int rc;
  int submission_count = 0;
  struct task_node *tsk_node = dsa->multi_task_node;
  struct task_node *start_tsk_node = tsk_node;

  /* Prep all task nodes */
  while (tsk_node) {
		tsk_node->tsk->dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((tsk_node->tsk->test_flags & TEST_FLAGS_BOF) && dsa->bof)
			tsk_node->tsk->dflags |= IDXD_OP_FLAG_BOF;
		dsa_prep_memcpy(tsk_node->tsk);
		tsk_node = tsk_node->next;
	}

  tsk_node = dsa->multi_task_node;


  /* Submit up to the work queue depth */
  int submitted = 0;
  while(tsk_node && submitted < wq_depth){
    if (tsk_node->tsk->test_flags & TEST_FLAGS_CPFLT)
			madvise(tsk_node->tsk->comp, 4096, MADV_DONTNEED);
		acctest_desc_submit(dsa, tsk_node->tsk->desc);
    tsk_node = tsk_node->next;
    submitted++;
  }

  /* Streaming submission as completions arrive */
  struct task_node *next_to_complete = dsa->multi_task_node;
  while(tsk_node){
    dsa_wait_memcpy(dsa, next_to_complete->tsk);
    next_to_complete = next_to_complete->next;
    if (tsk_node->tsk->test_flags & TEST_FLAGS_CPFLT)
			madvise(tsk_node->tsk->comp, 4096, MADV_DONTNEED);
		acctest_desc_submit(dsa, tsk_node->tsk->desc);
    tsk_node = tsk_node->next;
    // submitted++;
  }
  // printf("Submitted: %d\n", submitted);

  /* Collect last batch */
  while(next_to_complete){
    dsa_wait_memcpy(dsa, next_to_complete->tsk);
    next_to_complete = next_to_complete->next;
  }

  return 0;
}

int dsa_streaming_submit(void *args) {
  SerialDSASubmitArgs *threadArgs = (SerialDSASubmitArgs *)args;
  int buf_size = threadArgs->buf_size;
  int wq_id = threadArgs->wq_id;
  int submitDepth = threadArgs->serialDepth;
  int wq_depth = threadArgs->wq_depth;
  struct acctest_context *dsa;
  struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

  dsa = acctest_init(tflags);
  rc = acctest_alloc(dsa, 0, threadArgs->dev_id, wq_id);
  if(ACCTEST_STATUS_OK != rc){
    printf("Failed to allocate DSA\n");
    exit(-1);
  }
  dsa->is_batch = 0;
  rc = acctest_alloc_multiple_tasks(dsa, submitDepth);
  if (rc != ACCTEST_STATUS_OK)
		return rc;
  printf("Allocated tasks\n");

  dsa_tsk_node = dsa->multi_task_node;
	while (dsa_tsk_node) {
		dsa_tsk_node->tsk->xfer_size = buf_size;

		rc = dsa_init_task(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		dsa_tsk_node = dsa_tsk_node->next;
	}
  printf("Starting test\n");
  printf("wq_depth: %d\n", wq_depth);
  struct timespec times[2];

  pthread_barrier_wait(&barrier);
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  for(int i=0; i<num_iter; i++){
      dsa_streaming_submission(dsa,submitDepth, wq_depth);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

  uint64_t nanos = (times[1].tv_sec - times[0].tv_sec) * 1000000000 + times[1].tv_nsec - times[0].tv_nsec;
  printf("WQ: %d SerializationGranularity: %d BufSize: %d Throughput: %f GB/s\n",
    wq_id, submitDepth, buf_size, (double)buf_size * submitDepth * num_iter / nanos);
  acctest_free_task(dsa);
  acctest_free(dsa);
}