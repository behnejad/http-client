#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define FAIL_REVERSE            (-2)
#define HTTP_MAX_HEADERS        40
#define HTTP_MAX_BUFFER         128000


enum {
    HS_STATUS,
    HS_HEADERS,
    HS_PAYLOAD,
    HS_CHUNK_LEN,
    HS_CHUNK_VALUE,
    HS_CHUNK_END,
    HS_DONE,
    HS_ERROR,
};

typedef struct {
    char buffer[HTTP_MAX_BUFFER];
    int buffer_index;
    int buffer_size;
    char * last_line;
    char last_byte;
    int state;
    int content_length;
    int chunked_mode;
    int last_chunk_len;
    int response_code;
    int header_count;
    char * payload;
    struct {
        char * tag;
        char * value;
    } headers[HTTP_MAX_HEADERS];
} http_response_t;

void http_response_init(http_response_t * response)
{
    memset(response, 0, sizeof(http_response_t));
    response->state = HS_STATUS;
    response->last_line_index = response->buffer;
}

int http_read_int(const char * ptr)
{
    int res = 0;
    while (*ptr >= '0' && *ptr <= '9')
    {
        res *= 10;
        res += (*ptr - '0');
        ptr++;
    }

    return res;
}

int ascii_2_hex(char d)
{
    if (d >= '0' && d <= '9')
        return d - '0';
    else if (d >= 'a' && d <= 'f')
        return d - 'a' + 10;
    else if (d >= 'A' && d <= 'F')
        return d - 'A' + 10;
    else
        return -1;
}

const char * http_read_chunk_len(const char * ptr, int * out)
{
    *out = 0;
    int t;
    while ((t = ascii_2_hex(*ptr)) >= 0)
    {
        *out *= 16;
        *out += t;
        ptr++;
    }

    return ptr;
}

int http_parse(http_response_t * response)
{
    if (response->buffer_size > sizeof(response->buffer))
    {
        return -1;
    }

    if (response->state == HS_DONE)
    {
        return 0;
    }

    while (response->buffer_index < response->buffer_size)
    {
        const char * inja = response->buffer + response->buffer_index;
        char current_byte = response->buffer[response->buffer_index];
        int newline = (response->last_byte == '\r' && current_byte == '\n');

        if (response->state == HS_CHUNK_VALUE)
        {
            if (response->buffer_index - (response->last_line - response->buffer) == response->last_chunk_len)
            {
                response->state = HS_CHUNK_END;
            }
        }
        else if (response->state == HS_PAYLOAD)
        {
            if (response->content_length == 0 ||
                response->buffer_index - (response->payload - response->buffer) == response->content_length)
            {
                // wait until connection closes
                response->state = HS_DONE;
                return 0;
            }
        }
        else if (newline)
        {
            int update_last_line = 0;
            if (response->state == HS_CHUNK_LEN)
            {
                http_read_chunk_len(response->last_line, &response->last_chunk_len);
                response->state = HS_CHUNK_VALUE;
                update_last_line = 1;
            }
            else if (response->state == HS_CHUNK_END)
            {
                if (response->last_chunk_len == 0)
                {
                    char * ptr_before = response->payload;
                    const char * ptr_after = ptr_before;
                    const char * ptr_end = response->buffer + response->buffer_index;

                    while (ptr_after <= ptr_end)
                    {
                        int chunk_len;
                        ptr_after = http_read_chunk_len(ptr_after, &chunk_len) + 2;
                        memcpy(ptr_before, ptr_after, chunk_len);
                        ptr_before += chunk_len;
                        ptr_after += chunk_len + 2;
                    }

                    *ptr_before = 0;
                    response->content_length = ptr_before - response->payload;
                    response->state = HS_DONE;
                    return 0;
                }
                else
                {
                    response->state = HS_CHUNK_LEN;
                    update_last_line = 1;
                }
            }
            else if (response->buffer_index >= 4 && response->buffer[response->buffer_index - 2] == 0)
            {
                response->payload = response->buffer + response->buffer_index + 1;
                response->state = response->chunked_mode ? HS_CHUNK_LEN : HS_PAYLOAD;

                update_last_line = 1;
            }
            else if (response->state == HS_STATUS)
            {
                char * s = strchr(response->buffer, ' ');
                if (s == NULL)
                {
                    response->state = HS_ERROR;
                    return -1;
                }

                response->response_code = http_read_int(s + 1);
                response->state = HS_HEADERS;

                update_last_line = 1;
            }
            else if (response->state == HS_HEADERS)
            {
                if (response->header_count < HTTP_MAX_HEADERS)
                {
                    response->headers[response->header_count].tag = response->last_line;
                    char * s = strchr(response->last_line, ':');
                    if (s == NULL)
                    {
                        response->state = HS_ERROR;
                        return -1;
                    }

                    *s = 0;
                    s++;
                    if (*s != ' ')
                    {
                        response->state = HS_ERROR;
                        return -1;
                    }

                    s++;
                    response->headers[response->header_count].value = s;
                    if (strcasecmp(response->last_line, "Content-Length") == 0)
                    {
                        response->content_length = http_read_int(s);
                    }
                    else if (strcasecmp(response->last_line, "Transfer-Encoding") == 0 &&
                             memcmp(s, "chunked", 7) == 0)
                    {
                        response->chunked_mode = 1;
                    }

                    response->header_count++;
                }

                update_last_line = 1;
            }

            if (update_last_line)
            {
                response->buffer[response->buffer_index] = 0;
                response->buffer[response->buffer_index - 1] = 0;
                response->last_line = response->buffer + response->buffer_index + 1;
            }
        }

        response->last_byte = current_byte;
        response->buffer_index++;
    }

    return 1;
}

void http_print(http_response_t * hs)
{
    int j = 0;
    printf("state: %d\n", hs->state);
    printf("code: %d\n", hs->response_code);
    for (j = 0; j < hs->header_count; ++j)
        printf("%s -> %s\n", hs->headers[j].tag, hs->headers[j].value);
    printf("payload -> %s\n", hs->payload);
    printf("---------------\n");
}

static const struct httpRequestsStruct {
    const char * ip;
    const int port;
    const int timeout_connect;
    const int timeout_transfer;
    const char * hostName;
    const struct httpUrlStruct {
        const char * method;
        const char * url;
        const char * payload;
    } urls[];
} httpRequests = {
        "192.168.1.1",
        80,
        20,
        30,
        "www.example.com",
        {
                {"POST", "/jikjik", "{\"gholeidoon\": \"%s\"}"},
        }
};

int SocketCreate(const char * ip, int port, int timeout_s)
{
    struct timeval tv;
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        printf("cannot create socket\n");
        return -1;
    }

    long arg = fcntl(fd, F_GETFL, NULL);
    if (arg < 0)
    {
        printf("Error fcntl(..., F_GETFL) (%s)\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, arg | O_NONBLOCK) < 0)
    {
        printf("Error fcntl(..., F_SETFL) (%s)\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = inet_addr(ip);

    connect(fd, (struct sockaddr*) &address, sizeof(address));

    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);

    if (select(fd + 1, NULL, &fdset, NULL, &tv) == 1)
    {
        int so_error;
        socklen_t len = sizeof so_error;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

        if (so_error == 0)
        {
            arg = fcntl(fd, F_GETFL, NULL);
            if (arg < 0)
            {
                fprintf(stderr, "Error fcntl(..., F_GETFL) (%s)\n", strerror(errno));
                close(fd);
                return -1;
            }

            if (fcntl(fd, F_SETFL, arg & (~O_NONBLOCK)) < 0) {
                fprintf(stderr, "Error fcntl(..., F_SETFL) (%s)\n", strerror(errno));
                close(fd);
                return -1;
            }

            printf("socket connected\n");
            return fd;
        }
    }

    printf("select ended\n");

    close(fd);
    return -1;
}

int SocketSendHttp(int request, http_response_t * resp, ...)
{
    int res = -1;
    char payload[HTTP_MAX_BUFFER];
    char body[HTTP_MAX_BUFFER];

    int socket = SocketCreate(httpRequests.ip, httpRequests.port, httpRequests.timeout_connect);
    if (socket < 0)
    {
        return -1;
    }

    const struct httpUrlStruct * req = &httpRequests.urls[request];

    va_list ap;
    va_start(ap, request);
    int body_len = vsnprintf(body, sizeof(body), req->payload, ap);
    va_end(ap);

    int payload_len = sprintf(payload,
                      "%s %s HTTP/1.0\r\nHost: %s\r\nAccept: application/json\r\n"
                      "Content-Length: %d\r\nContent-Type: application/json\r\n\r\n%s",
                      req->method, req->url, httpRequests.hostName, body_len, body);

    if (send(socket, payload, payload_len, 0) < 0)
    {
        close(socket);
        return -1;
    }

    http_response_init(resp);

    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(socket, &fdset);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    time_t start_time = time(NULL);
    while (1)
    {
        if (resp->state == HS_DONE)
        {
            http_print(resp);
            break;
        }

        if (time(NULL) - start_time >= httpRequests.timeout_transfer)
        {
            return FAIL_REVERSE;
        }

        res = select(socket + 1, &fdset, NULL, NULL, &tv);
        if (res < 0)
        {
            return FAIL_REVERSE;
        }
        else if (res == 1)
        {
            int len = recv(socket, resp->buffer + resp->buffer_index, HTTP_MAX_BUFFER - resp->buffer_index, 0);
            if (len < 0)
            {
                return FAIL_REVERSE;
            }
            else if (len > 0)
            {
                resp->buffer_size += len;
                http_parse(resp);
            }
        }
    }

    close(socket);
    return 0;
}

const char * data[] = {
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 12\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Hello World.",

        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n",

        "HTTP/1.0 400 Bad Request\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Length: 13\r\n"
        "Date: Mon, 25 Sep 2023 06:10:49 GMT\r\n"
        "\r\n"
        "<html></html>",

        "HTTP/1.0 400 Bad Request\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Date: Mon, 25 Sep 2023 06:10:49 GMT\r\n"
        "\r\n"
        "6\r\n12345-\r\n"
        "5\r\nsalam\r\n"
        "1\r\n\n\r\n"
        "11\r\npashmak gholikhan\r\n"
        "000\r\n\r\n",

        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Date: Mon, 25 Sep 2023 06:10:49 GMT\r\n"
        "\r\n"
        "10\r\n[[\r\nxx1>0\r\n\r\n<]]\r\n"
        "5\r\nsalam\r\n"
        "1\r\n\n\r\n"
        "11\r\npashmak gholikhan\r\n"
        "000\r\n\r\n",
};

//#include "cJSON.h"
//cJSON * cJSON_FindObject(cJSON * root, const char * tag)
//{
//    cJSON * elem;
//    if (root == NULL)
//        return NULL;
//    else if (root->type == cJSON_Array) {
//        int i = 0;
//        for (i = 0; i < cJSON_GetArraySize(root); ++i) {
//            elem = cJSON_GetArrayItem(root, i);
//            if (elem) {
//                elem = cJSON_FindObject(elem, tag);
//                if (elem)
//                    return elem;
//            } else
//                return NULL;
//        }
//    }
//    else if (root->type == cJSON_Object) {
//        elem = cJSON_GetObjectItem(root, tag);
//        if (elem)
//            return elem;
//        elem = root->child;
//        while (elem) {
//            if (elem->type == cJSON_Array || elem->type == cJSON_Object) {
//                cJSON * temp = cJSON_FindObject(elem, tag);
//                if (temp)
//                    return temp;
//            }
//            elem = elem->next;
//        }
//    }
//    return NULL;
//}

int main1() {
    http_response_t hs;
    int i = 0, j = 0;
    for (i = 0; i < sizeof(data)/ sizeof(data[0]); ++i)
    {
        http_response_init(&hs);

        hs.buffer_size = strlen(data[i]);
        strcpy(hs.buffer, data[i]);
        http_parse(&hs);

        http_print(&hs);
    }
    return 0;
}

int main()
{
    http_response_t response;
    SocketSendHttp(0, &response, "23");
    return 0;
}
