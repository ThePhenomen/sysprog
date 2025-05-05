#define _GNU_SOURCE

#include "chat.h"
#include "buffer.h"
#include "chat_client.h"

#include <sys/epoll.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <stdbool.h>
#include <assert.h>

#define MAX_EVENTS 1

struct message_node
{
    struct chat_message *msg;
    struct message_node *next;
};

struct chat_client
{
    int socket;
    int epoll_fd;
    struct buffer input_buffer;
    struct buffer output_buffer;
    struct message_node *msg_queue_head;
    struct message_node *msg_queue_tail;
    bool connected;
    bool connect_in_progress;
    bool epoll_registered;
    bool needs_write;
    int last_error;
};

static void 
client_queue_push(struct chat_client *client, struct chat_message *msg) 
{
    struct message_node *node = malloc(sizeof(struct message_node));
    if (!node) {
        chat_message_delete(msg);
        return;
    }

    node->msg = msg;
    node->next = NULL;
    if (client->msg_queue_tail) 
        client->msg_queue_tail->next = node;
    else 
        client->msg_queue_head = node;
    client->msg_queue_tail = node;
}

struct chat_message*
chat_client_pop_next(struct chat_client *client)
{
    if (!client || !client->msg_queue_head) 
        return NULL;

    struct message_node *head_node = client->msg_queue_head;
    struct chat_message *msg = head_node->msg;
    client->msg_queue_head = head_node->next;
    if (client->msg_queue_head == NULL) 
        client->msg_queue_tail = NULL;
    free(head_node);
    return msg;
}

static int 
client_update_events(struct chat_client *client) 
{
    if (!client || client->epoll_fd < 0 || client->socket < 0) {
        if (client) 
            client->epoll_registered = false;
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLET | EPOLLRDHUP;

    if (client->connect_in_progress) {
        ev.events |= EPOLLOUT;
    } 
    else if (client->connected) {
        ev.events |= EPOLLIN;
        if (client->needs_write)
            ev.events |= EPOLLOUT;
    } 
    else {
        if (client->epoll_registered)
            client->epoll_registered = false;
        return 0; 
    }

    ev.data.ptr = client;

    if (client->epoll_registered) {
        if (epoll_ctl(client->epoll_fd, EPOLL_CTL_DEL, client->socket, NULL) == -1)
            client->epoll_registered = false;
    }

    if (epoll_ctl(client->epoll_fd, EPOLL_CTL_ADD, client->socket, &ev) == -1) {
        client->epoll_registered = false;
        client->last_error = CHAT_ERR_SYS;
        return -1;
    }

    client->epoll_registered = true;
    client->last_error = 0;
    return 0;
}

struct chat_client*
chat_client_new(const char *name) 
{
    struct chat_client *client = calloc(1, sizeof(*client));
    if (!client) 
        return NULL;

    (void)name;
    client->socket = -1;
    client->epoll_fd = -1;
    client->connected = false;
    client->connect_in_progress = false;
    client->epoll_registered = false;
    client->needs_write = false;
    client->last_error = 0;
    client->msg_queue_head = NULL;
    client->msg_queue_tail = NULL;

    client->epoll_fd = epoll_create1(0);
    if (client->epoll_fd == -1) {
        free(client);
        return NULL;
    }

    if (buffer_init(&client->input_buffer, INITIAL_BUFFER_SIZE) != 0 || buffer_init(&client->output_buffer, INITIAL_BUFFER_SIZE) != 0)
    {
        close(client->epoll_fd);
        buffer_free(&client->input_buffer);
        free(client);
        return NULL;
    }

    return client;
}

void
chat_client_delete(struct chat_client *client)
{
    if (!client) 
        return;

    if (client->socket >= 0) {
        if (client->epoll_registered && client->epoll_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOLL_CTL_DEL, client->socket, NULL);
        close(client->socket);
        client->socket = -1;
    }
    client->epoll_registered = false;

    if (client->epoll_fd >= 0) {
        close(client->epoll_fd);
        client->epoll_fd = -1;
    }

    struct chat_message *msg;
    while ((msg = chat_client_pop_next(client)) != NULL)
        chat_message_delete(msg);

    buffer_free(&client->input_buffer);
    buffer_free(&client->output_buffer);
    free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr_str)
{
    if (!client || !addr_str) 
        return CHAT_ERR_INVALID_ARGUMENT;

    if (client->socket >= 0 || client->connect_in_progress || client->connected)
        return CHAT_ERR_ALREADY_STARTED;

    client->last_error = 0;

    char *addr_copy = strdup(addr_str);
    assert(addr_copy);

    char *host = addr_copy;
    char *port_str = strrchr(addr_copy, ':');
    if (!port_str || host == port_str || *(port_str + 1) == '\0') {
        free(addr_copy);
        client->last_error = CHAT_ERR_INVALID_ARGUMENT;
        return client->last_error;
    }
    *port_str = '\0';
    port_str++;

    struct addrinfo hints, *servinfo, *p;
    int rv;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(host, port_str, &hints, &servinfo)) != 0) {
        free(addr_copy);
        client->last_error = CHAT_ERR_SYS;
        return client->last_error;
    }
    free(addr_copy);

    client->socket = -1;
    for (p = servinfo; p != NULL; p = p->ai_next) {
        client->socket = socket(p->ai_family, p->ai_socktype | SOCK_NONBLOCK, p->ai_protocol);
        if (client->socket == -1)
            continue;

        int rc = connect(client->socket, p->ai_addr, p->ai_addrlen);
        if (rc == 0) {
            client->connected = true;
            client->connect_in_progress = false;
            client->needs_write = (client->output_buffer.length > client->output_buffer.processed);
            
            if (client_update_events(client) != 0) {
                 close(client->socket);
                 client->socket = -1;
                 client->connected = false;
                 continue;
            }
            break;
        } 
        else if (errno == EINPROGRESS) {
            client->connect_in_progress = true;
            client->connected = false;
            client->needs_write = false;
            
            if (client_update_events(client) != 0) {
                close(client->socket);
                client->socket = -1;
                client->connect_in_progress = false;
                continue;
            }
            break;
        } 
        else {
            close(client->socket);
            client->socket = -1;
            continue;
        }
    }

    freeaddrinfo(servinfo);

    if (client->socket == -1) {
        if (client->last_error == 0) 
            client->last_error = CHAT_ERR_SYS;
        
        return client->last_error;
    }

    client->last_error = 0;
    return 0;
}

static int 
check_connection_status(struct chat_client *client) 
{
    if (!client || !client->connect_in_progress || client->socket < 0) {
        return CHAT_ERR_SYS;
    }

    int sock_err = 0;
    socklen_t err_len = sizeof(sock_err);
    if (getsockopt(client->socket, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) == -1) {
        client->last_error = CHAT_ERR_SYS;
        client->connect_in_progress = false;
        return client->last_error;
    }

    client->connect_in_progress = false;

    if (sock_err == 0) {
        client->connected = true;
        client->last_error = 0;
        client->needs_write = (client->output_buffer.length > client->output_buffer.processed);
        
        if (client_update_events(client) != 0) {
            client->connected = false;
            return client->last_error;
        }

        return 0;
    } 
    else {
        client->connected = false;
        client->last_error = CHAT_ERR_SYS;
        return client->last_error;
    }
}

static int 
process_input_buffer(struct chat_client *client) 
{
    if (!client) 
        return CHAT_ERR_INVALID_ARGUMENT;

    char *current_pos = client->input_buffer.data;
    size_t remaining_len = client->input_buffer.length;
    size_t bytes_processed = 0;

    while (remaining_len > 0) {
        char *newline = memchr(current_pos, '\n', remaining_len);
        if (!newline) 
            break; 

        size_t msg_len = newline - current_pos;
        struct chat_message *msg = malloc(sizeof(*msg));
        if (!msg)
            return CHAT_ERR_SYS;

        msg->data = malloc(msg_len + 1);
        if (!msg->data) { 
            free(msg);
            return CHAT_ERR_SYS;
        }

        memcpy(msg->data, current_pos, msg_len);
        msg->data[msg_len] = '\0';
        client_queue_push(client, msg);

        size_t consumed_total = msg_len + 1;
        current_pos += consumed_total;
        remaining_len -= consumed_total;
        bytes_processed += consumed_total;
    }

    if (bytes_processed > 0)
        buffer_consume(&client->input_buffer, bytes_processed);
    return 0;
}


int 
chat_client_update(struct chat_client *client, double timeout) 
{
    if (!client) 
        return CHAT_ERR_INVALID_ARGUMENT;

    if (client->socket < 0 && !client->connect_in_progress)
        return (client->last_error != 0) ? client->last_error : CHAT_ERR_NOT_STARTED;

    struct epoll_event events[MAX_EVENTS];
    int nfds = epoll_wait(client->epoll_fd, events, MAX_EVENTS, timeout);

    if (nfds < 0) {
        if (errno == EINTR) 
            return 0;
        client->last_error = CHAT_ERR_SYS;
        return client->last_error;
    }

    if (nfds == 0)
        return CHAT_ERR_TIMEOUT;

    uint32_t revents = events[0].events;
    int result = 0;
    bool needs_epoll_update = false;

    if (revents & (EPOLLERR | EPOLLHUP)) {
        if (client->connect_in_progress) {
            result = check_connection_status(client);
        } 
        else {
            client->last_error = CHAT_ERR_SYS; 
            result = client->last_error;
        }

        revents &= ~(EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    } 
    else if (revents & EPOLLRDHUP) {
        client->last_error = CHAT_ERR_SYS;
        result = client->last_error;
        revents &= ~(EPOLLIN | EPOLLOUT);
    }

    if (result == 0 && (revents & EPOLLOUT)) {
        if (client->connect_in_progress)
            result = check_connection_status(client);

        if (result == 0 && client->connected && client->output_buffer.length > client->output_buffer.processed) {
            struct buffer *out_buf = &client->output_buffer;
            size_t to_send = out_buf->length - out_buf->processed;

            while (to_send > 0) {
                ssize_t sent = send(client->socket, out_buf->data + out_buf->processed, to_send, MSG_NOSIGNAL);
                if (sent > 0) {
                    out_buf->processed += sent;
                    to_send -= sent;
                } 
                else if (sent == 0) {
                    client->last_error = CHAT_ERR_SYS; 
                    result = client->last_error;
                    break;
                } 
                else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (!client->needs_write) {
                            client->needs_write = true;
                            needs_epoll_update = true;
                        }
                        break;
                    } 
                    else if (errno == EPIPE || errno == ECONNRESET) {
                        client->last_error = CHAT_ERR_SYS;
                        result = client->last_error;
                        break;
                    } 
                    else {
                        client->last_error = CHAT_ERR_SYS;
                        result = client->last_error;
                        break;
                    }
                }
            }

            if (result == 0 && out_buf->processed == out_buf->length) {
                out_buf->length = 0;
                out_buf->processed = 0;
                if (client->needs_write) {
                    client->needs_write = false;
                    needs_epoll_update = true;
                }
            }
        }
    }

    if (result == 0 && (revents & EPOLLIN)) {
        char read_buf[4096];
        ssize_t received;
        bool read_occurred = false;
        bool read_error = false;

        while (true) {
            received = recv(client->socket, read_buf, sizeof(read_buf), 0);
            if (received > 0) {
                read_occurred = true;
                if (buffer_append(&client->input_buffer, read_buf, received) != 0) {
                    client->last_error = CHAT_ERR_SYS; 
                    result = client->last_error;
                    read_error = true;
                    break;
                }
            } 
            else if (received == 0) {
                client->last_error = CHAT_ERR_SYS; 
                result = client->last_error;
                read_error = true;
                break;
            } 
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } 
                else if (errno == ECONNRESET) {
                    client->last_error = CHAT_ERR_SYS; 
                    result = client->last_error;
                    read_error = true;
                    break;
                } 
                else {
                    client->last_error = CHAT_ERR_SYS; 
                    result = client->last_error;
                    read_error = true;
                    break;
                }
            }
        }

        if (result == 0 && read_occurred && !read_error) {
            int process_res = process_input_buffer(client);
            if (process_res != 0) {
                client->last_error = process_res; 
                result = client->last_error;
            }
        }
    }

    if (result != 0 && result != CHAT_ERR_TIMEOUT) {
        if (client->socket >= 0) {
            if (client->epoll_registered && client->epoll_fd >=0) {
                epoll_ctl(client->epoll_fd, EPOLL_CTL_DEL, client->socket, NULL);
                client->epoll_registered = false;
            }
            close(client->socket);
            client->socket = -1;
        }
        client->connected = false;
        client->connect_in_progress = false;
        client->needs_write = false;
        client->last_error = result;
        return result;
    }

    if (needs_epoll_update && client->socket >= 0) {
        if (client_update_events(client) != 0) {
        if (result == 0) 
            result = client->last_error; 
        if (client->socket >= 0) {
            if (client->epoll_registered && client->epoll_fd >=0) {
                epoll_ctl(client->epoll_fd, EPOLL_CTL_DEL, client->socket, NULL);
                client->epoll_registered = false;
            }
            close(client->socket); client->socket = -1;
        }
        client->connected = false; client->connect_in_progress = false; client->needs_write = false;
        client->last_error = result;
        return result;
        }
    }

    if (result == 0) {
        if (client->msg_queue_head != NULL)
            return 0;

        if (client->input_buffer.length > 0)
            return 0;

        if (client->needs_write) {
            return 0;
        }

        return CHAT_ERR_TIMEOUT;

    } 
    else {
        return result;
    }
}


int
chat_client_get_descriptor(const struct chat_client *client) 
{
    return client->socket;
}

int
chat_client_get_events(const struct chat_client *client) 
{
    if (!client || (client->socket < 0 && !client->connect_in_progress))
        return 0;

    int events = 0;
    if (client->connect_in_progress) {
        events |= CHAT_EVENT_OUTPUT;
    } 
    else if (client->connected) {
        events |= CHAT_EVENT_INPUT;
        if (client->output_buffer.length > client->output_buffer.processed)
            events |= CHAT_EVENT_OUTPUT;
    } 
    else {
        return 0;
    }
    return events;
}

int
chat_client_feed(struct chat_client *client, const char *msg_in, uint32_t msg_size) 
{
    if (!client || !msg_in || msg_size == 0) 
        return CHAT_ERR_INVALID_ARGUMENT;

    if (client->socket < 0 && !client->connect_in_progress) {
        return (client->last_error != 0) ? client->last_error : CHAT_ERR_NOT_STARTED;
    }
     if (client->last_error != 0 && client->last_error != CHAT_ERR_TIMEOUT) {
        return client->last_error;
    }

    if (buffer_ensure_space(&client->output_buffer, msg_size + 1) != 0) {
        client->last_error = CHAT_ERR_SYS;
        return client->last_error;
    }
    memcpy(client->output_buffer.data + client->output_buffer.length, msg_in, msg_size);
    client->output_buffer.length += msg_size;
    client->output_buffer.data[client->output_buffer.length] = '\0';

    bool needs_update = false;
    if (!client->needs_write) {
        client->needs_write = true;
        needs_update = true;
    }

    if (needs_update && (client->connected || client->connect_in_progress) && client->socket >= 0) {
        if (client_update_events(client) != 0)
            return client->last_error;
    }

    return 0;
}