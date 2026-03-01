#include "p2p_packet_queue.h"
#include <stdlib.h>
#include <string.h>

int p2p_packet_queue_init(p2p_packet_queue_t *q, int capacity)
{
    if (!q || capacity <= 0) return -1;
    q->buf = calloc(capacity, sizeof(p2p_packet_t));
    if (!q->buf) return -1;
    q->capacity = capacity;
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

void p2p_packet_queue_destroy(p2p_packet_queue_t *q)
{
    if (!q) return;
    free(q->buf);
    q->buf = NULL;
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
}

int p2p_packet_queue_push(p2p_packet_queue_t *q, const uint8_t *data,
                          size_t size, uint64_t recv_time_us, int agent_idx)
{
    if (!q || !data || size == 0 || size > P2P_PKT_MAX_SIZE) return -1;

    pthread_mutex_lock(&q->mutex);
    if (q->count >= q->capacity) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    p2p_packet_t *pkt = &q->buf[q->tail];
    memcpy(pkt->data, data, size);
    pkt->size = size;
    pkt->recv_time_us = recv_time_us;
    pkt->agent_idx = agent_idx;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int p2p_packet_queue_pop(p2p_packet_queue_t *q, p2p_packet_t *out)
{
    if (!q || !out) return -1;

    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int p2p_packet_queue_count(p2p_packet_queue_t *q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->mutex);
    int c = q->count;
    pthread_mutex_unlock(&q->mutex);
    return c;
}
