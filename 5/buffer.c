#include "buffer.h"

int 
buffer_init(struct buffer *buf, size_t initial_capacity)
 {
    buf->data = malloc(initial_capacity);
    if (!buf->data) 
        return -1;
    buf->capacity = initial_capacity;
    buf->length = 0; buf->processed = 0;
    buf->data[0] = '\0'; 
    return 0;
}

void 
buffer_free(struct buffer *buf) 
{
    free(buf->data); 
    buf->data = NULL;
    buf->capacity = 0;
    buf->length = 0;
    buf->processed = 0;
}

int 
buffer_ensure_space(struct buffer *buf, size_t needed) 
{
    if (buf->capacity >= buf->length + needed) 
        return 0;

    size_t new_capacity = buf->capacity;
    while (new_capacity < buf->length + needed)
        new_capacity = (new_capacity == 0) ? INITIAL_BUFFER_SIZE : new_capacity * 2;
    char *new_data = realloc(buf->data, new_capacity);
    if (!new_data) 
        return -1;

    buf->data = new_data; 
    buf->capacity = new_capacity;
    return 0;
}

int 
buffer_append(struct buffer *buf, const char *data, size_t size) 
{
    if (buffer_ensure_space(buf, size + 1) != 0) 
        return -1;
    memcpy(buf->data + buf->length, data, size);
    buf->length += size;
    buf->data[buf->length] = '\0';
    return 0;
}

void 
buffer_consume(struct buffer *buf, size_t count) 
{
    if (count == 0) 
        return;

    if (count >= buf->length) {
        buf->length = 0; buf->processed = 0;
    } 
    else {
        memmove(buf->data, buf->data + count, buf->length - count);
        buf->length -= count;
        buf->processed = (buf->processed >= count) ? buf->processed - count : 0;
    }

    if (buf->capacity > 0) 
        buf->data[buf->length] = '\0';
}