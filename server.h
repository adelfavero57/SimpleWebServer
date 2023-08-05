#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "comp_handler.h"
#include "myqueue.h"

#define BUFFSIZE 4096
#define SERVER_BACKLOG 100
#define THREAD_POOL_SIZE 5

struct arg {
    pthread_mutex_t mutex;
    int client_socket;
    char *file_path;
    struct code_wrapper *dict;
};

struct arg_thread_func {
    struct queue *q;
    pthread_mutex_t mutex;
    pthread_cond_t cond_var;
};

void read_config(char **argv, char **ip_address, uint16_t **port, char **file_path);

char *read_byte_file(char *file_path, int *file_len, unsigned long offset);

void *thread_func(void *q);

void *handle_connection(void *args);

void error_handle(uint8_t *write_payload, void *args, uint8_t *payload_buffer, uint8_t *length, int client_socket);

void combine_path(char **full_path, void *args, char *fname);

unsigned long calculate_length(uint8_t *length_buffer);

void compress_payload(uint8_t *payload_buffer, uint8_t **write_payload, uint64_t *write_payload_length, struct code_wrapper *dict, uint64_t length);

void decompress_payload(uint8_t **payload_buffer, uint64_t *length, struct code_wrapper *dict);

#endif