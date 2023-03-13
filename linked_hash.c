#include <rte_hash.h>
#include <rte_ring.h>
#include <rte_malloc.h>
#include "linked_hash.h"

struct linked_hash* linked_hash_create(const struct rte_hash_parameters *hash_params)
{
    if (hash_params->extra_flag & RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY ||
        hash_params->extra_flag & RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD ||
        hash_params->extra_flag & RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF)
    {
        RTE_LOG(ERR, HASH,
                "%s:Concurrency is not currently supported for linked_hash\n", __func__);
        return NULL;
    }

    struct rte_hash *hashtbl = rte_hash_create(hash_params);
    if (hashtbl == NULL)
        return NULL;

    struct node *nodes = rte_zmalloc("linked hash nodes", (hash_params->entries + 1) * sizeof(struct node), 0);
    if (nodes == NULL){
        rte_hash_free(hashtbl);
        return NULL;
    }

    struct rte_ring *free_nodes = rte_ring_create("free nodes", hash_params->entries,
                                     rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);

    if (free_nodes == NULL){
        rte_hash_free(hashtbl);
        rte_free(nodes);
        return NULL;
    }

    for (int32_t i = 1; i <= ((int32_t) hash_params->entries); i++)
        rte_ring_enqueue(free_nodes, (void *) ((intptr_t) i));


    struct linked_hash *h = rte_zmalloc("linked hash", sizeof(struct linked_hash), 0);
    if (h == NULL){
        rte_hash_free(hashtbl);
        rte_ring_free(free_nodes);
        rte_free(nodes);
        return NULL;
    }

    h->hashtbl = hashtbl;
    h->free_nodes = free_nodes;
    h->nodes = nodes;
    h->size = hash_params->entries;
    return h;
}


int32_t linked_hash_front(const struct linked_hash* h, void **key, void **data){
    if (unlikely(h == NULL || key == NULL || data == NULL))
        return -1;

    if (unlikely(h->front == 0))
        return -1;      

    int32_t tbl_pos = h->nodes[h->front].tbl_pos;

    int ret = rte_hash_get_key_with_position(h->hashtbl, tbl_pos, key);
    if (ret != 0)
        return -1;

    *data = h->nodes[h->front].data;

    return h->front;
}

int32_t linked_hash_move_pos_to_front(struct linked_hash* h, int32_t pos){
    if (unlikely(pos <= 0 || pos > h->size))
        return -1;

    struct node *n = &h->nodes[pos];
    if (unlikely(n->tbl_pos == 0))
        return -1;

    if (h->front == pos) // already at front
        return 0;
    
    //stich together prev and next 
    h->nodes[n->next].prev = n->prev;
    h->nodes[n->prev].next = n->next;

    //update back if necessary
    if (h->back == pos)
        h->back = n->prev;

    //insert at front
    h->nodes[h->front].prev = pos;
    n->next = h->front;
    n->prev = 0;
    h->front = pos;

    return 0;
}

int32_t linked_hash_move_pos_to_back(struct linked_hash* h, int32_t pos){
    if (unlikely(pos <= 0 || pos > h->size))
        return -1;

    struct node *n = &h->nodes[pos];
    if (unlikely(n->tbl_pos == 0))
        return -1;

    if (h->back == pos) // already at back
        return 0;
    
    //stich together prev and next 
    h->nodes[n->next].prev = n->prev;
    h->nodes[n->prev].next = n->next;

    //update front if necessary
    if (h->front == pos)
        h->front = n->next;

    //insert at back
    h->nodes[h->back].next = pos;
    n->prev = h->back;
    n->next = 0;
    h->back = pos;

    return 0;
}

int32_t linked_hash_add_key_data(struct linked_hash* h, const void *key, void *data){
    if (unlikely(h == NULL || key == NULL))
        return -1;

    hash_sig_t hash = rte_hash_hash(h->hashtbl, key);
    intptr_t pos;
    int32_t ret;

    // make sure data isnt already in table
    ret = rte_hash_lookup_with_hash_data(h->hashtbl, key, hash, (void *)&pos);
    if (ret < 0){
        ret = rte_ring_dequeue(h->free_nodes, (void *)&pos);
        if (unlikely(ret < 0)) //linked hash is full
            return -1;
        
        ret = rte_hash_add_key_with_hash_data(h->hashtbl, key, hash, (void *)pos);

        if (unlikely(ret < 0)){
            RTE_LOG_DP(INFO, HASH,
                "%s:Unexpected hash table key data insertion error\n", __func__);
            rte_ring_enqueue(h->free_nodes, (void*)pos);
            return -1;
        }

        // annoying we need to look up this position, since the add key data functions
        // do not return a position number for the key
        int32_t tbl_pos = rte_hash_lookup_with_hash(h->hashtbl, key, hash);

        h->nodes[((int32_t) pos)].tbl_pos = tbl_pos;
        h->nodes[((int32_t) pos)].next = 0; 
        h->nodes[((int32_t) pos)].prev = h->back; 
        h->nodes[h->back].next = ((int32_t) pos);
        h->back = ((int32_t) pos);
        if (h->front == 0)
            h->front = ((int32_t) pos);
    }

    h->nodes[((int32_t) pos)].data = data;
    return ((int32_t) pos);
}

int32_t linked_hash_lookup_data(const struct linked_hash* h, const void *key, void **data){
    if(unlikely(h == NULL || key == NULL || data == NULL))
        return -1;
    
    intptr_t pos;

    int32_t ret = rte_hash_lookup_data(h->hashtbl, key, (void *)&pos);

    if (ret < 0)
        return -1;

    *data = h->nodes[(int32_t) pos].data;
    return ((int32_t) pos);
}

int32_t linked_hash_del_key(struct linked_hash* h, const void *key){
    if (unlikely(h == NULL || key == NULL))
        return -1;
        
    hash_sig_t hash = rte_hash_hash(h->hashtbl, key);
    intptr_t pos;

    int32_t ret = rte_hash_lookup_with_hash_data(h->hashtbl, key, hash, (void *)&pos);
    if(ret < 0)
        return -1;

    rte_hash_del_key_with_hash(h->hashtbl, key, hash);

    rte_ring_enqueue(h->free_nodes, (void*)pos);

    struct node *n = &h->nodes[(int32_t) pos];

    //stich together prev and next 
    h->nodes[n->next].prev = n->prev;
    h->nodes[n->prev].next = n->next;

    //update front if necessary
    if (h->front == ((int32_t) pos))
        h->front = n->next;

    if (h->back == ((int32_t) pos))
        h->back = n->prev;

    n->tbl_pos = 0;

    return 0;
}

int32_t linked_hash_del_pos(struct linked_hash* h, int32_t pos){
    if (unlikely(pos <= 0 || pos > h->size))
        return -1;

    struct node *n = &h->nodes[pos];
    if (n->tbl_pos == 0)
        return -1;

    void *key;
    int ret = rte_hash_get_key_with_position(h->hashtbl, n->tbl_pos, &key);
    if (unlikely(ret != 0))
        return -1;

    return linked_hash_del_key(h, key);
}

int32_t linked_hash_iterate(const struct linked_hash* h, void **key, void **data, int32_t *next){
    if (unlikely(h == NULL || key == NULL || data == NULL || next == NULL))
        return -1;

    int32_t pos = *next; 
    if (unlikely(pos < 0 || pos > h->size))
        return -1;

    if (pos == 0){
        if (h->front == 0)
            return -1;

        pos = h->front;
    }

    struct node *n = &h->nodes[pos];
    if (unlikely(n->tbl_pos == 0))
        return -1;
    
    void *key_lookup;
    int ret = rte_hash_get_key_with_position(h->hashtbl, n->tbl_pos, &key_lookup);
    if (unlikely(ret != 0))
        return -1;
    
    *key = key_lookup;
    *data = n->data;
    if (n->next == 0)
        *next = -1;
    else
        *next = n->next;

    return pos;
}

int32_t linked_hash_count(const struct linked_hash* h){
    return rte_hash_count(h->hashtbl);
}

void linked_hash_free(struct linked_hash* h){
    rte_hash_free(h->hashtbl);
    rte_ring_free(h->free_nodes);
    rte_free(h->nodes);
    rte_free(h);
}