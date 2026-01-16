#ifndef CORE_PROTOCOL_H
#define CORE_PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

// limits and configs
#define shared_mem_name "/chat_app_shm"
#define participant_limit 10
#define msg_char_limit 256
#define convos_limit 10
#define msgs_per_convo_limit 50

// message format
typedef struct {
  int convo_id;
  int source_id;
  long sequence_id;
  int reader_count;

  // checks for activity and termination 
  bool active_check;
  bool end_check;

  char payload[msg_char_limit];
} Message;

// convo format
typedef struct {
  int convo_id;
  int member_count;
  long next_sequence_id;
  bool active_check;
  sem_t mutex; // protects convo data
} Convo;

// shared memory layout
typedef struct {
  Convo convos[convos_limit];
  Message messages[convos_limit * msgs_per_convo_limit];
  sem_t shm_mutex; // protects shared mem
} SharedMemory;

#endif