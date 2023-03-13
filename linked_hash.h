#include <rte_hash.h>
#include <rte_ring.h>

struct node {
    void *data;
    int32_t tbl_pos;
    int32_t next;
    int32_t prev;
};

struct linked_hash {
    struct rte_hash *hashtbl;
    struct node *nodes;
    struct rte_ring *free_nodes;
    uint32_t front;
    uint32_t back;
    uint32_t size;
};

// create a new linked hash structure with the given hash_params
// return: pointer to linked_hash structure on success, NULL on failure
struct linked_hash* linked_hash_create(const struct rte_hash_parameters* hash_params);

// get the front value in the linked hash, the key an corresponding data are
// placed in key and data parameters
// return: the positive position of the front element on success, -1 on failure
int32_t linked_hash_front(const struct linked_hash* h, void **key, void **data);

// move the entry corresponding with the passed postion to the front of the linked hash
// return: 0 on success, -1 on failure
int32_t linked_hash_move_pos_to_front(struct linked_hash* h, int32_t pos);

// move the entry corresponding with the passed postion to the end of the linked hash
// return: 0 on success, -1 on failure
int32_t linked_hash_move_pos_to_back(struct linked_hash* h, int32_t pos);

// add a new entry to the end of the linked hash, if the key already exists in the hash then the
// corresponding data entry will be updated with the new value
// return: the positive position of the new/existing entry on success, -1 on failure
int32_t linked_hash_add_key_data(struct linked_hash* h, const void *key, void *data);

// look up an entry with a specified key, and place the corresponding data in the data parameter
// return: the positive position of the entry on success, -1 on failure
int32_t linked_hash_lookup_data(const struct linked_hash* h, const void *key, void **data);

// delete entry fron the linked hash with the specified key
// return: 0 on success, -1 on failure
int32_t linked_hash_del_key(struct linked_hash* h, const void *key);

// delete entry fron the linked hash with the specified position
// return: 0 on success, -1 on failure
int32_t linked_hash_del_pos(struct linked_hash* h, int32_t pos);

// iterates through the linked hash from front to back, the value pointed to by next should be set
// to 0 to start the iteration from the front
// return: the positive position of the entry reached by the iteration call, 
// -1 on reaching the end of the linked hash or failure
int32_t linked_hash_iterate(const struct linked_hash* h, void **key, void **data, int32_t *next);

// get the number of entries in the linked hash structure
// return: the number entries
int32_t linked_hash_count(const struct linked_hash* h);

// free memory associated with the linked hash
void linked_hash_free(struct linked_hash* h);

//some possible other functions that can be added:

//int32_t linked_hash_back(const struct linked_hash* h, void **key, void **data);
//int32_t linked_hash_move_key_to_front(struct linked_hash* h, const void *key);
//int32_t linked_hash_move_key_to_back(struct linked_hash* h, const void *key);
//int32_t linked_hash_lookup_data_with_pos(const struct linked_hash* h, int32_t pos, void **data);