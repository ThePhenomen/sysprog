#include <stdlib.h>
#include <string.h>

#define INITIAL_BUFFER_SIZE 1024

struct buffer
{
    char *data;
    size_t capacity;
    size_t length;
    size_t processed;
};

int buffer_init(struct buffer *buf, size_t initial_capacity);
void buffer_free(struct buffer *buf);
int buffer_ensure_space(struct buffer *buf, size_t needed);
int buffer_append(struct buffer *buf, const char *data, size_t size);
void buffer_consume(struct buffer *buf, size_t count);