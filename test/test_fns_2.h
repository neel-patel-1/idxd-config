
typedef struct {
    int num_descs;
    int buf_size;
    int wq_id;
    int serialDepth;
} SerialDSASubmitArgs;


int dsa_single_thread_serialize_granularity(void *args) {
  SerialDSASubmitArgs *threadArgs = (SerialDSASubmitArgs *)args;
  int num_descs = threadArgs->num_descs;
  int buf_size = threadArgs->buf_size;
  int wq_id = threadArgs->wq_id;
  int submitDepth = threadArgs->serialDepth;
  struct acctest_context *dsa;
  struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;
  int wq_depth = 32;

  dsa = acctest_init(tflags);
  rc = acctest_alloc(dsa, 0, 0, wq_id);
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
  if (submitDepth > wq_depth){
    printf("Total number of descriptors greater than work queue depth is n"
    "ot allowed for serialized submit test\n");
    exit(-1);
  }
  printf("wq_depth: %d\n", wq_depth);
  struct timespec times[2];

  pthread_barrier_wait(&barrier);
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  for(int i=0; i<num_iter; i++){
      dsa_submit_and_wait(dsa,submitDepth);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

  uint64_t nanos = (times[1].tv_sec - times[0].tv_sec) * 1000000000 + times[1].tv_nsec - times[0].tv_nsec;
  printf("WQ: %d SerializationGranularity: %d BufSize: %d Throughput: %f GB/s\n",
    wq_id, submitDepth, buf_size, (double)buf_size * num_descs * num_iter / nanos);
  acctest_free_task(dsa);
  acctest_free(dsa);
}