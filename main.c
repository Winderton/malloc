#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define w_char char
#define w_u8_t uint8_t
#define w_u16_t uint16_t

#define STACK_SIZE 32
#define HEAP_SIZE (STACK_SIZE * 4)
#define HEADER_SIZE sizeof(size_t)

typedef struct
{
    w_u8_t   stack[STACK_SIZE];
    w_char** unmapped;
    w_u8_t   heap[HEAP_SIZE];

    struct
    {
        w_char** data;
        w_char** bss;
        w_char*  text;
    } data_t;

} w_vmem_t;

typedef struct w_node
{
    struct w_node* next;
    size_t         size;
} w_node_t;

typedef enum
{
    W_ALLOC_FIRST,
    W_ALLOC_BEST
} w_alloc_policy_t;

typedef struct
{
    w_u8_t*          data;
    w_node_t*        head;
    size_t           used;
    w_alloc_policy_t policy;
} w_alloc_t;

static w_alloc_t* g_alloc;
static w_vmem_t   g_mem;

void w_node_find_first(w_alloc_t* alloc, size_t size, w_node_t** out_prev, w_node_t** out_cur)
{
    w_node_t* prev = NULL;
    w_node_t* cur = alloc->head;
    w_node_t* first_prev = NULL;
    w_node_t* first_cur = NULL;

    while(cur)
    {
        if(cur->size >= size)
        {
            first_prev = prev;
            first_cur = cur;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    *out_prev = first_prev;
    *out_cur = first_cur;
}

void w_node_find_best(w_alloc_t* alloc, size_t size, w_node_t** out_prev, w_node_t** out_cur)
{
    w_node_t* prev = NULL;
    w_node_t* cur = alloc->head;
    size_t min_diff = SIZE_MAX;
    w_node_t* best_prev = NULL;
    w_node_t* best_cur = NULL;

    while(cur)
    {
        if(cur->size >= size && (cur->size - size) < min_diff)
        {
            min_diff = cur->size - size;
            best_prev = prev;
            best_cur = cur;
        }
        prev = cur;
        cur = cur->next;
    }

    *out_prev = best_prev;
    *out_cur = best_cur;
}

void w_node_find(w_alloc_t* alloc, size_t size, w_node_t** out_prev, w_node_t** out_cur)
{
    switch(alloc->policy)
    {
        case W_ALLOC_FIRST:
            return w_node_find_first(alloc, size, out_prev, out_cur);
        case W_ALLOC_BEST:
            return w_node_find_best(alloc, size, out_prev, out_cur);
        default:
            *out_prev = *out_cur = NULL;
            break;
    }
}

void w_node_insert(w_alloc_t* alloc, w_node_t* prev, w_node_t* node)
{
    if(!prev)
    {
        node->next = alloc->head;
        alloc->head = node;
    }
    else
    {
        node->next = prev->next;
        prev->next = node;
    }
}

void w_node_remove(w_alloc_t* alloc, w_node_t* prev, w_node_t* node)
{
    if(!prev)
    {
        alloc->head = node->next;
    }
    else
    {
        prev->next = node->next;
    }
}

void w_coalescence(w_alloc_t* alloc, w_node_t* prev, w_node_t* node)
{
    if(node->next && (w_u8_t*)node + node->size == (w_u8_t*)node->next)
    {
        node->size += node->next->size;
        w_node_remove(alloc, node, node->next);
    }

    if(prev && (w_u8_t*)prev + prev->size == (w_u8_t*)node)
    {
        prev->size += node->size;
        w_node_remove(alloc, prev, node);
    }
}

void* w_allocate(w_alloc_t* alloc, size_t size)
{
    w_node_t* prev = NULL;
    w_node_t* node = NULL;

    w_node_find(alloc, size, &prev, &node);
    assert(node);

    size_t diff = node->size - size;

    if(diff > HEADER_SIZE)
    {
        w_node_t* free_node = (w_node_t*)((w_u8_t*)node + size);
        w_node_insert(alloc, node, free_node);
        free_node->size = diff;
    }

    w_node_remove(alloc, prev, node);

    w_u8_t* header = (w_u8_t*)node;
    w_u8_t* data = (w_u8_t*)node + HEADER_SIZE;

    *header = size;
    alloc->used += size;

    return data;
}

void w_deallocate(w_alloc_t* alloc, void* ptr)
{
    w_u8_t* data = (w_u8_t*)ptr;
    w_u8_t* header = data - HEADER_SIZE;
    w_node_t* free_node = (w_node_t*)header;

    free_node->size = *header;
    free_node->next = NULL;

    w_node_t* prev = NULL;
    w_node_t* node = alloc->head;

    while(node)
    {
        if((w_u8_t*)ptr < (w_u8_t*)node)
        {
            w_node_insert(alloc, prev, free_node);
            break;
        }
        prev = node;
        node = node->next;
    }

    alloc->used -= free_node->size;
    w_coalescence(alloc, prev, free_node);
}

w_alloc_t* w_alloc_create(size_t size)
{
    w_alloc_t* alloc = malloc(sizeof(w_alloc_t)); // For now use standard malloc
    memset(alloc, 0, sizeof(w_alloc_t));
    alloc->data = g_mem.heap;

    w_node_t* node = (w_node_t*)alloc->data;
    node->next = NULL;
    node->size = HEAP_SIZE;

    w_node_insert(alloc, NULL, node);
    alloc->used = 0;
    alloc->policy = W_ALLOC_BEST; // Test different options
    return alloc;
}

void w_print_info()
{
    w_node_t* node = g_alloc->head;

    size_t num_nodes = 0;

    while(node)
    {
        ++num_nodes;
        printf("%lu) size=%lu\n", num_nodes, node->size);
        node = node->next;
    }

    printf("num_nodes=%lu, used=%lu\n\n", num_nodes, g_alloc->used);
}

void* w_malloc(size_t size)
{
    assert(size >= sizeof(w_node_t));
    if(!g_alloc)
    {
        g_alloc = w_alloc_create(size);
    }

    void* ptr = w_allocate(g_alloc, size);
    w_print_info();
    return ptr;
}

void w_free(void* ptr)
{
    assert(g_alloc);
    w_deallocate(g_alloc, ptr);
    w_print_info();
}

int main(int argc, char** argv)
{
    typedef struct
    {
        int a;
        int b;
        int c;
        int d;
    } foo_t;

    foo_t* a = w_malloc(sizeof(foo_t));
    foo_t* b = w_malloc(sizeof(foo_t));

    a->a = 1;
    a->b = 12;

    b->a = 2;
    b->b = 22;

    printf("foo[a]: a=%d b=%d\n", a->a, a->b);
    printf("foo[b]: a=%d b=%d\n", b->a, b->b);

    w_free(b);
    w_free(a);

	return 0;
}