//thread_utils.h
#pragma once

void createKWorkers(opRing **rings, int numKWorkers){
  pthread_t cbTd;
	opRing *ring = malloc(sizeof(opRing));
	memset(ring, 0, sizeof(opRing));
	kWorkerArgs *kArgs = malloc(sizeof(kWorkerArgs));
	memset(kArgs, 0, sizeof(kWorkerArgs));
	kArgs->ring = ring;
	pthread_mutex_init(&ring->lock, NULL);

	pthread_create(&cbTd, NULL, app_worker_thread, kArgs);
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(5, &cpuset);
	pthread_setaffinity_np(cbTd, sizeof(cpu_set_t), &cpuset);
}