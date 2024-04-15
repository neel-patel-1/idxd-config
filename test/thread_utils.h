//thread_utils.h
#pragma once

typedef struct{
  int nKWorkers;
} parallelTdOps;

void createKWorkers(opRing ***pRings, int numKWorkers, int startCPU){
  pthread_t cbTd;
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
    pthread_mutex_init(&ring->lock, NULL);

    pthread_create(&cbTd, NULL, app_worker_thread, (void *)kArgs);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(startCPU+i, &cpuset);
    pthread_setaffinity_np(cbTd, sizeof(cpu_set_t), &cpuset);
    rings[i] = ring;

  }
  *pRings = rings;
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
					complete = 1;
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