#include "server.h"

int main(int argc, char **argv) {
    int server_socket, client_socket, addr_size;
    struct sockaddr_in servaddr, client_addr;

    char *ip_address;
    uint16_t *port;
    char *file_path;

    //reading config file from command line
    read_config(argv, &ip_address, &port, &file_path);

    //creating threads for thread pool; this allows us to only create a set number of threads, avoiding overhead if we were to create 1 thread per connection
    pthread_t thread_pool[THREAD_POOL_SIZE];
    struct queue *q = malloc(sizeof(struct queue));
    q->head = NULL;
    q->tail = NULL;

    struct arg_thread_func *atf = malloc(sizeof(struct arg_thread_func));
    pthread_mutex_init(&atf->mutex, NULL);
    atf->q = q;
    pthread_cond_init(&atf->cond_var, NULL);

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {

        pthread_create(&thread_pool[i], NULL, thread_func, (void *)atf);
    }

    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Failed to create a socket\n");
    }

    //creating address that we are listening on
    //accepting connections
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_aton(ip_address, &servaddr.sin_addr);  //responding to any addresses
    servaddr.sin_port = *port;                  //listening on defined server port

    //binding listening socket to address
    if ((bind(server_socket, (struct sockaddr *)&servaddr, sizeof(servaddr))) < 0) {
        printf("bind error\n");
        free(file_path);
        return 0;
    }

    //listening on the socket
    if ((listen(server_socket, SERVER_BACKLOG)) < 0) {
        printf("listen error\n");
        return 0;
    }

    struct code_wrapper *dictionary = read_comp_dict();

    for (;;) {
        //accept blocks until an incoming connection arrives
        //it returns a file descriptor to the connection
        addr_size = sizeof(struct sockaddr_in);

        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, (socklen_t *)&addr_size);

        //setting up arguments for thread handler
        struct arg *args = malloc(sizeof(struct arg));
        pthread_mutex_init(&args->mutex, NULL);
        args->file_path = malloc(strlen(file_path) + 1);
        args->dict = dictionary;
        strcpy(args->file_path, file_path);
        args->client_socket = client_socket;

        //enqueue the connection and the connection handler arguments into the thread queue
        enqueue(q, args);
        //Send a signal to the thread handler that there is data in the queue
        pthread_cond_signal(&atf->cond_var);
    }
}

void *thread_func(void *atf) {
    for (;;) {

        pthread_mutex_lock(&((struct arg_thread_func *)atf)->mutex);
        //attempt to dequeue
        struct arg *args = dequeue(((struct arg_thread_func *)atf)->q);
        //if there is no data, signal condition wait (will wait until cond_signal is triggered) threads will only wait if they can't get work from the queue, this is to avoid desynchronization between signal and wait
        if (args == NULL) {
            pthread_cond_wait(&((struct arg_thread_func *)atf)->cond_var, &((struct arg_thread_func *)atf)->mutex);
            args = dequeue(((struct arg_thread_func *)atf)->q);
        }
        pthread_mutex_unlock(&((struct arg_thread_func *)atf)->mutex);
        if (args != NULL) {
            void *close_condition = handle_connection(args);
            if((size_t *)close_condition) {
              break;
            }
        }
    }
    free(((struct arg_thread_func *)atf)->q);
    free((struct arg_thread_func *)atf);
    exit(0);
}

void *handle_connection(void *args) {
    int client_socket = ((struct arg *)args)->client_socket;
    pthread_mutex_t mutex = ((struct arg *)args)->mutex;
    struct code_wrapper *dict = ((struct arg *)args)->dict;
    uint8_t first_byte; //type digit + compression and require compression flags
    uint8_t *length_buffer = NULL; //stores length of the payload
    uint8_t *write_payload = NULL; //stores payload to be sent to client
    uint8_t *payload_buffer = NULL; //stores payload recieved by client
    unsigned long length; //length buffer is converted into length for usibility
    unsigned long write_payload_length;
    size_t ret_val = 0; //if ret val= 1, exit program
    //for loop will run until shutdown command received, error is triggered, or no bytes are received from client
    pthread_mutex_lock(&mutex);
    for (;;) {

        int read_flag = read(client_socket, &first_byte, 1);
        char *full_path = NULL;

        //checks if any input is received from client
        if (read_flag == 0) {
            //shutdown connection in this case
            close(client_socket);
            free(((struct arg *)args)->file_path);
            free(args);
            if (length_buffer != NULL) {
                free(length_buffer);
                length_buffer = NULL;
            }
            if (payload_buffer != NULL) {
                free(payload_buffer);
                payload_buffer = NULL;
            }
            break;
        }
        //split buffer into type bits(first_4), compression bit and require compression bit.
        uint8_t first_4 = first_byte >> 4;
        uint8_t comp_bit = (first_byte >> 3) & 1;
        uint8_t req_comp_bit = (first_byte >> 2) & 1;

        //error handling where the type is not accepted
        if (first_4 != 0 && first_4 != 2 && first_4 != 4 && first_4 != 6 && first_4 != 8) {
            error_handle(write_payload, args, payload_buffer, length_buffer, client_socket);
            break;
        }
        //shutdown command received, terminates program and frees all data that is not already free'd
        if (first_4 == 8) {
            shutdown(client_socket, SHUT_RDWR);
            close(client_socket);
            if (args != NULL) {
                free(((struct arg *)args)->dict);
                ((struct arg *)args)->dict = NULL;
                free(((struct arg *)args)->file_path);
                ((struct arg *)args)->file_path = NULL;
                free(args);
                args = NULL;
            }
            if (length_buffer != NULL) {
                free(length_buffer);
            }
            if (payload_buffer != NULL) {
                free(payload_buffer);
            }
            pthread_mutex_unlock(&mutex);
            pthread_mutex_destroy(&mutex);
            ret_val = 1;
            return ((void*)ret_val);
        }

        length_buffer = malloc(sizeof(uint8_t) * 8);
        read(client_socket, length_buffer, 8);
        length = calculate_length(length_buffer);

        payload_buffer = malloc(sizeof(uint8_t) * length);
        read(client_socket, payload_buffer, length);
        //echo command received
        if (first_4 == 0) {
            if (comp_bit == 0 && req_comp_bit == 1) {
                compress_payload(payload_buffer, &write_payload, &write_payload_length, dict, length);
                write_payload[0] = 0x18;  // 0b00011000
            }

            else {
                write_payload = malloc(sizeof(uint8_t) * (length + 9));
                memset(write_payload, 0, length + 9);
                //indicating that the payload is compressed (only if the received payload is compressed and req_comp_bit is not set)
                write_payload[0] = 0x10 | (comp_bit << 3);
                for (int i = 0; i < 8; i++) {

                    write_payload[i + 1] = length_buffer[i];

                }
                for (int i = 0; i < length; i++) {

                    write_payload[9 + i] = payload_buffer[i];

                }
                write_payload_length = length + 9;
            }

            write(client_socket, write_payload, write_payload_length);
            //freeing all allocated memory so that it can be used by other iterations.
            free(write_payload);
            write_payload = NULL;
            free(payload_buffer);
            payload_buffer = NULL;

            free(length_buffer);
            length_buffer = NULL;
        }

        else if (first_4 == 2) {
            struct dirent **sd;
            // sd stores the files from the working directory and num_files stores the number of files
            int num_files = scandir(((struct arg *)args)->file_path, &sd, NULL, NULL);
            if (num_files == -1) {
                error_handle(write_payload, args, payload_buffer, length_buffer, client_socket);

                for (int i = 0; i < num_files; i++) {

                    free(sd[i]);

                }
                free(sd);
                break;
            }

            if(comp_bit == 1) {
              decompress_payload(&payload_buffer, &length, dict);
            }

            unsigned long bytes_to_send = 0;
            unsigned long total_bytes = 0;
            uint8_t *temp_write_payload;

            //checking whether each path is regular and incrementing the total_bytes to send by each character in the regular file name, this is so that we can malloc the bufffer before we store the file names
            for (int i = 0; i < num_files; i++) {
                //combining file name and working directory to access the full path
                combine_path(&full_path, args, sd[i]->d_name);
                struct stat path_stat;
                stat(full_path, &path_stat);
                if (S_ISREG(path_stat.st_mode) != 0) {
                    for (int j = 0; j < strlen(sd[i]->d_name) + 1; j++) {

                        total_bytes++;

                    }
                }
                free(full_path);
            }
            // 9 represents the number of bytes containing the payload length and type/flag byte
            total_bytes += 9;
            //temp_write_payload stores the file names until we check whether or not we need to compress the payload. Write payload will still hold the first byte and the length bytes as these will be changed in the compression function if necessary
            write_payload = malloc(9);
            temp_write_payload = malloc(total_bytes - 9);
            write_payload[0] = 0x30;

            for (int i = 0; i < 8; i++) {

                write_payload[i + 1] = ((total_bytes - 9) >> 8 * (7 - i)) & 0xFF;

            }

            bytes_to_send = 0; //keeps track of the index of the temp_write_payload when adding the file names

            //adding the file names to the temp_write_buffer if they are regular
            for (int i = 0; i < num_files; i++) {

                combine_path(&full_path, args, sd[i]->d_name);
                struct stat path_stat;
                stat(full_path, &path_stat);

                if (S_ISREG(path_stat.st_mode) != 0) {
                    for (int j = 0; j < strlen(sd[i]->d_name) + 1; j++) {

                        temp_write_payload[bytes_to_send + j] = sd[i]->d_name[j];

                    }
                    bytes_to_send += strlen(sd[i]->d_name) + 1;
                }
                free(full_path);
            }

            if (req_comp_bit == 1) {
                compress_payload(temp_write_payload, &write_payload, &write_payload_length, dict, bytes_to_send);
                write_payload[0] = 0x38;  //0b00111000

                total_bytes = write_payload_length;
            }

            else {
                write_payload = realloc(write_payload, total_bytes);
                for (int i = 0; i < total_bytes - 9; i++) {

                    write_payload[9 + i] = temp_write_payload[i];

                }
            }

            write(client_socket, write_payload, total_bytes);
            for (int i = 0; i < num_files; i++) {

                free(sd[i]);

            }
            free(sd);
            free(temp_write_payload);
            temp_write_payload = NULL;
            free(write_payload);
            temp_write_payload = NULL;
            free(payload_buffer);
            payload_buffer = NULL;
            free(length_buffer);
            length_buffer = NULL;

        }

        else if (first_4 == 4) {
            long long file_size;
            uint8_t *write_payload = malloc(1 + 8 + 8);
            char *fname = malloc(length);

            if (comp_bit == 1) {
                decompress_payload(&payload_buffer, &length, dict);
            }

            //manually assembling the target filename
            for (int i = 0; i < length; i++) {

                fname[i] = (char)payload_buffer[i];

            }
            //creating the full path to the file
            combine_path(&full_path, args, fname);
            free(fname);
            FILE *f = fopen(full_path, "r");
            //checking if the file exists, else through an error
            if (f == NULL) {
                error_handle(write_payload, args, payload_buffer, length_buffer, client_socket);
                break;
            }

            int fd = fileno(f);
            struct stat sb;
            uint8_t *temp_write_payload = malloc(8);

            if (fstat(fd, &sb) != -1) {
                file_size = sb.st_size;
            }
            write_payload[0] = 0x50;
            for (int i = 0; i < 7; i++) {

                write_payload[i + 1] = 0x00;

            }

            write_payload[8] = 0x08;
            for (int i = 0; i < 8; i++) {

                temp_write_payload[i] = (uint8_t)(((file_size) >> 8 * (7 - i)) & 0xFF);

            }

            if (req_comp_bit == 1) {
                compress_payload(temp_write_payload, &write_payload, &write_payload_length, dict, 8);
                write_payload[0] = 0x58;
            }

            else {
                for (int i = 0; i < 8; i++) {

                    write_payload[i + 9] = temp_write_payload[i];
                    write_payload_length = 17;

                }
            }

            write(client_socket, write_payload, write_payload_length);
            fclose(f);
            free(temp_write_payload);
            temp_write_payload = NULL;
            free(full_path);
            full_path = NULL;
            free(write_payload);
            full_path = NULL;
            free(payload_buffer);
            payload_buffer = NULL;
            free(length_buffer);
            length_buffer = NULL;
        }

        else if (first_4 == 6) {
            if (comp_bit == 1) {
                decompress_payload(&payload_buffer, &length, dict);
            }

            uint8_t offset_buffer[8] = {payload_buffer[4], payload_buffer[5], payload_buffer[6],
              payload_buffer[7], payload_buffer[8], payload_buffer[9],
              payload_buffer[10], payload_buffer[11]};
            unsigned long start_offset = calculate_length(offset_buffer);

            uint8_t file_contents_length_buffer[8];
            for (int i = 0; i < 8; i++) {
                //12 byte offset indicates the start of the file contents length;
                file_contents_length_buffer[i] = payload_buffer[12 + i];

            }
            unsigned long file_contents_length = calculate_length(file_contents_length_buffer);

            char *file_name;
            int file_name_length = 0;
            //19 is the starting index of the file name in the payload_buffer
            //determining the length of the file name
            int index = 19;
            while (payload_buffer[index] != 0x00) {

                index++;
                file_name_length++;

            }

            file_name = malloc(file_name_length);
            //creating a separate file name variable
            for (int i = 0; i < file_name_length; i++) {

                file_name[i] = (char)payload_buffer[20 + i];

            }

            combine_path(&full_path, args, file_name);
            int file_len = 0;
            uint8_t *file_contents = (uint8_t *)read_byte_file(full_path, &file_len, start_offset);

            //if bad file range then throw error
            if ((start_offset + file_contents_length) > file_len) {
                error_handle(write_payload, args, payload_buffer, length_buffer, client_socket);
                break;
            }

            if (file_contents == NULL) {
                error_handle(write_payload, args, payload_buffer, length_buffer, client_socket);
                free(full_path);
                free(file_name);
                break;
            }

            write_payload_length = 9 + 20 + file_contents_length;
            //temporary buffer before checking whether or not we compress the payload
            uint8_t *temp_write_payload = malloc(write_payload_length);
            write_payload = malloc(9);
            int write_payload_offset = 0;
            uint8_t temp_length_buffer[8];
            unsigned long return_length = file_contents_length + 20;

            for (int i = 0; i < 8; i++) {

                temp_length_buffer[i] = (uint8_t)(((return_length) >> 8 * (7 - i)) & 0xFF);

            }
            // length stored in write payload, it will be changed in the compression function if it needs to be compressed
            for (int i = 0; i < 8; i++) {

                write_payload[1 + i] = temp_length_buffer[i];

            }

            //all other data is stored in the temporary buffer
            for (int i = 0; i < 4; i++) {

                temp_write_payload[write_payload_offset + i] = payload_buffer[i];

            }
            write_payload_offset += 4;

            for (int i = 0; i < 8; i++) {

                temp_write_payload[write_payload_offset + i] = offset_buffer[i];

            }
            write_payload_offset += 8;

            for (int i = 0; i < 8; i++) {

                temp_write_payload[write_payload_offset + i] = file_contents_length_buffer[i];

            }
            write_payload_offset += 8;

            for (int i = 0; i < file_contents_length; i++) {

                temp_write_payload[write_payload_offset + i] = file_contents[i];

            }

            if (req_comp_bit == 1) {
                compress_payload(temp_write_payload, &write_payload,
                  &write_payload_length, dict, write_payload_length - 9);
                write_payload[0] = 0x78;
            }

            else {
                write_payload = realloc(write_payload, write_payload_length);
                for (int i = 0; i < write_payload_length - 9; i++) {

                    write_payload[i + 9] = temp_write_payload[i];

                }
                write_payload[0] = 0x70;
            }

            write(client_socket, write_payload, write_payload_length);
            free(file_contents);
            file_contents = NULL;
            free(temp_write_payload);
            temp_write_payload = NULL;
            free(full_path);
            full_path = NULL;
            free(file_name);
            file_name = NULL;
            free(write_payload);
            write_payload = NULL;
            free(payload_buffer);
            payload_buffer = NULL;
            free(length_buffer);
            length_buffer = NULL;
        }

        else {
            error_handle(write_payload, args, payload_buffer, length_buffer, client_socket);
            break;
        }
    }

    pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);
    return (void *)ret_val;
}
//Reads config file specified in command line argument, takes information and stores it in the passed variables
void read_config(char **argv, char **ip_address, uint16_t **port, char **file_path) {
  int file_len = 0;
  char *buffer = read_byte_file(argv[1], &file_len, 0);
  char byte_buf[] = {buffer[0], buffer[1], buffer[2], buffer[3]};
  int *x = (int *)byte_buf;

  struct in_addr inaddr;
  inaddr.s_addr = *x;
  (*ip_address) = inet_ntoa(inaddr);
  char byte_buf2[] = {buffer[4], buffer[5]};
  (*port) = (uint16_t *)byte_buf2;

  int string_length = file_len - 6;
  (*file_path) = (char *)malloc(string_length + 1);
  memcpy((*file_path), &buffer[6], string_length);
  (*file_path)[string_length] = '\0';

  free(buffer);
}

/*
USYD CODE CITATION ACKNOWLEDGEMENT
  I declare that the following lines of code have been copied from the website titled: "Read file byte by byte using fread"

  Original URL
      https://stackoverflow.com/questions/28269995/c-read-file-byte-by-byte-using-fread
*/

char *read_byte_file(char *file_path, int *file_len, unsigned long offset) {
    FILE *ptr;
    char *buffer;

    ptr = fopen(file_path, "rb");
    if (ptr == NULL) {
        return NULL;
    }

    fseek(ptr, 0, SEEK_END);
    *file_len = ftell(ptr);
    fseek(ptr, offset, SEEK_SET);

    buffer = (char *)malloc(sizeof(char) *
                            (*file_len));
    fread(buffer, *file_len, 1, ptr);
    fclose(ptr);
    return buffer;
}

//Is called when an error has been triggered. Sends back an error payload to the lient then closes the socket.
void error_handle(uint8_t *write_payload, void *args, uint8_t *payload_buffer, uint8_t *length_buffer, int client_socket) {
    write_payload = malloc(sizeof(uint8_t) * 9);
    memset(write_payload, 0, 9);
    write_payload[0] = 240;
    for(int i = 0; i < 8; i++) {

      write_payload[i+1] = 0;

    }

    write(client_socket, write_payload, 9);
    close(client_socket);

    if (args != NULL) {
        free(((struct arg *)args)->dict);
        ((struct arg *)args)->dict = NULL;
        free(((struct arg *)args)->file_path);
        ((struct arg *)args)->file_path = NULL;
        free(args);
        args = NULL;
    }
    if (write_payload != NULL) {
        free(write_payload);
        write_payload = NULL;
    }
    if (payload_buffer != NULL) {
        free(payload_buffer);
        payload_buffer = NULL;
    }
    if (length_buffer != NULL) {
        free(length_buffer);
        length_buffer = NULL;
    }
}

//combines a directory and a file name and stores it in the fullpath
void combine_path(char **full_path, void *args, char *fname) {
    *full_path = malloc(strlen(((struct arg *)args)->file_path) + strlen(fname) + 3);
    strcpy((*full_path), ((struct arg *)args)->file_path);
    (*full_path)[strlen(((struct arg *)args)->file_path)] = '/';
    for (int j = 0; j < strlen(fname) + 1; j++) {

        (*full_path)[strlen(((struct arg *)args)->file_path) + j + 1] = fname[j];

    }
}

//enqueues an argument to be used in the connection handler
void enqueue(struct queue *q, struct arg *args) {
    struct node *newnode = malloc(sizeof(struct node));
    newnode->args = args;
    newnode->next = NULL;
    if (q->tail == NULL) {
        q->head = newnode;
    }

    else {
        q->tail->next = newnode;
    }
    q->tail = newnode;
}

struct arg *dequeue(struct queue *q) {
    if (q->head == NULL) {
        return NULL;
    }

    else {
        struct arg *result = q->head->args;
        struct node *temp = q->head;
        q->head = q->head->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        free(temp);
        return result;
    }
}

//reads the compression dictionary into 256 code_wrappers
struct code_wrapper *read_comp_dict() {
    int file_len;
    //reading the compression dictionary into dict
    uint8_t *dict = (uint8_t *)read_byte_file("compression.dict", &file_len, 0);
    uint8_t *char_convert = (uint8_t *)malloc(file_len * 8);
    struct code_wrapper *wrapper = (struct code_wrapper *)malloc(sizeof(struct code_wrapper) * 256);

    //converting all bits into char bytes for easy manipultation
    for (int i = 0; i < file_len; i++) {

        for (int j = 0; j < 8; j++) {

            char_convert[(i * 8 + j)] = (dict[i] >> (7 - j)) & 1;

        }
    }

    int i = 0;
    //iterating through list of codes (that have been manipulated to bytes) and adding each one to the list of code wrappers
    //that store a code and its size. (bytes are added bit by bit)
    for (int pos = 0; pos < 256; pos++) {

        uint8_t length = 0;
        for (int j = 0; j < 8; j++) {

            length |= char_convert[i + j] << (7 - j); //building the length of the code bit by bit

        }
        i += 8;
        wrapper[pos].code_len = length;
        uint64_t v = 0;
        for (int j = 0; j < length; j++) {
            //building the code itself, stored as a uint64_t. Padding between the byte boundary is at the start of the byte and does not make a difference when using the code
            v |= char_convert[i + j] << (length - j - 1);

        }
        wrapper[pos].code = v;
        i += length;
    }
    free(dict);
    free(char_convert);
    return wrapper;
}

// converts 8 bytes in a buffer into an unsigned long
unsigned long calculate_length(uint8_t *length_buffer) {
    unsigned long length = ((unsigned long)length_buffer[0] << 56) | ((unsigned long)length_buffer[1] << 48) | ((unsigned long)length_buffer[2] << 40) | ((unsigned long)length_buffer[3] << 32) | (length_buffer[4] << 24) | (length_buffer[5] << 16) | (length_buffer[6] << 8) | length_buffer[7];

    return length;
}

//Decompresses a payload using the dictionary
void decompress_payload(uint8_t **payload_buffer, uint64_t *length, struct code_wrapper *dict) {
    uint8_t *temp_byte_array = malloc(0);  // array of bytes after decompression
    uint64_t tba_len = 0;                  //length of the temp byte array
    uint64_t val = 0;                      // current value of code
    uint8_t val_len = 0;                   //length of val
    uint64_t total_bits = 8 * (*length);   // length of payload_buffer in bits
    //remove padding 0 by deleting the last byte from the buffer;
    total_bits -= (*payload_buffer)[(*length) - 1];
    //remove the byte that indicates padding from the length of the temp byte array
    total_bits -= 8;
    //building the value bit by bit until it matches one of the codes in the compression dict
    for (int i = 0; i < total_bits; i++) {
        //shifting val to the left by 1 (adding a zero on the right)
        // adding the next bit from payload buffer, replacing that 0 with a 1 or leaving the 0
        val = (val << 1) | (((*payload_buffer)[i / 8] >> (7 - i % 8)) & 1);
        val_len++;
        //compring the val against all codes in the dictionary
        for (int j = 0; j < 256; j++) {

            if (val == dict[j].code && val_len == dict[j].code_len) {
                val = 0;
                val_len = 0;
                tba_len++;
                temp_byte_array = realloc(temp_byte_array, tba_len);
                temp_byte_array[tba_len - 1] = j;
                break;

            }
        }
    }
    free((*payload_buffer));
    (*payload_buffer) = temp_byte_array;
    (*length) = tba_len;
}

//compresses payload using the dictionary
void compress_payload(uint8_t *payload_buffer, uint8_t **write_payload, uint64_t *write_payload_length, struct code_wrapper *dict, uint64_t length) {
    uint8_t *temp_bit_array = malloc(0);
    uint64_t tba_len = 0;
    uint8_t code_len = 0;

    //iterates over the dictionary
    for (int i = 0; i < length; i++) {
        code_len = dict[payload_buffer[i]].code_len;
        temp_bit_array = realloc(temp_bit_array, tba_len + code_len);
        //iterates over the code's bits, converts each bit into a byte by adding 7 0's as padding.
        //this makes the code far more workable, though it does use alot of memory
        for (int j = 0; j < code_len; j++) {

            temp_bit_array[tba_len + j] = dict[payload_buffer[i]].code >> (code_len - j - 1) & 1;

        }
        tba_len += code_len;
    }
    //if the last code does not hang over the byte boundary, then padding is equal to 0, if it does, padding is equal to 8 minus the overhanging bits.
    uint8_t zero_padding = (tba_len % 8 != 0 ? (8 - tba_len % 8) : 0);

    //we reallocate the size of the array then initialize each byte to 0
    for (int i = 0; i < (zero_padding); i++) {

        temp_bit_array = realloc(temp_bit_array, tba_len + i + 1);
        temp_bit_array[tba_len + i] = 0;

    }
    tba_len += (zero_padding) + 8;
    (*write_payload) = realloc((*write_payload), tba_len / 8 + 9);
    temp_bit_array = realloc(temp_bit_array, tba_len);
    //adding the zero padding one 0 at a time, from front to back
    for (int i = 0; i < 8; i++) {

        temp_bit_array[tba_len - 8 + i] = (zero_padding >> (7 - i)) & 1;

    }
    //adding the length (in bytes) of the payload to the write buffer
    for (int i = 0; i < 8; i++) {

        (*write_payload)[i + 1] = ((tba_len / 8) >> (8 * (7 - i))) & 0xFF;

    }

    // adding the contents of the temp array to the write buffer. We are only addin the least significant bit of each byte, therefore the codes have been transformed back into bits.
    for (int i = 0; i < tba_len / 8; i++) {

        (*write_payload)[9 + i] = 0;
        for (int j = 0; j < 8; j++) {

            (*write_payload)[9 + i] |= temp_bit_array[8 * i + j] << (7 - j);

        }
    }
    (*write_payload_length) = 9 + tba_len / 8;
    free(temp_bit_array);
}