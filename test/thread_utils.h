//thread_utils.h
#pragma once

typedef struct{
  int nKWorkers;
} parallelTdOps;

pthread_t *kWorkers =NULL;
kWorkerArgs **pKArgs = NULL;
void createKWorkers(opRing ***pRings, int numKWorkers, int startCPU){
  pthread_t cbTd;
	kWorkers = malloc(sizeof(pthread_t) * numKWorkers);
	pKArgs = malloc(sizeof(kWorkerArgs*) * numKWorkers);
  opRing **rings = NULL;
  kWorkerArgs *kArgs;
  opRing *ring;

  rings = malloc(sizeof(opRing*) * numKWorkers);
  for(int i=0; i< numKWorkers; i++){
    ring = malloc(sizeof(opRing));
    memset(ring, 0, sizeof(opRing));
    kArgs = malloc(sizeof(kWorkerArgs));
    memset(kArgs, 0, sizeof(kWorkerArgs));
    kArgs->ring = ring;
		kArgs->cancelled = 0;
		pKArgs[i] = kArgs;
    pthread_mutex_init(&ring->lock, NULL);

		pthread_attr_t attrs;
		pthread_attr_init(&attrs);
		pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_JOINABLE);

    pthread_create(&cbTd, &attrs, app_worker_thread, (void *)kArgs);
		kWorkers[i] = cbTd;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(startCPU+i, &cpuset);
    pthread_setaffinity_np(cbTd, sizeof(cpu_set_t), &cpuset);
    rings[i] = ring;

  }
  *pRings = rings;
}

void joinKWorkers(int numKWorkers){
	for(int i=0; i< numKWorkers; i++){
		pKArgs[i]->cancelled = 1;
		pthread_join(kWorkers[i], NULL);
	}
}


/* Stage Operation Functions */
/* To be called instantiated as a pthread or called in a serial chain test */

int submit_memcpy_ops(struct acctest_context *dsa){
  int rc = 0;
	clock_gettime(CLOCK_MONOTONIC, &times[0]);
	rc = dsa_memcpy_submit_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		pthread_exit((void *)(intptr_t)rc);
}

int memcpy_and_forward(struct acctest_context *dsa, struct acctest_context *iaa, int nKWorkers, opRing **ring){
  int rc;
  struct task_node *dsa_tsk_node, *iaa_tsk_node;

  host_op_args *args;
	dsa_tsk_node = dsa->multi_task_node;
	iaa_tsk_node = iaa->multi_task_node;
  int idx = 0;
  while (dsa_tsk_node) {
			rc = dsa_wait_memcpy(dsa, dsa_tsk_node->tsk);
			if (rc != ACCTEST_STATUS_OK)
					pthread_exit((void *)(intptr_t)rc);

      args = malloc(sizeof(host_op_args));
			if (args == NULL) {
					pthread_exit((void *)(intptr_t)ENOMEM);  // Handle memory allocation failure
			}
			int *(*selected_op)(void *buffer, size_t size) = select_host_op(host_op_sel);
			if(do_spt_spinup){
				args->host_op = selected_op;
				args->buffer = dsa_tsk_node->tsk->dst1;
				args->size = buf_size;
				enqueue(ring[idx], args);
				idx = (idx + 1) % nKWorkers;

			} else {
				selected_op(dsa_tsk_node->tsk->dst1, buf_size);
				// no atomic ctr update from thread -- do it here
				finalHostOpCtr += 1;
				if(finalHostOpCtr == expectedHostOps){
					intermediate_host_ops_complete = 1;
				}
			}

        // Continue with other operations
        iaa_tsk_node->tsk->src1 = dsa_tsk_node->tsk->dst1;
        iaa_prep_sub_task_node(iaa, iaa_tsk_node);

        dsa_tsk_node = dsa_tsk_node->next;
        iaa_tsk_node = iaa_tsk_node->next;
  }
  pthread_exit((void *)ACCTEST_STATUS_OK);

}


/* Initialize the task nodes with buffers and work description/ors*/
int init_dsa_task_nodes( struct acctest_context *dsa, size_t buf_size, int tflags, int num_desc){
  int rc;
  struct task_node *dsa_tsk_node;


  rc = acctest_alloc_multiple_tasks(dsa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

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

/* Initialize the task nodes with buffers and work description/ors*/
int init_iaa_task_nodes( struct acctest_context *iaa, size_t buf_size, int tflags, int num_desc){
  int rc;
  struct task_node *iaa_tsk_node;


  rc = acctest_alloc_multiple_tasks(iaa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

  iaa_tsk_node = iaa->multi_task_node;
  while (iaa_tsk_node) {
		iaa_tsk_node->tsk->xfer_size = buf_size;

		rc = iaa_init_task(iaa_tsk_node->tsk, tflags, IAX_OPCODE_COMPRESS, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		iaa_tsk_node = iaa_tsk_node->next;
	}
	return rc;
}

int init_iaa_dsa_task_nodes(struct acctest_context **pdsa, struct acctest_context **piaa, size_t buf_size, int num_desc, int tflags, int wq_type, int dev_id, int wq_id){
  struct task_node *dsa_tsk_node, *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
  struct acctest_context *dsa = NULL;
  struct acctest_context *iaa = NULL;

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

	info("testmemory: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     DSA_OPCODE_MEMMOVE, buf_size, tflags, num_desc);

	(dsa)->is_batch = 0;

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
  *pdsa = dsa;
  *piaa = iaa;
	return rc;
}