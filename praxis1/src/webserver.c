#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQUEST_SIZE 8192
#define MAX_HEADERS 40
#define MAX_LINE_LENGTH 256
#define MAX_RESOURCES 100

struct http_request {
    char method[MAX_LINE_LENGTH];
    char path[MAX_LINE_LENGTH];
    char version[MAX_LINE_LENGTH];
    size_t content_length;
    const char *body;
};

struct resource_entry {
    char path[MAX_LINE_LENGTH];
    unsigned char *data;
    size_t len;
    bool used;
};

static struct resource_entry resources[MAX_RESOURCES];

static int find_resource(const char *path) {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (resources[i].used && strcmp(resources[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int store_resource(const char *path, const unsigned char *data, size_t len) {
    int idx = find_resource(path);
    if (idx == -1) {
        for (int i = 0; i < MAX_RESOURCES; i++) {
            if (!resources[i].used) {
                idx = i;
                resources[i].used = true;
                strncpy(resources[i].path, path, sizeof(resources[i].path) - 1);
                resources[i].path[sizeof(resources[i].path) - 1] = '\0';
                break;
            }
        }
    } else {
        free(resources[idx].data);
        resources[idx].data = NULL;
    }

    if (idx == -1) {
        return -1;
    }

    resources[idx].data = (unsigned char *)malloc(len);
    if (len > 0 && !resources[idx].data) {
        resources[idx].used = false;
        return -1;
    }

    if (len > 0) {
        memcpy(resources[idx].data, data, len);
    }
    resources[idx].len = len;
    return idx;
}

static void delete_resource(const char *path) {
    int idx = find_resource(path);
    if (idx != -1) {
        free(resources[idx].data);
        resources[idx].data = NULL;
        resources[idx].len = 0;
        resources[idx].used = false;
        resources[idx].path[0] = '\0';
    }
}

static void send_response(int client_fd, int status, const char *text, const unsigned char *payload, size_t payload_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n\r\n", status, text, payload_len);
    if (header_len < 0) {
        return;
    }

    send(client_fd, header, (size_t)header_len, 0);
    if (payload_len > 0 && payload) {
        send(client_fd, payload, payload_len, 0);
    }
}

static bool read_headers(const char *start, const char *end, size_t *content_length) {
    int header_count = 0;
    *content_length = 0;

    const char *cursor = start;
    while (cursor < end && !(cursor[0] == '\r' && cursor[1] == '\n')) {
        const char *line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) {
            return false;
        }

        size_t line_len = (size_t)(line_end - cursor);
        if (line_len == 0 || line_len >= MAX_LINE_LENGTH) {
            return false;
        }

        char temp[MAX_LINE_LENGTH];
        memcpy(temp, cursor, line_len);
        temp[line_len] = '\0';

        char *colon = strchr(temp, ':');
        if (!colon || colon == temp || colon[1] != ' ') {
            return false;
        }

        *colon = '\0';
        const char *value = colon + 2;
        if (strcasecmp(temp, "Content-Length") == 0) {
            char *tail = NULL;
            long parsed = strtol(value, &tail, 10);
            if (tail == value || parsed < 0) {
                return false;
            }
            *content_length = (size_t)parsed;
        }

        header_count++;
        if (header_count > MAX_HEADERS) {
            return false;
        }

        cursor = line_end + 2;
    }

    return true;
}

static int parse_request(const unsigned char *buf, size_t buf_len, struct http_request *req, size_t *consumed) {
    const unsigned char *body_start = NULL;
    for (size_t i = 0; i + 3 < buf_len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            body_start = buf + i + 4;
            break;
        }
    }

    if (!body_start) {
        return 0; // need more bytes
    }

    size_t header_len = (size_t)(body_start - buf);
    const unsigned char *line_end = NULL;
    for (size_t i = 0; i + 1 < header_len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            line_end = buf + i;
            break;
        }
    }

    if (!line_end) {
        *consumed = header_len;
        return -1;
    }

    size_t first_line_len = (size_t)(line_end - buf);
    if (first_line_len == 0 || first_line_len >= MAX_LINE_LENGTH) {
        *consumed = header_len;
        return -1;
    }

    char first_line[MAX_LINE_LENGTH];
    memcpy(first_line, buf, first_line_len);
    first_line[first_line_len] = '\0';

    if (sscanf(first_line, "%255s %255s %255s", req->method, req->path, req->version) != 3) {
        *consumed = header_len;
        return -1;
    }

    if (strncmp(req->version, "HTTP/1.", 7) != 0) {
        *consumed = header_len;
        return -1;
    }

    const char *headers_start = (const char *)(line_end + 2);
    const char *headers_end = (const char *)body_start - 2;

    size_t payload_len = 0;
    if (!read_headers(headers_start, headers_end, &payload_len)) {
        *consumed = header_len;
        return -1;
    }

    size_t total_needed = header_len + payload_len;
    if (buf_len < total_needed) {
        return 0; // body incomplete
    }

    req->content_length = payload_len;
    req->body = (const char *)(buf + header_len);
    *consumed = total_needed;
    return 1;
}

static bool path_starts_with(const char *path, const char *prefix) {
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static void handle_request(int client_fd, const struct http_request *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (path_starts_with(req->path, "/static/")) {
            const char *name = req->path + strlen("/static/");
            const char *payload = NULL;
            if (strcmp(name, "foo") == 0) payload = "Foo";
            else if (strcmp(name, "bar") == 0) payload = "Bar";
            else if (strcmp(name, "baz") == 0) payload = "Baz";

            if (payload) {
                send_response(client_fd, 200, "OK", (const unsigned char *)payload, strlen(payload));
            } else {
                send_response(client_fd, 404, "Not Found", NULL, 0);
            }
            return;
        }

        if (path_starts_with(req->path, "/dynamic/")) {
            int idx = find_resource(req->path);
            if (idx != -1) {
                send_response(client_fd, 200, "OK", resources[idx].data, resources[idx].len);
            } else {
                send_response(client_fd, 404, "Not Found", NULL, 0);
            }
            return;
        }

        send_response(client_fd, 404, "Not Found", NULL, 0);
        return;
    }

    if (strcmp(req->method, "PUT") == 0) {
        if (!path_starts_with(req->path, "/dynamic/")) {
            send_response(client_fd, 403, "Forbidden", NULL, 0);
            return;
        }

        int existed = find_resource(req->path);
        if (store_resource(req->path, (const unsigned char *)req->body, req->content_length) == -1) {
            send_response(client_fd, 500, "Internal Server Error", NULL, 0);
            return;
        }

        if (existed == -1) {
            send_response(client_fd, 201, "Created", NULL, 0);
        } else {
            send_response(client_fd, 204, "No Content", NULL, 0);
        }
        return;
    }

    if (strcmp(req->method, "DELETE") == 0) {
        if (!path_starts_with(req->path, "/dynamic/")) {
            send_response(client_fd, 403, "Forbidden", NULL, 0);
            return;
        }

        int idx = find_resource(req->path);
        if (idx == -1) {
            send_response(client_fd, 404, "Not Found", NULL, 0);
        } else {
            delete_resource(req->path);
            send_response(client_fd, 204, "No Content", NULL, 0);
        }
        return;
    }

    send_response(client_fd, 501, "Not Implemented", NULL, 0);
}

static void handle_connection(int client_fd) {
    unsigned char buffer[MAX_REQUEST_SIZE * 2];
    size_t buffered = 0;

    while (1) {
        ssize_t got = recv(client_fd, buffer + buffered, sizeof(buffer) - buffered, 0);
        if (got <= 0) {
            break;
        }
        buffered += (size_t)got;

        size_t consumed_total = 0;
        while (consumed_total < buffered) {
            struct http_request req;
            size_t consumed = 0;
            int state = parse_request(buffer + consumed_total, buffered - consumed_total, &req, &consumed);
            if (state == 0) {
                break;
            }
            if (state < 0) {
                send_response(client_fd, 400, "Bad Request", NULL, 0);
                consumed_total += consumed;
                continue;
            }

            handle_request(client_fd, &req);
            consumed_total += consumed;
        }

        if (consumed_total > 0 && consumed_total < buffered) {
            memmove(buffer, buffer + consumed_total, buffered - consumed_total);
        }
        if (consumed_total > 0) {
            buffered -= consumed_total;
        }
    }
}

static int create_server_socket(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (!host || host[0] == '\0') {
        hints.ai_flags = AI_PASSIVE;
    }

    struct addrinfo *info = NULL;
    int rc = getaddrinfo(host, port, &hints, &info);
    if (rc != 0) {
        fprintf(stderr, "Error parsing host/port: %s\n", gai_strerror(rc));
        exit(EXIT_FAILURE);
    }

    int server_fd = -1;
    for (int pass = 0; pass < 2 && server_fd < 0; pass++) {
        for (struct addrinfo *p = info; p != NULL; p = p->ai_next) {
            if (pass == 0 && p->ai_family != AF_INET) continue;
            if (pass == 1 && p->ai_family == AF_INET) continue;

            server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (server_fd < 0) {
                continue;
            }

            int opt = 1;
            setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            if (p->ai_family == AF_INET6) {
                int off = 0;
                setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
            }

            if (bind(server_fd, p->ai_addr, p->ai_addrlen) == 0 && listen(server_fd, 10) == 0) {
                break;
            }

            close(server_fd);
            server_fd = -1;
        }
    }

    freeaddrinfo(info);

    if (server_fd < 0) {
        perror("server setup");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int server_fd = create_server_socket(argv[1], argv[2]);

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }

        handle_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
