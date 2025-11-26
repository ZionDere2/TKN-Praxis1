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

static struct sockaddr_in derive_sockaddr(const char *host, const char *port) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
    };
    struct addrinfo *result_info;
    int returncode = getaddrinfo(host, port, &hints, &result_info);
    if (returncode) {
        fprintf(stderr, "Error parsing host/port\n");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in result = *((struct sockaddr_in *)result_info->ai_addr);
    freeaddrinfo(result_info);
    return result;
}

static int find_resource(const char *path) {
    for (int i = 0; i < MAX_RESOURCES; ++i) {
        if (resources[i].used && strcmp(resources[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int store_resource(const char *path, const unsigned char *data, size_t len) {
    int idx = find_resource(path);
    if (idx == -1) {
        for (int i = 0; i < MAX_RESOURCES; ++i) {
            if (!resources[i].used) {
                idx = i;
                break;
            }
        }
        if (idx == -1) {
            return -1;
        }
        resources[idx].used = true;
        strncpy(resources[idx].path, path, sizeof(resources[idx].path) - 1);
        resources[idx].path[sizeof(resources[idx].path) - 1] = '\0';
    } else {
        free(resources[idx].data);
    }

    resources[idx].data = (unsigned char *)malloc(len);
    if (!resources[idx].data && len > 0) {
        resources[idx].used = false;
        return -1;
    }
    memcpy(resources[idx].data, data, len);
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

static void send_response(int client_fd, int status, const char *status_text, const unsigned char *payload, size_t payload_len) {
    char header[512];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n\r\n", status, status_text, payload_len);
    if (n < 0) {
        return;
    }
    send(client_fd, header, (size_t)n, 0);
    if (payload_len > 0 && payload) {
        send(client_fd, payload, payload_len, 0);
    }
}

static bool parse_headers(const char *headers_start, const char *headers_end, size_t *content_length) {
    const char *line_start = headers_start;
    *content_length = 0;
    int header_count = 0;

    while (line_start < headers_end && !(line_start[0] == '\r' && line_start[1] == '\n')) {
        const char *line_end = strstr(line_start, "\r\n");
        if (!line_end || line_end > headers_end) {
            return false;
        }
        size_t line_len = (size_t)(line_end - line_start);
        if (line_len == 0 || line_len >= MAX_LINE_LENGTH) {
            return false;
        }
        char line[MAX_LINE_LENGTH];
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        char *colon = strchr(line, ':');
        if (!colon || colon == line) {
            return false;
        }
        if (*(colon + 1) != ' ') {
            return false;
        }
        *colon = '\0';
        const char *value = colon + 2;

        if (strcasecmp(line, "Content-Length") == 0) {
            char *endptr = NULL;
            long val = strtol(value, &endptr, 10);
            if (endptr == value || val < 0) {
                return false;
            }
            *content_length = (size_t)val;
        }

        header_count++;
        if (header_count > MAX_HEADERS) {
            return false;
        }
        line_start = line_end + 2;
    }

    return true;
}

static int parse_request(const unsigned char *buffer, size_t buf_len, struct http_request *req, size_t *consumed) {
    const unsigned char *header_end_ptr = NULL;
    for (size_t i = 0; i + 3 < buf_len; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            header_end_ptr = buffer + i + 4;
            break;
        }
    }
    if (!header_end_ptr) {
        return 0; // incomplete
    }

    size_t header_len = (size_t)(header_end_ptr - buffer);
    if (header_len >= MAX_REQUEST_SIZE) {
        *consumed = header_len;
        return -1;
    }

    const unsigned char *body_start = header_end_ptr;

    const unsigned char *line_end_ptr = (const unsigned char *)memmem(buffer, header_len, "\r\n", 2);
    if (!line_end_ptr) {
        *consumed = header_len;
        return -1;
    }
    size_t startline_len = (size_t)(line_end_ptr - buffer);
    if (startline_len == 0 || startline_len >= MAX_LINE_LENGTH) {
        *consumed = header_len;
        return -1;
    }

    char startline[MAX_LINE_LENGTH];
    memcpy(startline, buffer, startline_len);
    startline[startline_len] = '\0';
    if (sscanf(startline, "%255s %255s %255s", req->method, req->path, req->version) != 3) {
        *consumed = header_len;
        return -1;
    }

    if (strncmp(req->version, "HTTP/1.", 7) != 0) {
        *consumed = header_len;
        return -1;
    }

    const char *headers_start = (const char *)(line_end_ptr + 2);
    const char *headers_end = (const char *)(header_end_ptr - 2);

    size_t content_length = 0;
    if (!parse_headers(headers_start, headers_end, &content_length)) {
        *consumed = header_len;
        return -1;
    }

    size_t total_length = header_len + content_length;
    if (buf_len < total_length) {
        return 0; // wait for rest
    }

    req->content_length = content_length;
    req->body = (const char *)body_start;
    *consumed = total_length;
    return 1;
}

static bool path_has_prefix(const char *path, const char *prefix) {
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static void handle_request(int client_fd, const struct http_request *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (path_has_prefix(req->path, "/static/")) {
            const char *resource = req->path + strlen("/static/");
            const char *payload = NULL;
            if (strcmp(resource, "foo") == 0) {
                payload = "Foo";
            } else if (strcmp(resource, "bar") == 0) {
                payload = "Bar";
            } else if (strcmp(resource, "baz") == 0) {
                payload = "Baz";
            }
            if (payload) {
                send_response(client_fd, 200, "OK", (const unsigned char *)payload, strlen(payload));
                return;
            }
            send_response(client_fd, 404, "Not Found", NULL, 0);
            return;
        }

        if (path_has_prefix(req->path, "/dynamic/")) {
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
        if (!path_has_prefix(req->path, "/dynamic/")) {
            send_response(client_fd, 403, "Forbidden", NULL, 0);
            return;
        }
        int existing = find_resource(req->path);
        if (store_resource(req->path, (const unsigned char *)req->body, req->content_length) == -1) {
            send_response(client_fd, 500, "Internal Server Error", NULL, 0);
            return;
        }
        if (existing == -1) {
            send_response(client_fd, 201, "Created", NULL, 0);
        } else {
            send_response(client_fd, 204, "No Content", NULL, 0);
        }
        return;
    }

    if (strcmp(req->method, "DELETE") == 0) {
        if (!path_has_prefix(req->path, "/dynamic/")) {
            send_response(client_fd, 403, "Forbidden", NULL, 0);
            return;
        }
        int existing = find_resource(req->path);
        if (existing == -1) {
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
    size_t buf_len = 0;

    while (1) {
        ssize_t received = recv(client_fd, buffer + buf_len, sizeof(buffer) - buf_len, 0);
        if (received <= 0) {
            break;
        }
        buf_len += (size_t)received;

        size_t offset = 0;
        while (offset < buf_len) {
            struct http_request req;
            size_t consumed = 0;
            int parse_result = parse_request(buffer + offset, buf_len - offset, &req, &consumed);
            if (parse_result == 0) {
                break; // need more data
            } else if (parse_result < 0) {
                send_response(client_fd, 400, "Bad Request", NULL, 0);
                offset += consumed;
                continue;
            } else {
                handle_request(client_fd, &req);
                offset += consumed;
            }
        }

        if (offset > 0 && offset < buf_len) {
            memmove(buffer, buffer + offset, buf_len - offset);
        }
        if (offset > 0) {
            buf_len -= offset;
        }
    }
}

static int create_server_socket(const char *host, const char *port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = derive_sockaddr(host, port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
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
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
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
