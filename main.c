#include "core_protocol.h"
#include <signal.h>
#include <time.h>

// global variables
SharedMemory *g_shm = NULL;
int g_shm_fd = -1;
int current_convo_idx = -1;
int process_id = 0;
volatile bool is_running = true;
pthread_t thread_send, thread_recv;

// clean up memory when everything is done
void shutdown_resources() {
    if (g_shm && g_shm != MAP_FAILED) {
        sem_wait(&g_shm->shm_mutex);
        
        bool active_conversations = false;
        for (int i = 0; i < convos_limit; i++) {
            if (g_shm->convos[i].active_check) {
                active_conversations = true;
                break;
            }
        }

        if (!active_conversations) {
            shm_unlink(shared_mem_name);
            printf("shared mem has been cleaned up.\n");
        }
        
        sem_post(&g_shm->shm_mutex);
        munmap(g_shm, sizeof(SharedMemory));
    }

    if (g_shm_fd != -1) {
        close(g_shm_fd);
    }
}

// handle ctrl+c
void signal_handler(int sig_num) {
    printf("\nsignal caught %d. exiting..\n", sig_num);
    is_running = false;
    pthread_cancel(thread_send);
    pthread_cancel(thread_recv);
}

// find spot for  new message
int get_available_msg_slot() {
    for (int idx = 0; idx < convos_limit * msgs_per_convo_limit; idx++) {
        if (!g_shm->messages[idx].active_check) {
            return idx;
        }
    }
    return -1;
}

// thread to read messages
void *listen_for_messages(void *arg) {
    long last_seen_seq = 0;

    while (is_running) {
        if (current_convo_idx == -1) {
            usleep(100000); 
            continue;
        }

        Convo *active_convo = &g_shm->convos[current_convo_idx];

        for (int k = 0; k < convos_limit * msgs_per_convo_limit; k++) {
            // quick check without lock
            if (g_shm->messages[k].active_check && 
                g_shm->messages[k].convo_id == current_convo_idx && 
                g_shm->messages[k].sequence_id > last_seen_seq) {

                // lock it up
                sem_wait(&active_convo->mutex);

                Message *msg_ptr = &g_shm->messages[k];

                // double check
                if (msg_ptr->active_check && 
                    msg_ptr->convo_id == current_convo_idx && 
                    msg_ptr->sequence_id > last_seen_seq) {

                    if (msg_ptr->source_id != process_id) {
                        if (msg_ptr->end_check) {
                            printf("\n[System] User %d left. Exiting...\n", msg_ptr->source_id);
                            is_running = false;
                            msg_ptr->reader_count++;
                            sem_post(&active_convo->mutex);
                            
                            pthread_cancel(thread_send);
                            return NULL;
                        } else {
                            printf("\n[User %d]: %s\n", msg_ptr->source_id, msg_ptr->payload);
                            fflush(stdout);
                        }
                    }

                    last_seen_seq = msg_ptr->sequence_id;

                    // update read count
                    if (msg_ptr->source_id != process_id) {
                        msg_ptr->reader_count++;
                        if (msg_ptr->reader_count >= (active_convo->member_count - 1)) {
                            msg_ptr->active_check = false; // delete  it
                        }
                    }
                }
                sem_post(&active_convo->mutex);
            }
        }
        usleep(200000); // sleep for a bit
    }
    return NULL;
}

// thread to send messages
void *process_user_input(void *arg) {
    char input_buf[msg_char_limit];

    while (is_running) {
        if (current_convo_idx == -1) {
            sleep(1);
            continue;
        }

        if (fgets(input_buf, sizeof(input_buf), stdin) != NULL) {
            // remove newline
            size_t len = strlen(input_buf);
            if (len > 0 && input_buf[len - 1] == '\n') {
                input_buf[len - 1] = '\0';
            }

            bool terminate_cmd = (strcmp(input_buf, "TERMINATE") == 0);

            // find a slot
            sem_wait(&g_shm->shm_mutex);
            int slot_idx = get_available_msg_slot();
            if (slot_idx != -1) {
                g_shm->messages[slot_idx].active_check = true;
                g_shm->messages[slot_idx].convo_id = -1;
                g_shm->messages[slot_idx].reader_count = 0;
            }
            sem_post(&g_shm->shm_mutex);

            if (slot_idx == -1) {
                printf("Error: No space for msg!\n");
                continue;
            }

            // fill in     
            Convo *c = &g_shm->convos[current_convo_idx];
            sem_wait(&c->mutex);

            g_shm->messages[slot_idx].source_id = process_id;
            g_shm->messages[slot_idx].sequence_id = ++c->next_sequence_id;

            if (terminate_cmd) {
                strcpy(g_shm->messages[slot_idx].payload, "TERMINATE");
                g_shm->messages[slot_idx].end_check = true;
            } else {
                strncpy(g_shm->messages[slot_idx].payload, input_buf, msg_char_limit);
                g_shm->messages[slot_idx].end_check = false;
            }


            g_shm->messages[slot_idx].convo_id = current_convo_idx;
            sem_post(&c->mutex);

            if (terminate_cmd) {
                printf("chat ended\n");
                is_running = false;
                pthread_cancel(thread_recv);
                break;
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    process_id = getpid();
    int choice;
    char input_tmp[16];

    // open shared mem
    g_shm_fd = shm_open(shared_mem_name, O_RDWR | O_CREAT, 0666);
    if (g_shm_fd == -1) {
        perror("shm_open failed");
        exit(1);
    }

    struct stat s;
    fstat(g_shm_fd, &s);
    bool is_creator = (s.st_size == 0);

    if (ftruncate(g_shm_fd, sizeof(SharedMemory)) == -1) {
        perror("ftruncate  failed");
        exit(1);
    }

    g_shm = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    if (is_creator) {
        memset(g_shm, 0, sizeof(SharedMemory));
        if (sem_init(&g_shm->shm_mutex, 1, 1) == -1) {
            perror("sem_init failed");
            exit(1);
        }
        printf("sharedmem made\n");
    }

    // menu
    printf("welcome pid %d\n", process_id);
    printf("1) make new convo\n");
    printf("2) join convo\n");
    printf("pick: ");

    if (fgets(input_tmp, sizeof(input_tmp), stdin)) {
        if (sscanf(input_tmp, "%d", &choice) != 1) {
            printf("wrong input\n");
            exit(1);
        }
    } else {
        exit(1);
    }

    sem_wait(&g_shm->shm_mutex);
    if (choice == 1) {
        for (int i = 0; i < convos_limit; i++) {
            if (!g_shm->convos[i].active_check) {
                current_convo_idx = i;
                g_shm->convos[i].active_check = true;
                g_shm->convos[i].convo_id = i;
                g_shm->convos[i].member_count = 1;
                g_shm->convos[i].next_sequence_id = 0;
                sem_init(&g_shm->convos[i].mutex, 1, 1);

                // clear old messages 
                for (int m = 0; m < convos_limit * msgs_per_convo_limit; m++) {
                    if (g_shm->messages[m].convo_id == i) {
                        g_shm->messages[m].active_check = false;
                    }
                }

                printf("made the following convo id: %d\n", i);
                break;
            }
        }
        if (current_convo_idx == -1) {
            printf("no slots\n");
            sem_post(&g_shm->shm_mutex);
            exit(1);
        }
    } else if (choice == 2) {
        int target;
        printf("input convo id: ");
        if (fgets(input_tmp, sizeof(input_tmp), stdin)) {
            if (sscanf(input_tmp, "%d", &target) != 1) {
                printf("wrong Input\n");
                sem_post(&g_shm->shm_mutex);
                exit(1);
            }
        } else {
            sem_post(&g_shm->shm_mutex);
            exit(1);
        }

        if (target < 0 || target >= convos_limit || !g_shm->convos[target].active_check) {
            printf("wrong id\n");
            sem_post(&g_shm->shm_mutex);
            exit(1);
        }
        current_convo_idx = target;
        sem_wait(&g_shm->convos[target].mutex);
        g_shm->convos[target].member_count++;
        sem_post(&g_shm->convos[target].mutex);
        printf("joined Convo id: %d\n", current_convo_idx);
    } else {
        printf("bad selection\n");
        sem_post(&g_shm->shm_mutex);
        exit(1);
    }
    sem_post(&g_shm->shm_mutex);

    // threads
    signal(SIGINT, signal_handler);

    if (pthread_create(&thread_send, NULL, (void *)process_user_input, NULL) != 0) {
        perror("pthread_create failed");
    }
    if (pthread_create(&thread_recv, NULL, (void *)listen_for_messages, NULL) != 0) {
        perror("pthread_create  failed");
    }

    printf("chat is active, type  'TERMINATE' to stop\n");

    // wait for threads
    pthread_join(thread_send, NULL);
    pthread_join(thread_recv, NULL);

    printf("Bye\n");

    if (current_convo_idx != -1) {
        sem_wait(&g_shm->shm_mutex);

        Convo *d = &g_shm->convos[current_convo_idx];

        if (d->active_check) {
            sem_wait(&d->mutex);
            d->member_count--;
            if (d->member_count <= 0) {
                // close convo
                d->active_check = false;
                sem_destroy(&d->mutex);
                printf("Convo %d Closed\n", current_convo_idx);
            } else {
                sem_post(&d->mutex);
            }
        }
        sem_post(&g_shm->shm_mutex);
    }

    shutdown_resources();

    return 0;
}
