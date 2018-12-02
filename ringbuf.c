#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define ONE_TO_ONE_NON_BLOCK (0)
#define ONE_TO_ONE_BLOCK_TO_READ (1)
#define ONE_TO_ONE_BLOCK_TO_WRITE (2)

#define TEST_SCENARIO (ONE_TO_ONE_BLOCK_TO_WRITE) //(ONE_TO_ONE_NON_BLOCK) //(ONE_TO_ONE_BLOCK_TO_READ) //(ONE_TO_ONE_BLOCK_TO_WRITE)

#define RING_BUFF_DEPTH (8LL)
#define RING_BUFF_CAPACITY (850LL)//(4096LL)
#define RING_BUFF_SIZE (RING_BUFF_DEPTH * RING_BUFF_CAPACITY)
#define RING_BUFF_MIDDLE ((RING_BUFF_DEPTH / 2) * RING_BUFF_CAPACITY)
#define RD_WR_INITIAL_DIFF (RING_BUFF_MIDDLE)

char out_buf[RING_BUFF_DEPTH][RING_BUFF_CAPACITY];

char * buff_start_ptr = NULL;
char * wr_start_ptr = NULL;
char * rd_start_ptr = NULL;
char * rd_ptr = NULL;
char * wr_ptr = NULL;

long long int wr_abs_offset = 0;
long long int wr_offset = 0;

long long int rd_abs_offset = 0;
long long int rd_offset = 0;

int ready_to_send = 0;

long int wait_for_new_data_n_bytes = 0;
long int wait_for_free_space_n_bytes = 0;

char src_buf[128+1666] = { 0 };
char res_buf[RING_BUFF_CAPACITY] = { 0 };

pthread_mutex_t lock;

pthread_mutex_t mutex_wait_to_pull;
pthread_mutex_t mutex_wait_to_push;

pthread_mutexattr_t attr;

pthread_cond_t condvar_data_ready_to_pull;
pthread_cond_t condvar_data_ready_to_push;

int rbuff_push_n_bytes(char * data_src, unsigned int data_length);
int rbuff_pull_n_bytes(char * data_dest, unsigned int data_length);
int rbuff_wait_to_push_n_bytes(char * data_src, unsigned int data_length, unsigned int timeout_ms);
int rbuff_wait_to_pull_n_bytes(char * data_dest, unsigned int data_length, unsigned int timeout_ms);


// Push data function
int rbuff_push_n_bytes(char * data_src, unsigned int data_length)
{
    long long int shaddow_rd_abs_offset, shaddow_wr_abs_offset;
    long long int rd_to_wr_diff;
    long int valid_data_in_buff_len = 0;
    long int free_space_in_buff_len = 0;
    unsigned int bytes_to_end = 0;
    
    int rc;

    pthread_mutex_lock(&lock);
    shaddow_rd_abs_offset = rd_abs_offset;
    shaddow_wr_abs_offset = wr_abs_offset;
    pthread_mutex_unlock(&lock);

    rd_to_wr_diff = shaddow_rd_abs_offset - shaddow_wr_abs_offset;

    if (rd_to_wr_diff < (1 * RING_BUFF_CAPACITY)) {
        printf("\nWARNING: Buffer overflow watermark alarm: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld < %lld = (%d x %lld) = (%d x RING_BUFF_CAPACITY)\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), RING_BUFF_CAPACITY, 1, RING_BUFF_CAPACITY, 1);
    }
    if (rd_to_wr_diff < data_length) {
        printf("\nERROR: Buffer overflow: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld < %u = data_length\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), data_length);
        return (-1);
    }
    if (rd_to_wr_diff < 0) {
        printf("\nERROR: Buffer overflow: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld < 0\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset));
        return (-1);
    }

    valid_data_in_buff_len =  RD_WR_INITIAL_DIFF - rd_to_wr_diff;
    free_space_in_buff_len = (RING_BUFF_SIZE) - valid_data_in_buff_len;

    printf("\n=================    end %p\n", buff_start_ptr + RING_BUFF_SIZE);
    printf("\n================= wr_ptr %p | data_src %p | data_length %u\n", wr_ptr, data_src, data_length);
    bytes_to_end = ((wr_start_ptr + RING_BUFF_SIZE) - wr_ptr);
    if (bytes_to_end >= data_length) {
        memcpy((void*)wr_ptr, (void*)data_src, data_length);
    } else {
        // Copy to whatever space is available
        printf("\n----------------- wr_ptr %p | data_src %p | bytes_to_end %u\n", wr_ptr, data_src, bytes_to_end);
        memcpy((void*)wr_ptr, (void*)data_src, bytes_to_end);
        // Copy the remaining part in the beginning of the buffer
        printf("\n----------------- wr_start_ptr %p | (data_src + bytes_to_end) %p | (data_length - bytes_to_end) %u\n", wr_start_ptr, (data_src + bytes_to_end), (data_length - bytes_to_end));
        memcpy((void*)wr_start_ptr, (void*)(data_src + bytes_to_end), (data_length - bytes_to_end));
    }

    printf("\nPushed another %u bytes at %lld position -> valid_data_in_buff_len: %ld, free_space_in_buff_len: %ld\n", data_length, wr_offset, valid_data_in_buff_len + data_length, free_space_in_buff_len - data_length);

    pthread_mutex_lock(&lock);
    wr_abs_offset += data_length;
    pthread_mutex_unlock(&lock);

    wr_offset = wr_abs_offset % RING_BUFF_SIZE;
    // Updating wr_ptr
    wr_ptr = wr_start_ptr + wr_offset;

    // Push was successful update wait_for_new_data_n_bytes then and signal consumer thread
    pthread_mutex_lock(&mutex_wait_to_pull);
    if ((valid_data_in_buff_len + data_length) >= wait_for_new_data_n_bytes) {
        // if newly available data is enough for the consumer
        wait_for_new_data_n_bytes = 0;
    } else {
        // if is not enough - just update the wait_for_new_data_n_bytes
        wait_for_new_data_n_bytes -= data_length;
    }
    if (wait_for_new_data_n_bytes <= 0) {
        pthread_cond_signal(&condvar_data_ready_to_pull);
    }
    pthread_mutex_unlock(&mutex_wait_to_pull);

    return 0;
}

// Pull data function
int rbuff_pull_n_bytes(char * data_dest, unsigned int data_length)
{    
    long long int shaddow_rd_abs_offset, shaddow_wr_abs_offset;
    long long int rd_to_wr_diff;
    long int valid_data_in_buff_len = 0;
    long int free_space_in_buff_len = 0;
    unsigned int bytes_to_end = 0;
    
    int rc;

    pthread_mutex_lock(&lock);
    shaddow_rd_abs_offset = rd_abs_offset;
    shaddow_wr_abs_offset = wr_abs_offset;
    pthread_mutex_unlock(&lock);

    rd_to_wr_diff = shaddow_rd_abs_offset - shaddow_wr_abs_offset;

    if (rd_to_wr_diff >= ((RING_BUFF_DEPTH - 1) * RING_BUFF_CAPACITY)) {
        printf("\nWARNING: Buffer underflow watermark alarm: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld >= %lld = ((%lld - 1) * %lld) = ((RING_BUFF_DEPTH - 1) * RING_BUFF_CAPACITY)\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), ((RING_BUFF_DEPTH - 1) * RING_BUFF_CAPACITY), RING_BUFF_DEPTH, RING_BUFF_CAPACITY);
    }
    if (rd_to_wr_diff >= (RING_BUFF_DEPTH * RING_BUFF_CAPACITY)) {
        printf("\nERROR: Buffer underflow: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld >= %lld = (%lld * %lld) = (RING_BUFF_DEPTH * RING_BUFF_CAPACITY)\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), (RING_BUFF_DEPTH * RING_BUFF_CAPACITY), RING_BUFF_DEPTH, RING_BUFF_CAPACITY);
        return (-1);
    }

    valid_data_in_buff_len = RD_WR_INITIAL_DIFF - rd_to_wr_diff;
    free_space_in_buff_len = (RING_BUFF_SIZE) - valid_data_in_buff_len;

    printf("\n=================    end %p\n", buff_start_ptr + RING_BUFF_SIZE);
    printf("\n================= rd_ptr %p | data_dest %p | data_length %u\n", rd_ptr, data_dest, data_length);
    bytes_to_end = ((rd_start_ptr + RING_BUFF_SIZE) - rd_ptr);
    if (bytes_to_end >= data_length) {
        memcpy((void*)data_dest, (void*)rd_ptr, data_length);
    } else {
        // Read whatever is available
        printf("\n----------------- rd_ptr %p | data_dest %p | bytes_to_end %u\n", rd_ptr, data_dest, bytes_to_end);
        memcpy((void*)data_dest, (void*)rd_ptr, bytes_to_end);
        // Read the remaining part from the beginning of the buffer
        printf("\n----------------- rd_start_ptr %p | (data_dest + bytes_to_end) %p | (data_length - bytes_to_end) %u\n", rd_start_ptr, (data_dest + bytes_to_end), (data_length - bytes_to_end));
        memcpy((void*)(data_dest + bytes_to_end), (void*)rd_start_ptr, (data_length - bytes_to_end));
    }

    printf("\nPulled another %u bytes from %lld position -> valid_data_in_buff_len: %ld, free_space_in_buff_len: %ld, \n", data_length, rd_offset, valid_data_in_buff_len - data_length, free_space_in_buff_len + data_length);

    pthread_mutex_lock(&lock);
    rd_abs_offset += data_length;
    pthread_mutex_unlock(&lock);

    rd_offset = rd_abs_offset % RING_BUFF_SIZE;
    // Updating rd_ptr
    rd_ptr = rd_start_ptr + rd_offset;

    // Pull was successful update wait_for_free_space_n_bytes then and signal producer thread
    pthread_mutex_lock(&mutex_wait_to_push);
    if ((free_space_in_buff_len + data_length) >= wait_for_free_space_n_bytes) {
        // if newly free space is enough for the producer
        wait_for_free_space_n_bytes = 0;
    } else {
        // if is not enough - just update the wait_for_free_space_n_bytes
        wait_for_free_space_n_bytes -= data_length;
    }
    if (wait_for_free_space_n_bytes <= 0) {
        pthread_cond_signal(&condvar_data_ready_to_push);
    }
    pthread_mutex_unlock(&mutex_wait_to_push);

    return 0;
}

int rbuff_wait_to_push_n_bytes(char * data_src, unsigned int data_length, unsigned int timeout_ms)
{
    long long int shaddow_rd_abs_offset, shaddow_wr_abs_offset;
    long long int rd_to_wr_diff;
    long int valid_data_in_buff_len = 0;
    long int free_space_in_buff_len = 0;
    unsigned int bytes_to_end = 0;

    int err;
    int rc;

    struct timeval tv;
    struct timespec ts;

    pthread_mutex_lock(&mutex_wait_to_push);
    wait_for_free_space_n_bytes = data_length;
    pthread_mutex_unlock(&mutex_wait_to_push);

    pthread_mutex_lock(&mutex_wait_to_push);
    gettimeofday(&tv, NULL);
    ts.tv_sec = time(NULL) + timeout_ms / 1000;
    ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (timeout_ms % 1000);
    ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
    ts.tv_nsec %= (1000 * 1000 * 1000);
    while (wait_for_free_space_n_bytes > 0) {
        err = pthread_cond_timedwait(&condvar_data_ready_to_push, &mutex_wait_to_push, &ts);
        if (err == 0) {
            printf("\n>>>>>>>>>>>>>>>>>> REEEEEEEEEEEEEEEEEEDYY\n");
            rc = 0;
            break;
        } else if (err == ETIMEDOUT) {
            printf("\n>>>>>>>>>>>>>>>>>> TIMEOOOOOOOOOOOOOOOOUT\n");
            rc = -ETIMEDOUT;
            break;
        } else {
            printf("\n>>>>>>>>>>>>>>>>>> ERRRRRROOOOOOOOOOOOOOR\n");
            rc = -err;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_wait_to_push);

    if (rc < 0) {
        return rc;
    }

    pthread_mutex_lock(&lock);
    shaddow_rd_abs_offset = rd_abs_offset;
    shaddow_wr_abs_offset = wr_abs_offset;
    pthread_mutex_unlock(&lock);

    rd_to_wr_diff = shaddow_rd_abs_offset - shaddow_wr_abs_offset;

    if (rd_to_wr_diff < (1 * RING_BUFF_CAPACITY)) {
        printf("\nWARNING: Buffer overflow watermark alarm: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld < %lld = (%d x %lld) = (%d x RING_BUFF_CAPACITY)\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), RING_BUFF_CAPACITY, 1, RING_BUFF_CAPACITY, 1);
    }
    if (rd_to_wr_diff < data_length) {
        printf("\nERROR: Buffer overflow: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld < %u = data_length\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), data_length);
        return (-1);
    }
    if (rd_to_wr_diff < 0) {
        printf("\nERROR: Buffer overflow: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld < 0\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset));
        return (-1);
    }

    valid_data_in_buff_len =  RD_WR_INITIAL_DIFF - rd_to_wr_diff;
    free_space_in_buff_len = (RING_BUFF_SIZE) - valid_data_in_buff_len;

    printf("\n=================    end %p\n", buff_start_ptr + RING_BUFF_SIZE);
    printf("\n================= wr_ptr %p | data_src %p | data_length %u\n", wr_ptr, data_src, data_length);
    bytes_to_end = ((wr_start_ptr + RING_BUFF_SIZE) - wr_ptr);
    if (bytes_to_end >= data_length) {
        memcpy((void*)wr_ptr, (void*)data_src, data_length);
    } else {
        // Copy to whatever space is available
        printf("\n----------------- wr_ptr %p | data_src %p | bytes_to_end %u\n", wr_ptr, data_src, bytes_to_end);
        memcpy((void*)wr_ptr, (void*)data_src, bytes_to_end);
        // Copy the remaining part in the beginning of the buffer
        printf("\n----------------- wr_start_ptr %p | (data_src + bytes_to_end) %p | (data_length - bytes_to_end) %u\n", wr_start_ptr, (data_src + bytes_to_end), (data_length - bytes_to_end));
        memcpy((void*)wr_start_ptr, (void*)(data_src + bytes_to_end), (data_length - bytes_to_end));
    }

    printf("\nPushed another %u bytes at %lld position -> valid_data_in_buff_len: %ld, free_space_in_buff_len: %ld\n", data_length, wr_offset, valid_data_in_buff_len + data_length, free_space_in_buff_len - data_length);

    pthread_mutex_lock(&lock);
    wr_abs_offset += data_length;
    pthread_mutex_unlock(&lock);

    wr_offset = wr_abs_offset % RING_BUFF_SIZE;
    // Updating wr_ptr
    wr_ptr = wr_start_ptr + wr_offset;

    return 0;
}

int rbuff_wait_to_pull_n_bytes(char * data_dest, unsigned int data_length, unsigned int timeout_ms)
{    
    long long int shaddow_rd_abs_offset, shaddow_wr_abs_offset;
    long long int rd_to_wr_diff;
    long int valid_data_in_buff_len = 0;
    long int free_space_in_buff_len = 0;
    unsigned int bytes_to_end = 0;
    
    int err;
    int rc;

    struct timeval tv;
    struct timespec ts;

    pthread_mutex_lock(&mutex_wait_to_pull);
    wait_for_new_data_n_bytes = data_length;
    pthread_mutex_unlock(&mutex_wait_to_pull);

    pthread_mutex_lock(&mutex_wait_to_pull);
    gettimeofday(&tv, NULL);
    ts.tv_sec = time(NULL) + timeout_ms / 1000;
    ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (timeout_ms % 1000);
    ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
    ts.tv_nsec %= (1000 * 1000 * 1000);
    while (wait_for_new_data_n_bytes > 0) {
        err = pthread_cond_timedwait(&condvar_data_ready_to_pull, &mutex_wait_to_pull, &ts);
        if (err == 0) {
            printf("\n>>>>>>>>>>>>>>>>>> REEEEEEEEEEEEEEEEEEDYY\n");
            rc = 0;
            break;
        } else if (err == ETIMEDOUT) {
            printf("\n>>>>>>>>>>>>>>>>>> TIMEOOOOOOOOOOOOOOOOUT\n");
            rc = -ETIMEDOUT;
            break;
        } else {
            printf("\n>>>>>>>>>>>>>>>>>> ERRRRRROOOOOOOOOOOOOOR\n");
            rc = -err;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_wait_to_pull);

    if (rc < 0) {
        return rc;
    }

    pthread_mutex_lock(&lock);
    shaddow_rd_abs_offset = rd_abs_offset;
    shaddow_wr_abs_offset = wr_abs_offset;
    pthread_mutex_unlock(&lock);

    rd_to_wr_diff = shaddow_rd_abs_offset - shaddow_wr_abs_offset;

    if (rd_to_wr_diff >= ((RING_BUFF_DEPTH - 1) * RING_BUFF_CAPACITY)) {
        printf("\nWARNING: Buffer underflow watermark alarm: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld >= %lld = ((%lld - 1) * %lld) = ((RING_BUFF_DEPTH - 1) * RING_BUFF_CAPACITY)\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), ((RING_BUFF_DEPTH - 1) * RING_BUFF_CAPACITY), RING_BUFF_DEPTH, RING_BUFF_CAPACITY);
    }
    if (rd_to_wr_diff >= (RING_BUFF_DEPTH * RING_BUFF_CAPACITY)) {
        printf("\nERROR: Buffer underflow: (rd_abs_offset - wr_abs_offset) = %lld - %lld = %lld >= %lld = (%lld * %lld) = (RING_BUFF_DEPTH * RING_BUFF_CAPACITY)\n", shaddow_rd_abs_offset, shaddow_wr_abs_offset, (shaddow_rd_abs_offset - shaddow_wr_abs_offset), (RING_BUFF_DEPTH * RING_BUFF_CAPACITY), RING_BUFF_DEPTH, RING_BUFF_CAPACITY);
        return (-1);
    }

    valid_data_in_buff_len = RD_WR_INITIAL_DIFF - rd_to_wr_diff;
    free_space_in_buff_len = (RING_BUFF_SIZE) - valid_data_in_buff_len;

    printf("\n=================    end %p\n", buff_start_ptr + RING_BUFF_SIZE);
    printf("\n================= rd_ptr %p | data_dest %p | data_length %u\n", rd_ptr, data_dest, data_length);
    bytes_to_end = ((rd_start_ptr + RING_BUFF_SIZE) - rd_ptr);
    if (bytes_to_end >= data_length) {
        memcpy((void*)data_dest, (void*)rd_ptr, data_length);
    } else {
        // Read whatever is available
        printf("\n----------------- rd_ptr %p | data_dest %p | bytes_to_end %u\n", rd_ptr, data_dest, bytes_to_end);
        memcpy((void*)data_dest, (void*)rd_ptr, bytes_to_end);
        // Read the remaining part from the beginning of the buffer
        printf("\n----------------- rd_start_ptr %p | (data_dest + bytes_to_end) %p | (data_length - bytes_to_end) %u\n", rd_start_ptr, (data_dest + bytes_to_end), (data_length - bytes_to_end));
        memcpy((void*)(data_dest + bytes_to_end), (void*)rd_start_ptr, (data_length - bytes_to_end));
    }

    printf("\nPulled another %u bytes from %lld position -> valid_data_in_buff_len: %ld, free_space_in_buff_len: %ld\n", data_length, rd_offset, valid_data_in_buff_len - data_length, free_space_in_buff_len + data_length);

    pthread_mutex_lock(&lock);
    rd_abs_offset += data_length;
    pthread_mutex_unlock(&lock);

    rd_offset = rd_abs_offset % RING_BUFF_SIZE;
    // Updating rd_ptr
    rd_ptr = rd_start_ptr + rd_offset;

    return 0;
}

#if (TEST_SCENARIO == ONE_TO_ONE_NON_BLOCK)

void * write_thread()
{
    unsigned char data_len_just_pushed = 0;
    unsigned int data_to_send_len = 0;

    while(1){

        for (unsigned int i = 0; i < 16/*128*/; i++) {
            data_to_send_len = 0;
            unsigned char j;
            for (j = 1; j <= (i + 1); j++) {
                src_buf[j - 1] = (char)j;
                data_len_just_pushed = j;
            }
            usleep(10000);

            (void)rbuff_push_n_bytes(src_buf, data_len_just_pushed);//96);//data_len_just_pushed);

            data_to_send_len += data_len_just_pushed;

            if (data_to_send_len >= RING_BUFF_CAPACITY) {
                ready_to_send = 1;
            }
        }
    }
}

void * read_thread()
{
    usleep(1000000);
    while(1){
        usleep(1000000);
        
        (void)rbuff_pull_n_bytes(res_buf, RING_BUFF_CAPACITY);
        
        for (unsigned int i = 0; i < RING_BUFF_CAPACITY; i++)
        {
            printf("%u ", (unsigned int)res_buf[i]);
        }

        printf("\n\n\n\n\n");
    }
}

#elif (TEST_SCENARIO == ONE_TO_ONE_BLOCK_TO_READ)

void * write_thread()
{
    unsigned char data_len_just_pushed = 0;
    unsigned int data_to_send_len = 0;
    
    usleep(1000000);
    while(1){

        for (unsigned int i = 0; i < 16/*128*/; i++) {
            data_to_send_len = 0;
            unsigned char j;
            for (j = 1; j <= (i + 1); j++) {
                src_buf[j - 1] = (char)j;
                data_len_just_pushed = j;
            }

            usleep(100000);

            (void)rbuff_push_n_bytes(src_buf, data_len_just_pushed);//96);//data_len_just_pushed);

            data_to_send_len += data_len_just_pushed;

            if (data_to_send_len >= RING_BUFF_CAPACITY) {
                ready_to_send = 1;
            }
        }
    }
}

void * read_thread()
{
    int res = 0;
    //usleep(1000000);
    while(1){
        //usleep(100000);
        res = rbuff_wait_to_pull_n_bytes(res_buf, RING_BUFF_CAPACITY, 10000);
        if (res >= 0) {
            for (unsigned int i = 0; i < RING_BUFF_CAPACITY; i++) {
                printf("%u ", (unsigned int)res_buf[i]);
            }
        } else {
            printf("\nERROR in rbuff_wait_to_pull_n_bytes()\n");
        }
        printf("\n\n\n\n\n");
    }
}

#elif (TEST_SCENARIO == ONE_TO_ONE_BLOCK_TO_WRITE)

void * write_thread()
{
    unsigned char data_len_just_pushed = 0;
    unsigned int data_to_send_len = 0;
    
    //usleep(1000000);
    while(1){

    for (unsigned int i = 0; i < 16/*128*/; i++) {
        data_to_send_len = 0;
        unsigned char j;
        for (j = 1; j <= (i + 1); j++) {
            src_buf[j - 1] = (char)j;
            data_len_just_pushed = j;
        }
        //usleep(1000);
        //usleep(10000);

        (void)rbuff_wait_to_push_n_bytes(src_buf, data_len_just_pushed, 10000);//96);//data_len_just_pushed);

        data_to_send_len += data_len_just_pushed;

        if (data_to_send_len >= RING_BUFF_CAPACITY) {
            ready_to_send = 1;
        }
    }
    }
}

void * read_thread()
{
    usleep(1000000);
    while(1){
        usleep(1000000);
        (void)rbuff_pull_n_bytes(res_buf, 5);

        for (unsigned int i = 0; i < 5; i++)
        {
            printf("%u ", (unsigned int)res_buf[i]);
        }

        printf("\n\n\n\n\n");
    }
}

#endif

int main()
{
    int status;
    pthread_t tid1, tid2;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);//PTHREAD_MUTEX_RECURSIVE);//PTHREAD_MUTEX_ERRORCHECK);

    pthread_mutex_init(&lock, &attr);

    pthread_mutex_init(&mutex_wait_to_pull, &attr);
    pthread_mutex_init(&mutex_wait_to_push, &attr);

    /* initialize a condition variable to its default value */
    pthread_cond_init(&condvar_data_ready_to_pull, NULL);
    pthread_cond_init(&condvar_data_ready_to_push, NULL);


    buff_start_ptr = &out_buf[0][0];

    wr_start_ptr = buff_start_ptr;
    wr_abs_offset = 0;
    wr_offset = wr_abs_offset % RING_BUFF_SIZE;
    wr_ptr = wr_start_ptr + wr_offset;

    printf("wr_start_ptr:%p wr_abs_offset: %lld wr_offset: %lld wr_ptr: %p\n", wr_start_ptr, wr_abs_offset, wr_offset, wr_ptr);

    // Initialize rd_start_ptr in the middle of whole ring buffer
    rd_start_ptr = buff_start_ptr;
    rd_abs_offset = RD_WR_INITIAL_DIFF;
    rd_offset = rd_abs_offset % RING_BUFF_SIZE;
    rd_ptr = rd_start_ptr + rd_offset;

    printf("rd_start_ptr:%p rd_abs_offset: %lld rd_offset: %lld rd_ptr: %p\n", rd_start_ptr, rd_abs_offset, rd_offset, rd_ptr);

    pthread_create(&tid1, NULL, write_thread, NULL);
    pthread_create(&tid2, NULL, read_thread, NULL);
    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);
    return 0;
}






