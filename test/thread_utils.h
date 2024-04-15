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