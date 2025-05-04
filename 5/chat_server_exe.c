#include "chat.h"
#include "chat_server.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static int
port_from_str(const char *str, uint16_t *port)
{
    errno = 0;
    char *end = NULL;
    long res = strtol(str, &end, 10);
    if (res == 0 && errno != 0)
        return -1;
    if (*end != 0)
        return -1;
    if (res > UINT16_MAX || res < 0)
        return -1;
    *port = (uint16_t)res;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    uint16_t port = 0;
    if (port_from_str(argv[1], &port) != 0) {
        printf("Invalid port\n");
        return -1;
    }

    struct chat_server *serv = chat_server_new();
    int rc = chat_server_listen(serv, port);
    if (rc != 0) {
        printf("Couldn't listen: %d\n", rc);
        chat_server_delete(serv);
        return -1;
    }

    struct pollfd poll_fds[2];
    memset(poll_fds, 0, sizeof(poll_fds));

    struct pollfd *poll_input = &poll_fds[0];
    poll_input->fd = STDIN_FILENO;
    poll_input->events = POLLIN;

    struct pollfd *poll_server = &poll_fds[1];
    poll_server->fd = chat_server_get_descriptor(serv);
    assert(poll_server->fd >= 0);

    const int buf_size = 1024;
    char buf[buf_size];

    while (true) {
        poll_server->events = chat_events_to_poll_events(chat_server_get_events(serv));

        int rc = poll(poll_fds, 2, -1);
        if (rc < 0) {
            printf("Poll error: %d\n", errno);
            break;
        }

        if (poll_input->revents != 0) {
            poll_input->revents = 0;
            rc = read(STDIN_FILENO, buf, buf_size - 1);
            if (rc == 0) {
                printf("EOF - exiting\n");
                break;
            }
            buf[rc] = '\0';
            rc = chat_server_feed(serv, buf, rc);
            if (rc != 0) {
                printf("Feed error: %d\n", rc);
                break;
            }
        }

        if (poll_server->revents != 0) {
            poll_server->revents = 0;
            rc = chat_server_update(serv, 0);
            if (rc != 0 && rc != CHAT_ERR_TIMEOUT) {
                printf("Update error: %d\n", rc);
                break;
            }
        }

        struct chat_message *msg;
        while ((msg = chat_server_pop_next(serv))) {
            if (!msg->is_server_message)
                printf("%s\n", msg->data);

            chat_message_delete(msg);
        }
    }

    chat_server_delete(serv);
    return 0;
}