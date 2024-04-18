#include <stdatomic.h>
#define PTR_SIZE sizeof(void*)
atomic_int keep_running = 1;

static inline int increment_comp_if_tsk_valid(struct task_node *tsk_node, struct completion_record *comp){
  if(tsk_node){
    if(comp->status){
      tsk_node = tsk_node->next;
      if(tsk_node){
        comp = tsk_node->tsk->comp;
      }
      return 1;
    } else {
      return 0;
    }
  }
  else{
    return 2;
  }
}

void randomize_indices(size_t* indices, size_t len) {
    for (size_t i = 0; i < len - 1; i++) {
        size_t j = i + rand() / (RAND_MAX / (len - i) + 1);
        size_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
}

void** create_random_chain(size_t size) {
    size_t len = size / PTR_SIZE;
    void** memory = malloc(len * PTR_SIZE);
    size_t* indices = malloc(len * sizeof(size_t));

    for (size_t i = 0; i < len; i++) {
        indices[i] = i;
    }

    randomize_indices(indices, len);

    for (size_t i = 1; i < len; i++) {
        memory[indices[i - 1]] = &memory[indices[i]];
    }
    memory[indices[len - 1]] = &memory[indices[0]];

    free(indices);
    return memory;
}

void pointer_chase(void* arg) {
    char* lb = (char*)arg;
    uint64_t bufSize = *((uint64_t*)lb);
    void** chain = create_random_chain(bufSize);

    struct timespec start, end;
    uint64_t totalNanos = 0;
    int iterations = 1000;

    for (int i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        void* ptr = chain[0];
        for (size_t j = 0; j < bufSize / PTR_SIZE; j++) {
            ptr = *((void**)ptr);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);

        uint64_t nanos = (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
        totalNanos += nanos;
    }

    double averageNanos = (double)totalNanos / iterations;
    double throughput = bufSize / averageNanos; 

    printf("Average Throughput: %.3f GB/s\n", throughput);

    free(chain);
    atomic_store_explicit(&keep_running, 0, memory_order_release);
}

void pointer_chase_write_heavy(void* arg) {
    char* lb = (char*)arg;
    uint64_t bufSize = *((uint64_t*)lb);
    void** chain = create_random_chain(bufSize);
    uint64_t* buf = (uint64_t*)malloc(bufSize);

    struct timespec start, end;
    uint64_t totalNanos = 0;
    int iterations = 1000;

    for (int i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        void* ptr = chain[0];
        for (int j = 0; j < bufSize / PTR_SIZE; j++) {
            buf[j] = (uint64_t)ptr;
            ptr = *((void**)ptr);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);

        uint64_t nanos = (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
        totalNanos += nanos;
    }

    double averageNanos = (double)totalNanos / iterations;
    double throughput = bufSize / averageNanos; 

    printf("Average Throughput: %.3f GB/s\n", throughput);

    free(chain);
    atomic_store_explicit(&keep_running, 0, memory_order_release);
}

void read_heavy(void * arg) {
    uint64_t bufSize = (1 << 22); // 4MB
    char *lb = (char *)(arg);
    struct timespec start, end;
    uint64_t totalNanos = 0;
    int iterations = 1000;

    for (int i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_MONOTONIC, &start);

        for (int j = 0; j < bufSize; j++) {
            lb[j] = 'a';  // Perform write operation
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        uint64_t nanos = (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
        totalNanos += nanos;
    }

    double averageNanos = (double)totalNanos / iterations;
    double throughput = bufSize / averageNanos; // Convert bytes per nanosecond to GB/s

    printf("Average Throughput: %.3f GB/s\n", throughput);
    atomic_store_explicit(&keep_running, 0, memory_order_release);
}

void busy_wait_spt(void *arg){
  volatile int *v = malloc(sizeof(int));
  pthread_mutex_t lock;
  pthread_mutex_init(&lock, NULL);
  while (atomic_load_explicit(&keep_running, memory_order_acquire)) {
    if (*v == 0){
      pthread_mutex_lock(&lock);
      v++;
      pthread_mutex_unlock(&lock);
    }
  }
}

int spt_int(int config, int bufSize, int busy_wait){
  uint64_t buf = (uint64_t)malloc(bufSize);
  #define SPT_THREAD 1
  #define FOREGROUND_THREAD 41

  cpu_set_t cpuset;
  pthread_t read_heavy_thread, busy_wait_thread;
  pthread_t pointer_chase_thread, write_heavy_thread;

  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  if(busy_wait) {
      CPU_ZERO(&cpuset);
      CPU_SET(SPT_THREAD, &cpuset);
      pthread_create(&busy_wait_thread, NULL, busy_wait_spt, (void *) buf);
      pthread_setaffinity_np(busy_wait_thread, sizeof(cpu_set_t), &cpuset);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(FOREGROUND_THREAD, &cpuset);

  if(config == 0) {
    pthread_create(&read_heavy_thread, NULL, read_heavy, (void *) buf);
    pthread_setaffinity_np(read_heavy_thread, sizeof(cpu_set_t), &cpuset);
    pthread_join(read_heavy_thread, NULL);
  } else if(config == 1) {
    pthread_create(&pointer_chase_thread, NULL, (void *)pointer_chase, (void *)&bufSize);
    pthread_setaffinity_np(pointer_chase_thread, sizeof(cpu_set_t), &cpuset);
    pthread_join(pointer_chase_thread, NULL);
  } else if(config == 2) {
    pthread_create(&write_heavy_thread, NULL, (void *)pointer_chase_write_heavy, (void *)&bufSize);
    pthread_setaffinity_np(write_heavy_thread, sizeof(cpu_set_t), &cpuset);
    pthread_join(write_heavy_thread, NULL);
  }

  pthread_join(busy_wait_thread, NULL);
  clock_gettime(CLOCK_MONOTONIC, &times[1]);
}


int multi_iaa_test(int num_iaas, int tflags, int wq_type, int dev_id, int wq_id, size_t buf_size, int num_desc)
{
  int rc;
  struct acctest_context **iaa;
  iaa = malloc(num_iaas * sizeof(struct acctest_context *));

  for(int i=0; i<num_iaas; i++){
    iaa[i] = acctest_init(tflags);
    iaa[i]->dev_type = ACCFG_DEVICE_IAX;
    if (!iaa[i])
      return -ENOMEM;

    rc = acctest_alloc(iaa[i], wq_type, dev_id, wq_id);
    if (rc < 0)
      return -ENOMEM;

    if (buf_size > iaa[i]->max_xfer_size) {
      err("invalid transfer size: %lu\n", buf_size);
      return -EINVAL;
    }
  }

  for(int i=0; i<num_iaas; i++){
    rc = init_iaa_task_nodes(iaa[i], buf_size, tflags, num_desc);
    if (rc != ACCTEST_STATUS_OK)
      return rc;
  }

  struct task_node *iaa_tsk_node[num_iaas];
  struct completion_record *next_iaa_comp[num_iaas];

  /* Submission / work distribution across all iaa instances*/
  for(int i=0; i<num_iaas; i++){
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
    while(iaa_tsk_node[i]){
      iaa_prep_sub_task_node(iaa[i], iaa_tsk_node[i]);
      iaa_tsk_node[i] = iaa_tsk_node[i]->next;
    }
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
    next_iaa_comp[i] = iaa_tsk_node[i]->tsk->comp;
  }

  /* Many IAAs to Poll, don't block on any one IAA */

  int complete = 0;
  while (!complete) {
    complete = 1;
    for(int i=0; i<num_iaas; i++){
      if(iaa_tsk_node[i]){
        complete = 0;
        if(next_iaa_comp[i]->status){
          iaa_tsk_node[i] = iaa_tsk_node[i]->next;
          if(iaa_tsk_node[i]){
            next_iaa_comp[i] = iaa_tsk_node[i]->tsk->comp;
          }
        }
      }
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &times[1]);

}

typedef struct{
  int tflags;
  int wq_type;
  int dev_id;
  int wq_id;
  size_t buf_size;
  int num_desc;
} single_submitter_poller_args;

int single_iaa_test( void *arg){
  /* allocate this iaa's task nodes */
  int rc;
  struct acctest_context *iaa;
  single_submitter_poller_args *args = (single_submitter_poller_args *)arg;

  int tflags = args->tflags;
  int wq_type = args->wq_type;
  int dev_id = args->dev_id;
  int wq_id = args->wq_id;
  size_t buf_size = args->buf_size;
  int num_desc = args->num_desc;
  struct task_node *iaa_tsk_node;

  /* setup */
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
  init_iaa_task_nodes(iaa, buf_size, tflags, num_desc);
  iaa_tsk_node = iaa->multi_task_node;

  while(!test_started){}
  /* submit */
  while(iaa_tsk_node){
    iaa_prep_sub_task_node(iaa, iaa_tsk_node);
    iaa_tsk_node = iaa_tsk_node->next;
  }

  /* Reset and poll in order */
  iaa_tsk_node = iaa->multi_task_node;
  struct completion_record *next_iaa_comp = iaa_tsk_node->tsk->comp;
  while(iaa_tsk_node){
    if(next_iaa_comp->status){
      iaa_tsk_node = iaa_tsk_node->next;
      if(iaa_tsk_node){
        next_iaa_comp = iaa_tsk_node->tsk->comp;
      }
    }
  }
  pthread_exit((void *)ACCTEST_STATUS_OK);

}

int multi_iaa_bandwidth(int num_wqs, int num_descs, int buf_size){
  single_submitter_poller_args *args = malloc(num_wqs * sizeof(single_submitter_poller_args));
  pthread_t threads[num_wqs];
  for(int i=0; i<num_wqs; i++){
    args[i].tflags = 0;
    args[i].wq_type = 0;
    args[i].dev_id = 1;
    args[i].wq_id = i;
    args[i].buf_size = buf_size;
    args[i].num_desc = num_descs;
    pthread_create(&(threads[i]),NULL,single_iaa_test,(void *)&(args[i]));
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i+1, &cpuset);
    pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  test_started = 1;
  for(int i=0; i<num_wqs; i++){
    pthread_join(threads[i],NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

}

int reset_test_ctrs(){
  intermediate_host_ops_complete = 0;
  finalHostOpCtr = 0;
}

int dsa_memcpy_poll_task_comps(struct acctest_context *dsa){
  struct task_node *tsk_node = dsa->multi_task_node;
  struct completion_record *comp = tsk_node->tsk->comp;
  while(tsk_node){
    if(comp->status == DSA_COMP_SUCCESS){
      tsk_node = tsk_node->next;
      if(tsk_node){
        comp = tsk_node->tsk->comp;
      }
    }
  }
}



int single_dsa_test( void *arg){
  /* allocate this iaa's task nodes */
  int rc;
  struct acctest_context *dsa;
  single_submitter_poller_args *args = (single_submitter_poller_args *)arg;

  int tflags = args->tflags;
  int wq_type = args->wq_type;
  int dev_id = args->dev_id;
  int wq_id = args->wq_id;
  size_t buf_size = args->buf_size;
  int num_desc = args->num_desc;
  struct task_node *dsa_tsk_node;

  /* setup */
  dsa = acctest_init(tflags);
  dsa->dev_type = ACCFG_DEVICE_DSA;
  if (!dsa)
    return -ENOMEM;
  rc = acctest_alloc(dsa, wq_type, dev_id, wq_id);
  if (rc < 0){
    exit(-ENODEV);
  }
  if (buf_size > dsa->max_xfer_size) {
    err("invalid transfer size: %lu\n", buf_size);
    return -EINVAL;
  }
  init_dsa_task_nodes(dsa, buf_size, tflags, num_desc);
  dsa_tsk_node = dsa->multi_task_node;

  while(!test_started){}
  for(int i=0; i<num_iter; i++){
    /* submit */
    dsa_memcpy_submit_task_nodes(dsa);


    /* Reset and poll in order */
    dsa_memcpy_poll_task_comps(dsa);
  }
  pthread_exit((void *)dsa->multi_task_node);

}

int multi_dsa_bandwidth(int num_wqs, int num_descs, int buf_size){
  single_submitter_poller_args *args = malloc(num_wqs * sizeof(single_submitter_poller_args));
  pthread_t threads[num_wqs];
  for(int i=0; i<num_wqs; i++){
    args[i].tflags = 0;
    args[i].wq_type = 0;
    args[i].dev_id = 0;
    args[i].wq_id = i;
    args[i].buf_size = buf_size;
    args[i].num_desc = num_descs;
    pthread_create(&(threads[i]),NULL,single_dsa_test,(void *)&(args[i]));
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i+1, &cpuset);
    pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  test_started = 1;
  struct task_node *dsa_tsk_node[num_wqs];
  for(int i=0; i<num_wqs; i++){
    pthread_join(threads[i],&(dsa_tsk_node[i]));
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);
  // while(dsa_tsk_node[0]){
  //   if( ACCTEST_STATUS_OK != task_result_verify(dsa_tsk_node, num_wqs)){
  //     error("Task result verification failed");
  //     return -1;
  //   }
  //   dsa_tsk_node[0] = dsa_tsk_node[0]->next;
  // }

}