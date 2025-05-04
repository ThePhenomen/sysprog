#define _GNU_SOURCE

#include "chat.h"
#include "buffer.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdbool.h>
#include <assert.h>

#define MAX_EVENTS 64
#define MAX_PEERS 1024

struct chat_peer 
{
    int socket;
    struct buffer input_buffer;
    struct buffer output_buffer;
    struct chat_server *server_ref;
    bool epoll_registered;
    bool needs_write;
};

struct server_msg_node 
{
    struct chat_message *msg;
    struct server_msg_node *next;
};

struct chat_server 
{
    int socket;
    int epoll_fd;
    struct chat_peer **peers;
    size_t peer_count;
    size_t peer_capacity;
    struct server_msg_node *msg_queue_head;
    struct server_msg_node *msg_queue_tail;
    struct buffer server_input_buffer;
};

static void 
server_queue_push(struct chat_server *server, struct chat_message *msg, bool is_server_msg) 
{
    struct server_msg_node *node = malloc(sizeof(*node));
    if (!node) {
        chat_message_delete(msg);
        return;
    }
    node->msg = msg;
    node->next = NULL;
    msg->is_server_message = is_server_msg;

    if (server->msg_queue_tail)
        server->msg_queue_tail->next = node;
    else
        server->msg_queue_head = node;
    server->msg_queue_tail = node;
}

struct chat_message*
chat_server_pop_next(struct chat_server *server)
{
    if (!server || !server->msg_queue_head)
        return NULL;

    struct server_msg_node *node = server->msg_queue_head;
    struct chat_message *m = node->msg;
    server->msg_queue_head = node->next;
    if (!server->msg_queue_head)
        server->msg_queue_tail = NULL;
    free(node);
    return m;
}

static int 
server_update_events(struct chat_peer *peer) 
{
    if (!peer || !peer->server_ref || peer->server_ref->epoll_fd < 0 || peer->socket < 0)
        return -1;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (peer->needs_write)
        ev.events |= EPOLLOUT;
    ev.data.ptr = peer;

    if (peer->epoll_registered) {
        if (epoll_ctl(peer->server_ref->epoll_fd, EPOLL_CTL_DEL, peer->socket, NULL) == -1) {
            if (errno != ENOENT)
                return -1;
        }
    }

    if (epoll_ctl(peer->server_ref->epoll_fd, EPOLL_CTL_ADD, peer->socket, &ev) == -1) {
        peer->epoll_registered = false;
        return -1;
    }

    peer->epoll_registered = true;
    return 0;
}

static struct chat_peer* 
peer_new(int sock, struct chat_server *server) 
{
    struct chat_peer *peer = calloc(1, sizeof(struct chat_peer));
    if (!peer) {
        close(sock);
        return NULL;
    }

    peer->socket = sock;
    peer->server_ref = server;
    peer->epoll_registered = false;
    peer->needs_write = false;

    if (buffer_init(&peer->input_buffer, INITIAL_BUFFER_SIZE) != 0 || buffer_init(&peer->output_buffer, INITIAL_BUFFER_SIZE) != 0) {
        free(peer);
        close(sock);
        return NULL;
    }
    return peer;
}

static void 
peer_free(struct chat_peer *peer) 
{
    if (!peer) 
        return;

    if (peer->socket >= 0) {
        close(peer->socket);
        peer->socket = -1;
    }
    buffer_free(&peer->input_buffer);
    buffer_free(&peer->output_buffer);
    free(peer);
}

static int 
server_add_peer(struct chat_server *server, struct chat_peer *peer) 
{
    if (server->peer_count >= MAX_PEERS)
        return -1;

    if (server->peer_count >= server->peer_capacity) {
        size_t new_capacity = (server->peer_capacity == 0) ? 16 : server->peer_capacity * 2;
        struct chat_peer **new_peers = realloc(server->peers, new_capacity * sizeof(struct chat_peer *));
        if (!new_peers)
            return -1;

        server->peers = new_peers;
        server->peer_capacity = new_capacity;
    }

    server->peers[server->peer_count] = peer;
    server->peer_count++;
    return 0;
}

static void 
server_remove_peer(struct chat_server *server, struct chat_peer *peer_to_remove) 
{
    if (!server || !peer_to_remove) 
        return;

    size_t i;
    bool found = false;
    for (i = 0; i < server->peer_count; ++i) {
        if (server->peers[i] == peer_to_remove) {
            peer_free(server->peers[i]);
            server->peers[i] = NULL;

            if (i < server->peer_count - 1) {
                memmove(&server->peers[i],
                        &server->peers[i + 1],
                        (server->peer_count - 1 - i) * sizeof(server->peers[0]));
            }
            server->peer_count--;
            found = true;
            break;
        }
    }
    if (!found)
        peer_free(peer_to_remove);
}

struct chat_server*
chat_server_new(void)
{
    struct chat_server *server = calloc(1, sizeof(*server));
    if (!server) 
        return NULL;

    server->socket = -1;
    server->epoll_fd = -1;
    server->peers = NULL;
    server->peer_count = 0;
    server->peer_capacity = 0;
    server->msg_queue_head = NULL;
    server->msg_queue_tail = NULL;

    if (buffer_init(&server->server_input_buffer, INITIAL_BUFFER_SIZE) != 0) {
        free(server);
        return NULL;
    }

    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd == -1) {
        buffer_free(&server->server_input_buffer);
        free(server);
        return NULL;
    }

    return server;
}

void
chat_server_delete(struct chat_server *server)
{
    if (!server) 
        return;

    if (server->socket >= 0) {
        close(server->socket);
        server->socket = -1;
    }

    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }

    for (size_t i = 0; i < server->peer_count; ++i) {
        if (server->peers[i])
            peer_free(server->peers[i]);
    }
    free(server->peers);

    struct chat_message *msg;
    while ((msg = chat_server_pop_next(server)) != NULL) {
        chat_message_delete(msg);
    }

    buffer_free(&server->server_input_buffer);
    free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
    if (server->socket >= 0) {
        return CHAT_ERR_ALREADY_STARTED;
    }
    if (server->epoll_fd < 0) {
        return CHAT_ERR_SYS;
    }

    struct sockaddr_in addr;
    int opt = 1;

    server->socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server->socket == -1)
        return CHAT_ERR_SYS;

    if (setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        close(server->socket);
        server->socket = -1;
        return CHAT_ERR_SYS;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server->socket, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(server->socket);
        server->socket = -1;
        return CHAT_ERR_SYS;
    }

    if (listen(server->socket, SOMAXCONN) == -1) {
        close(server->socket);
        server->socket = -1;
        return CHAT_ERR_SYS;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = server;
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->socket, &ev) == -1) {
        close(server->socket);
        server->socket = -1;
        return CHAT_ERR_SYS;
    }

    return 0;
}

static int 
broadcast_message(struct chat_server *server, struct chat_peer *source, const char *data)
{
    size_t data_len = strlen(data);
    int result = 0;

    for (size_t i = 0; i < server->peer_count; ++i) {
        struct chat_peer *dest = server->peers[i];
        if (!dest || dest == source)
            continue;

        size_t required = data_len + 1;
        if (buffer_ensure_space(&dest->output_buffer, required) != 0) {
            result = CHAT_ERR_SYS;
            continue;
        }

        memcpy(dest->output_buffer.data + dest->output_buffer.length, data, data_len);
        dest->output_buffer.length += data_len;
        dest->output_buffer.data[dest->output_buffer.length++] = '\n';

        if (!dest->needs_write) {
            dest->needs_write = true;
            if (server_update_events(dest) != 0) {
                result = CHAT_ERR_SYS;
            }
        }
    }
    return result;
}

static int 
process_peer_input(struct chat_peer *peer) 
{
    struct chat_server *server = peer->server_ref;
    char *current_pos = peer->input_buffer.data;
    size_t remaining_len = peer->input_buffer.length;
    size_t bytes_processed = 0;
    int result = 0;

    while (remaining_len > 0) {
        char *newline = memchr(current_pos, '\n', remaining_len);
        if (!newline)
            break;

        size_t msg_len = newline - current_pos;

        struct chat_message *srv_msg = malloc(sizeof(*srv_msg));
        if (!srv_msg) {
            result = CHAT_ERR_SYS;
            break;
        }

        srv_msg->data = malloc(msg_len + 1);
        if (!srv_msg->data) {
            free(srv_msg);
            result = CHAT_ERR_SYS;
            break;
        }
        memcpy(srv_msg->data, current_pos, msg_len);
        srv_msg->data[msg_len] = '\0';

        server_queue_push(server, srv_msg, false);

        broadcast_message(server, peer, srv_msg->data);

        size_t consumed_total = msg_len + 1;
        current_pos += consumed_total;
        remaining_len -= consumed_total;
        bytes_processed += consumed_total;
    }

    if (bytes_processed > 0)
        buffer_consume(&peer->input_buffer, bytes_processed);

    return result;
}


static int 
handle_peer_event(struct chat_peer *peer, uint32_t events) 
{
    struct chat_server *server = peer->server_ref;
    int result = 0;
    bool needs_epoll_update = false;

    if ((events & EPOLLERR) || (events & EPOLLHUP) || (events & EPOLLRDHUP)) {
        server_remove_peer(server, peer);
        return 0;
    }

    if (events & EPOLLOUT) {
        struct buffer *out_buf = &peer->output_buffer;
        size_t to_send = out_buf->length - out_buf->processed;

        while (to_send > 0) {
            ssize_t sent = send(peer->socket, out_buf->data + out_buf->processed, to_send, MSG_NOSIGNAL);
            if (sent > 0) {
                out_buf->processed += sent;
                to_send -= sent;
            } 
            else if (sent == 0) {
                server_remove_peer(server, peer);
                return 0;
            } 
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } 
                else if (errno == EPIPE || errno == ECONNRESET) {
                    server_remove_peer(server, peer);
                    return 0;
                } 
                else {
                    server_remove_peer(server, peer);
                    return CHAT_ERR_SYS;
                }
            }
        }

        if (out_buf->processed == out_buf->length) {
            out_buf->length = 0;
            out_buf->processed = 0;
            if (peer->needs_write) {
                peer->needs_write = false;
                needs_epoll_update = true;
            }
        } 
        else {
            if (!peer->needs_write) {
                peer->needs_write = true;
                needs_epoll_update = true;
            }
        }
    }

    if (events & EPOLLIN) {
        char read_buf[4096];
        ssize_t received;
        bool read_error = false;

        while (true) {
            received = recv(peer->socket, read_buf, sizeof(read_buf), 0);

            if (received > 0) {
                if (buffer_append(&peer->input_buffer, read_buf, received) != 0) {
                    result = CHAT_ERR_SYS;
                    read_error = true;
                    server_remove_peer(server, peer);
                    break;
                }
            } 
            else if (received == 0) {
                server_remove_peer(server, peer);
                return 0;
            } 
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } 
                else if (errno == ECONNRESET) {
                    server_remove_peer(server, peer);
                    return 0;
                } else {
                    result = CHAT_ERR_SYS;
                    read_error = true;
                    server_remove_peer(server, peer);
                    break;
                }
            }
        }

        if (!read_error && peer->input_buffer.length > 0) {
            int process_res = process_peer_input(peer);
            if (process_res != 0 && result == 0)
                 result = process_res;
        }
    }

    if (needs_epoll_update && peer->epoll_registered) { 
        if (server_update_events(peer) != 0) {
            if (result == 0) 
                result = CHAT_ERR_SYS;
            server_remove_peer(server, peer);
        }
    }

    return result;
}


static int 
handle_listen_event(struct chat_server *server) 
{
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept4(server->socket, (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK);

        if (client_sock == -1 && errno == ENOSYS) {
             client_sock = accept(server->socket, (struct sockaddr *)&client_addr, &client_len);
             if (client_sock >= 0) {
                int flags = fcntl(client_sock, F_GETFL, 0);
                if (flags == -1 || fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
                    close(client_sock);
                    continue;
                }
             }
        }

        if (client_sock >= 0) {
            struct chat_peer *new_peer = peer_new(client_sock, server);
            if (!new_peer)
                continue;

            if (server_add_peer(server, new_peer) != 0) {
                peer_free(new_peer);
                continue;
            }

            new_peer->needs_write = false;
            if (server_update_events(new_peer) != 0) {
                server_remove_peer(server, new_peer);
                continue;
            }

        } 
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else if (errno == EMFILE || errno == ENFILE)
                break;
            else
                return CHAT_ERR_SYS;
        }
    }
    return 0;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
    if (!server || server->socket < 0 || server->epoll_fd < 0)
        return CHAT_ERR_NOT_STARTED;

    struct epoll_event events[MAX_EVENTS];
    int n_events = epoll_wait(server->epoll_fd, events, MAX_EVENTS, timeout);

    if (n_events < 0) {
        if (errno == EINTR)
            return 0;
        return CHAT_ERR_SYS;
    }
    if (n_events == 0)
        return CHAT_ERR_TIMEOUT;

    int overall_result = 0;
    for (int i = 0; i < n_events; ++i) {
        void *ptr = events[i].data.ptr;
        uint32_t event_flags = events[i].events;
        int current_result = 0;

        if (ptr == server) {
            if (event_flags & EPOLLIN)
                current_result = handle_listen_event(server);
        } else {
            struct chat_peer *peer = (struct chat_peer *)ptr;
            bool peer_found = false;
            for(size_t p_idx = 0; p_idx < server->peer_count; ++p_idx) {
                if (server->peers[p_idx] == peer) {
                    peer_found = true;
                    break;
                }
            }

            if (peer_found && peer->socket >= 0 && peer->epoll_registered)
                 current_result = handle_peer_event(peer, event_flags);
        }

        if (current_result != 0 && current_result != CHAT_ERR_TIMEOUT && overall_result == 0)
            overall_result = current_result;
    }

    return overall_result;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
    return server ? server->epoll_fd : -1;
}

int
chat_server_get_socket(const struct chat_server *server)
{
    return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
    if (!server || server->socket < 0)
        return 0;

    int events = CHAT_EVENT_INPUT;
    for (size_t i = 0; i < server->peer_count; ++i) {
        struct chat_peer *peer = server->peers[i];
        if (peer && peer->output_buffer.length > 0) {
            events |= CHAT_EVENT_OUTPUT;
            break;
        }
    }

    return events;
}

int 
chat_server_feed(struct chat_server *server, const char *msg_in, uint32_t msg_size) 
{
    if (!server || !msg_in || msg_size == 0)
        return CHAT_ERR_INVALID_ARGUMENT;

    if (server->socket < 0 || server->epoll_fd < 0)
        return CHAT_ERR_NOT_STARTED;

    if (buffer_append(&server->server_input_buffer, msg_in, msg_size) != 0)
        return CHAT_ERR_SYS;
    
    chat_server_update(server, 0);
    char *current_pos = server->server_input_buffer.data;
    size_t remaining_len = server->server_input_buffer.length;
    size_t bytes_processed = 0;
    int first_error = 0;

    while (remaining_len > 0) {
        char *newline = memchr(current_pos, '\n', remaining_len);
        if (!newline)
            break; 

        size_t msg_len = newline - current_pos;

        char *temp_msg_data = malloc(msg_len + 1);
        if (!temp_msg_data) {
            if (first_error == 0) 
                first_error = CHAT_ERR_SYS;
            break; 
        }
        memcpy(temp_msg_data, current_pos, msg_len);
        temp_msg_data[msg_len] = '\0';

        struct chat_message *msg = malloc(sizeof(*msg));
        if (!msg) {
            free(temp_msg_data);
            if (first_error == 0) 
                first_error = CHAT_ERR_SYS;
            break;
        }
        msg->data = temp_msg_data;
        msg->is_server_message = true; 

        server_queue_push(server, msg, true);

        int broadcast_res = broadcast_message(server, NULL, temp_msg_data);
        if (broadcast_res != 0 && first_error == 0)
            first_error = broadcast_res;

        size_t consumed_total = msg_len + 1;
        current_pos += consumed_total;
        remaining_len -= consumed_total;
        bytes_processed += consumed_total;
    }

    if (bytes_processed > 0)
        buffer_consume(&server->server_input_buffer, bytes_processed);

    return first_error;
}