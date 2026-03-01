#ifndef P2P_PACKET_QUEUE_H
#define P2P_PACKET_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define P2P_PKT_MAX_SIZE   1500
#define P2P_PKT_QUEUE_CAP  2048

typedef struct {
    uint8_t  data[P2P_PKT_MAX_SIZE];
    size_t   size;
    uint64_t recv_time_us;
    int      agent_idx;
} p2p_packet_t;

typedef struct {
    p2p_packet_t *buf;
    int           capacity;
    int           head;
    int           tail;
    int           count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
} p2p_packet_queue_t;

int  p2p_packet_queue_init(p2p_packet_queue_t *q, int capacity);
void p2p_packet_queue_destroy(p2p_packet_queue_t *q);

/* Returns 0 on success, -1 if full (drops packet). */
int  p2p_packet_queue_push(p2p_packet_queue_t *q, const uint8_t *data,
                           size_t size, uint64_t recv_time_us, int agent_idx);

/* Returns 0 on success, -1 if empty. Non-blocking. */
int  p2p_packet_queue_pop(p2p_packet_queue_t *q, p2p_packet_t *out);

int  p2p_packet_queue_count(p2p_packet_queue_t *q);

#endif
