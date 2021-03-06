/*
 * This module keeps an array of data items that are all the same size. Usually
 * these data items will be a struct of some kind. The data is re-allocated and
 * copied into the list in order to facilitate free()ing it.
 */
#include "common.h"

/*
 * This resizes the list to grow it. This should always be called before
 * adding aything to it.
 *
 * Aborts the program upon failure.
 */
static void resize_list(data_list_t* list)
{
    if(list->nitems + 2 > list->capacity)
    {
        list->capacity = list->capacity << 1;
        list->buffer = REALLOC(list->buffer, list->capacity * list->item_size);

        if(list->buffer == NULL)
            fatal_error("cannot allocate %lu bytes for managed buffer", list->capacity * list->item_size);
    }
}

/*
 * Internal function that calculates a pointer into the buffer, given the index
 * of the intended item. This returns a pointer to the first byte of the item
 * that was stored. It's up to the caller to convert it to a pointer to the data
 * object that it knows about.
 */
static unsigned char* list_at_index(data_list_t* list, int index)
{
    unsigned char* ptr = &list->buffer[list->item_size * index];

    return ptr;
}

/*
 * Free the list buffer. This is only used when the list will not longer
 * be used, such as when the program ends.
 */
void destroy_data_list(data_list_t* list)
{
    if(list->buffer != NULL && list->nitems != 0)
        FREE(list->buffer);
    FREE(list);
}

void init_data_list(data_list_t* list, size_t size) {

    list->item_size = size;
    list->capacity = 0x01 << 3;
    list->nitems = 0;
    list->index = 0;
    list->buffer = (uint8_t*)CALLOC(list->capacity, size);
    if(list->buffer == NULL)
        fatal_error("cannot allocate %lu bytes for managed list buffer", list->capacity * size);
}

/*
 * Initially create the list and initialize the contents to initial values.
 * If the list was in use before this, the buffer will be freed.
 *
 * Size is the size of each item that that will be put in the list.
 */
data_list_t* create_data_list(size_t size)
{
    data_list_t* list;

    list = (data_list_t*)MALLOC(sizeof(data_list_t));
    if(list == NULL)
        fatal_error("cannot allocate %lu bytes for managed list", sizeof(data_list_t));

    init_data_list(list, size);

    return list;
}

/*
 * Store the given item in the given list at the end of the list. This is
 * the ONLY way to store something in the list.
 */
void append_data_list(data_list_t* list, void* item)
{
    resize_list(list);
    memcpy(list_at_index(list, list->nitems), item, list->item_size);
    list->nitems++;
}

/*
 * If the index is within the bounds of the list, then return a raw pointer to
 * the element specified. If it is outside the list, or if there is nothing in
 * the list, then return NULL.
 */
void* get_data_list_by_index(data_list_t* list, int index)
{
    if(list != NULL)
    {
        if(index >= 0 && index < (int)list->nitems)
        {
            return (void*)list_at_index(list, index);
        }
    }

    return NULL;  // failed
}

/*
 * Return the next pointer to the raw list, given by the internal index.
 * This is used to iterate the list in order. Returns NULL for the end
 * of the list.
 */
void* get_data_list_next(data_list_t* list) {

    void* retv = NULL;

    if(list->index < list->nitems) {
        retv = (void*)list_at_index(list, list->index);
        list->index++;
        return retv;
    }

    return NULL;
}

/*
 * Reset the internal index to the beginning of the list.
 */
void reset_data_list(data_list_t* list) {
    list->index = 0;
}

