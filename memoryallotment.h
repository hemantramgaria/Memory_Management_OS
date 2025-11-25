
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>


#define ARENA_SIZE 4096
#define MAGIC      0xCAFEBABE
#define ALIGN8(x)  (((x) + 7UL) & ~7UL)



typedef struct block_header {
    size_t size;                 
    int    free;               
    uint32_t magic;

  
    struct block_header* prev_phys;
    struct block_header* next_phys;

  
    struct block_header* prev_free;
    struct block_header* next_free;
} header_t;


typedef struct avl_node {
    size_t key_size;            
    header_t* blk;              
    struct avl_node* left;
    struct avl_node* right;
    int height;
} avl_node_t;

static inline int avl_h(avl_node_t* n) { return n ? n->height : 0; }
static inline void avl_upd(avl_node_t* n){
    int hl = avl_h(n->left), hr = avl_h(n->right);
    n->height = (hl>hr?hl:hr)+1;
}

#define AVL_POOL_SIZE 256

static avl_node_t avl_pool[AVL_POOL_SIZE];
static int avl_pool_used = 0;


static avl_node_t* avl_newnode(header_t* blk) {
    if (avl_pool_used >= AVL_POOL_SIZE) {
        fprintf(stderr, "AVL pool exhausted!\n");
        return NULL;   // or exit(1)
    }

    avl_node_t* n = &avl_pool[avl_pool_used++];
    n->key_size = blk->size;
    n->blk = blk;
    n->left = n->right = NULL;
    n->height = 1;
    return n;
}

static int avl_cmp_blk_vs_node(header_t* a_blk, avl_node_t* node){
    if (a_blk->size < node->key_size) return -1;
    if (a_blk->size > node->key_size) return 1;
    if (a_blk <  node->blk) return -1;
    if (a_blk >  node->blk) return 1;
    return 0;
}
static avl_node_t* avl_rotr(avl_node_t* y){
    avl_node_t* x = y->left;
    y->left = x->right; x->right = y;
    avl_upd(y); avl_upd(x);
    return x;
}
static avl_node_t* avl_rotl(avl_node_t* x){
    avl_node_t* y = x->right;
    x->right = y->left; y->left = x;
    avl_upd(x); avl_upd(y);
    return y;
}
static avl_node_t* avl_insert(avl_node_t* root, header_t* blk){
    if (!root) return avl_newnode(blk);
    int c = avl_cmp_blk_vs_node(blk, root);
    if (c < 0) root->left  = avl_insert(root->left, blk);
    else if (c > 0) root->right = avl_insert(root->right, blk);
    else return root;

    avl_upd(root);
    int bf = avl_h(root->left) - avl_h(root->right);
    // LL
    if (bf > 1 && avl_cmp_blk_vs_node(blk, root->left) < 0) return avl_rotr(root);
    // RR
    if (bf < -1 && avl_cmp_blk_vs_node(blk, root->right) > 0) return avl_rotl(root);
    // LR
    if (bf > 1 && avl_cmp_blk_vs_node(blk, root->left) > 0) { root->left = avl_rotl(root->left); return avl_rotr(root); }
    // RL
    if (bf < -1 && avl_cmp_blk_vs_node(blk, root->right) < 0){ root->right = avl_rotr(root->right); return avl_rotl(root); }
    return root;
}
static avl_node_t* avl_min(avl_node_t* r){ while(r && r->left) r=r->left; return r; }

static avl_node_t* avl_delete(avl_node_t* root, header_t* blk) {
    if (!root) return NULL;

    int cmp = avl_cmp_blk_vs_node(blk, root);

    if (cmp < 0) {
        root->left = avl_delete(root->left, blk);
    } else if (cmp > 0) {
        root->right = avl_delete(root->right, blk);
    } else {
        if (!root->left || !root->right) {
            avl_node_t* tmp = root->left ? root->left : root->right;

            

            return tmp;
        } else {
            avl_node_t* min = avl_min(root->right);
            root->key_size = min->key_size;
            root->blk = min->blk;
            root->right = avl_delete(root->right, min->blk);
        }
    }

    avl_upd(root);

    int bf = avl_h(root->left) - avl_h(root->right);

    if (bf > 1 && avl_h(root->left->left) >= avl_h(root->left->right))
        return avl_rotr(root);
    if (bf > 1) {
        root->left = avl_rotl(root->left);
        return avl_rotr(root);
    }
    if (bf < -1 && avl_h(root->right->right) >= avl_h(root->right->left))
        return avl_rotl(root);
    if (bf < -1) {
        root->right = avl_rotr(root->right);
        return avl_rotl(root);
    }

    return root;
}


static header_t* avl_best_fit(avl_node_t* root, size_t need){
    header_t* ans = NULL;
    avl_node_t* cur = root;
    while (cur) { if (cur->key_size >= need) { ans = cur->blk; cur = cur->left; } else cur = cur->right; }
    return ans;
}
static header_t* avl_worst_fit(avl_node_t* root, size_t need){
    header_t* ans = NULL;
    avl_node_t* cur = root;
    while (cur) { if (cur->key_size >= need) { ans = cur->blk; cur = cur->right; } else cur = cur->right; }
    return ans;
}


static header_t* arena_base_nb  = NULL;
static header_t* free_head      = NULL;   
static header_t* nextfit_cur    = NULL;  
static avl_node_t* avl_root     = NULL;  
static int arena_initialized_nb = 0;


enum strategy_mode { MODE_FIRST, MODE_NEXT, MODE_BEST, MODE_WORST, MODE_BUDDY };
static enum strategy_mode current_mode = MODE_FIRST;

static void freelist_remove(header_t* b){
    if (!b) return;
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else              free_head = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->prev_free = b->next_free = NULL;
}
static void freelist_insert_head(header_t* b){
    b->prev_free = NULL;
    b->next_free = free_head;
    if (free_head) free_head->prev_free = b;
    free_head = b;
}

static void init_arena_once_nb(){
    if (arena_initialized_nb) return;
    arena_initialized_nb = 1;

    void* p = mmap(NULL, ARENA_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }

    header_t* h = (header_t*)p;
    h->size = ARENA_SIZE - sizeof(header_t);
    h->free = 1;
    h->magic = MAGIC;
    h->prev_phys = NULL;
    h->next_phys = NULL;
    h->prev_free = NULL;
    h->next_free = NULL;

    arena_base_nb = h;
    free_head     = h;
    nextfit_cur   = h;

}

static header_t* split_block_nb(header_t* b, size_t need){
    size_t asz = ALIGN8(need);
    if (b->size >= asz + sizeof(header_t) + 8) {
        header_t* r = (header_t*)((char*)b + sizeof(header_t) + asz);
        r->size = b->size - asz - sizeof(header_t);
        r->free = 1; r->magic = MAGIC;

        r->prev_phys = b;
        r->next_phys = b->next_phys;
        if (b->next_phys) b->next_phys->prev_phys = r;
        b->next_phys = r;

        freelist_insert_head(r);
        if (current_mode == MODE_BEST || current_mode == MODE_WORST)
            avl_root = avl_insert(avl_root, r);

        b->size = asz;
    }
    return b;
}
static void phys_merge_right_nb(header_t* left, header_t* right){
    left->size += sizeof(header_t) + right->size;
    left->next_phys = right->next_phys;
    if (right->next_phys) right->next_phys->prev_phys = left;
}
static header_t* coalesce_phys_nb(header_t* h){
    header_t* merged = h;

    if (merged->prev_phys && merged->prev_phys->free) {
        header_t* L = merged->prev_phys;
        freelist_remove(L);
        if (current_mode == MODE_BEST || current_mode == MODE_WORST)
            avl_root = avl_delete(avl_root, L);
        phys_merge_right_nb(L, merged);
        merged = L;
    }
    if (merged->next_phys && merged->next_phys->free) {
        header_t* R = merged->next_phys;
        freelist_remove(R);
        if (current_mode == MODE_BEST || current_mode == MODE_WORST)
            avl_root = avl_delete(avl_root, R);
        phys_merge_right_nb(merged, R);
    }
    return merged;
}
static void* allocate_from_block_nb(header_t* b, size_t size){
    split_block_nb(b, size);
    b->free = 0;
    return (void*)(b + 1);
}
static void ensure_avl_initialized_for_bw(void){
    if (avl_root) return;
    for (header_t* cur = free_head; cur; cur = cur->next_free)
        avl_root = avl_insert(avl_root, cur);
}

void* malloc_first_fit(size_t size) {
    init_arena_once_nb();
    current_mode = MODE_FIRST;
    size = ALIGN8(size);

    for (header_t* cur = free_head; cur; cur = cur->next_free) {
        if (cur->free && cur->size >= size) {
            freelist_remove(cur);
            return allocate_from_block_nb(cur, size);
        }
    }
    return NULL;
}
void* malloc_next_fit(size_t size) {
    init_arena_once_nb();
    current_mode = MODE_NEXT;
    size = ALIGN8(size);

    if (!nextfit_cur) nextfit_cur = free_head;
    if (!nextfit_cur) return NULL;

    header_t* start = nextfit_cur;
    header_t* cur = start;

    do {
        if (cur->free && cur->size >= size) {
            freelist_remove(cur);
            void* p = allocate_from_block_nb(cur, size);
            nextfit_cur = free_head;
            return p;
        }
        cur = cur->next_free ? cur->next_free : free_head;
    } while (cur && cur != start);

    return NULL;
}
void* malloc_best_fit(size_t size) {
    init_arena_once_nb();
    current_mode = MODE_BEST;
    size = ALIGN8(size);

    ensure_avl_initialized_for_bw();
    header_t* best = avl_best_fit(avl_root, size);
    if (!best) return NULL;

    avl_root = avl_delete(avl_root, best);
    freelist_remove(best);
    return allocate_from_block_nb(best, size);
}
void* malloc_worst_fit(size_t size) {
    init_arena_once_nb();
    current_mode = MODE_WORST;
    size = ALIGN8(size);

    ensure_avl_initialized_for_bw();
    header_t* worst = avl_worst_fit(avl_root, size);
    if (!worst) return NULL;

    avl_root = avl_delete(avl_root, worst);
    freelist_remove(worst);
    return allocate_from_block_nb(worst, size);
}


#define BUDDY_MIN_BLOCK   32U
#define BUDDY_MIN_ORDER   0
#define BUDDY_MAX_ORDER   7

typedef struct buddy_header {
    uint32_t magic;
    uint16_t order;   
    uint16_t free;    
    struct buddy_header* prev;
    struct buddy_header* next;
} buddy_hdr_t;

static buddy_hdr_t* buddy_free_lists[BUDDY_MAX_ORDER+1] = {0};
static void*        arena_base_buddy = NULL;
static int          buddy_initialized = 0;

static inline size_t buddy_block_size_from_order(uint16_t order){
    return ((size_t)BUDDY_MIN_BLOCK) << order;
}
static inline uint16_t buddy_order_for_size(size_t need_bytes){
    size_t need = need_bytes;
    size_t b = BUDDY_MIN_BLOCK;
    uint16_t order = 0;
    while (b < need && order < BUDDY_MAX_ORDER) { b <<= 1; order++; }
    return order;
}
static inline uintptr_t buddy_offset(void* p){
    return (uintptr_t)p - (uintptr_t)arena_base_buddy;
}
static inline void* buddy_addr_from_offset(uintptr_t off){
    return (void*)((uintptr_t)arena_base_buddy + off);
}
static inline void* buddy_compute_buddy_addr(void* block_addr, uint16_t order){
    size_t blk_size = buddy_block_size_from_order(order);
    uintptr_t off = buddy_offset(block_addr);
    uintptr_t buddy_off = off ^ blk_size;
    return buddy_addr_from_offset(buddy_off);
}
static void buddy_list_remove(uint16_t order, buddy_hdr_t* h){
    if (!h) return;
    if (h->prev) h->prev->next = h->next;
    else         buddy_free_lists[order] = h->next;
    if (h->next) h->next->prev = h->prev;
    h->prev = h->next = NULL;
}
static void buddy_list_push(uint16_t order, buddy_hdr_t* h){
    h->prev = NULL;
    h->next = buddy_free_lists[order];
    if (buddy_free_lists[order]) buddy_free_lists[order]->prev = h;
    buddy_free_lists[order] = h;
}

static void buddy_init_once(){
    if (buddy_initialized) return;
    buddy_initialized = 1;

    arena_base_buddy = mmap(NULL, ARENA_SIZE, PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (arena_base_buddy == MAP_FAILED) { perror("mmap"); exit(1); }

    buddy_hdr_t* h = (buddy_hdr_t*)arena_base_buddy;
    h->magic = MAGIC;
    h->order = BUDDY_MAX_ORDER;     
    h->free  = 1;
    h->prev = h->next = NULL;

    for (int i=0;i<=BUDDY_MAX_ORDER;i++) buddy_free_lists[i]=NULL;
    buddy_list_push(h->order, h);
}

static buddy_hdr_t* buddy_split_down_to(uint16_t target_order){
    uint16_t k = target_order;
    while (k <= BUDDY_MAX_ORDER && buddy_free_lists[k] == NULL) k++;
    if (k > BUDDY_MAX_ORDER) return NULL; // OOM

    buddy_hdr_t* h = buddy_free_lists[k];
    buddy_list_remove(k, h);

    while (k > target_order) {
        k--;
        size_t size_k = buddy_block_size_from_order(k);
        buddy_hdr_t* right = (buddy_hdr_t*)((char*)h + size_k);
        right->magic = MAGIC;
        right->order = k;
        right->free  = 1;
        right->prev = right->next = NULL;

        h->order = k;
        h->free  = 1;

        buddy_list_push(k, right);
    }
    return h; 
}

void* malloc_buddy_alloc(size_t size) {
    current_mode = MODE_BUDDY;
    buddy_init_once();

    size_t payload = ALIGN8(size);
    size_t total_need = sizeof(buddy_hdr_t) + payload;

    if (total_need > ARENA_SIZE) return NULL;
    uint16_t order = buddy_order_for_size(total_need);

    buddy_hdr_t* h = buddy_free_lists[order];
    if (!h) h = buddy_split_down_to(order);
    if (!h) return NULL; 

    buddy_list_remove(order, h);
    h->free = 0;

   
    return (void*)(h + 1);
}


static void buddy_free_impl(void* ptr){
    if (!ptr) return;
    buddy_hdr_t* h = ((buddy_hdr_t*)ptr) - 1;
    if (h->magic != MAGIC) return;

    if (h->free == 1) {
        fprintf(stderr, "Double free detected (buddy) at %p\n", ptr);
        return;
    }


    h->free = 1;


    while (h->order < BUDDY_MAX_ORDER) {
        void* buddy_addr = buddy_compute_buddy_addr((void*)h, h->order);
        buddy_hdr_t* b = (buddy_hdr_t*)buddy_addr;

       
        if ((uintptr_t)buddy_addr < (uintptr_t)arena_base_buddy ||
            (uintptr_t)buddy_addr >= (uintptr_t)arena_base_buddy + ARENA_SIZE ||
            b->magic != MAGIC || b->free != 1 || b->order != h->order) {
            break; 
        }

      
        buddy_list_remove(b->order, b);

      
        buddy_hdr_t* newh = (h < b) ? h : b;
        newh->order = h->order + 1;
        newh->free  = 1;
        newh->magic = MAGIC;
        newh->prev = newh->next = NULL;

        h = newh;
    }

   
    buddy_list_push(h->order, h);
}


void my_free(void* ptr) {
    if (!ptr) return;

    if (current_mode == MODE_BUDDY) {
        buddy_free_impl(ptr);
        return;
    }

    header_t* h = ((header_t*)ptr) - 1;
    if (h->magic != MAGIC) return;    

    if (h->free == 1) {
        fprintf(stderr, "Double free detected at %p\n", ptr);
        return;
    }

    h->free = 1;

    header_t* merged = coalesce_phys_nb(h);

    freelist_insert_head(merged);

    if (current_mode == MODE_BEST || current_mode == MODE_WORST)
        avl_root = avl_insert(avl_root, merged);

    if (current_mode == MODE_NEXT) {
        if (!nextfit_cur ||
            nextfit_cur == h ||
            nextfit_cur == merged ||
            nextfit_cur == merged->prev_phys ||
            nextfit_cur == merged->next_phys) {
            nextfit_cur = free_head;
        }
    }
}
