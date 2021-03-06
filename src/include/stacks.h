#ifndef __STACK_H__
#define __STACK_H__

enum {
    STACK_EMPTY = -1,
    STACK_INVALID = 123,
    STACK_NO_ERROR,
};

typedef struct _stack_item
{
    int type;
    size_t size;
    void* data;
    struct _stack_item* next;
} stack_item_t;

typedef struct
{
    stack_item_t* items;
} stack_t;

stack_t* create_stack(void);
void destroy_stack(stack_t* stack);
int push_stack(stack_t* stack, void* data, size_t size, int type);
int pop_stack(stack_t* stack, void* data, size_t size);
int peek_stack(stack_t* stack, void* data, size_t size);

#endif
