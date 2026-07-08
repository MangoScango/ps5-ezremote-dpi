#undef main

#include <string>
#include <vector>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <ps5/klog.h>

#include "sceAppInstUtil.h"

using namespace std;

#define BUF_SIZE 4096
#define PORT 9040

typedef struct notify_request
{
    char useless1[45];
    char message[3075];
} notify_request_t;

extern "C"
{
    int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
}

void notify(const char *fmt, ...)
{
    notify_request_t req;
    va_list args;

    bzero(&req, sizeof req);
    va_start(args, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, args);
    va_end(args);

    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(const char *start, const char *end, char *out, size_t out_size)
{
    size_t oi = 0;
    for (const char *p = start; p < end && oi + 1 < out_size; p++)
    {
        if (*p == '+')
        {
            out[oi++] = ' ';
        }
        else if (*p == '%' && (p + 2) < end &&
                 isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))
        {
            out[oi++] = (char)((hex_val(p[1]) << 4) | hex_val(p[2]));
            p += 2;
        }
        else
        {
            out[oi++] = *p;
        }
    }
    out[oi] = 0;
}

static bool get_query_param(const char *query, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *query_end = query + strlen(query);
    out[0] = 0;

    for (const char *p = query; p != nullptr && *p != 0; )
    {
        const char *amp = strchr(p, '&');
        const char *tok_end = (amp != nullptr) ? amp : query_end;
        const char *eq = (const char *)memchr(p, '=', (size_t)(tok_end - p));
        if (eq != nullptr)
        {
            size_t this_key_len = (size_t)(eq - p);
            if (this_key_len == key_len && strncmp(p, key, key_len) == 0)
            {
                url_decode(eq + 1, tok_end, out, out_size);
                return true;
            }
        }
        if (amp == nullptr) break;
        p = amp + 1;
    }
    return false;
}

// Build an absolute URL from a base URL's scheme+host and a (possibly relative)
// path. If rel is already absolute (http/https), it is copied through.
static void build_absolute_url(const char *base, const char *rel, char *out, size_t out_size)
{
    if (strncasecmp(rel, "http://", 7) == 0 || strncasecmp(rel, "https://", 8) == 0)
    {
        strncpy(out, rel, out_size - 1);
        out[out_size - 1] = 0;
        return;
    }

    const char *scheme_end = strstr(base, "://");
    if (scheme_end == nullptr)
    {
        strncpy(out, rel, out_size - 1);
        out[out_size - 1] = 0;
        return;
    }

    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');
    size_t prefix_len = (path_start != nullptr) ? (size_t)(path_start - base) : strlen(base);
    if (prefix_len >= out_size) prefix_len = out_size - 1;

    memcpy(out, base, prefix_len);
    out[prefix_len] = 0;
    strncat(out, rel, out_size - strlen(out) - 1);
}

// Creates a listening TCP socket on the given port. Returns the fd, or -1 on
// failure. SO_REUSEADDR lets us rebind immediately after a rest-mode resume,
// when the previous socket may still be lingering.
static int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(fd, (const sockaddr *)&address, (socklen_t)sizeof(address)) < 0)
    {
        close(fd);
        return -1;
    }
    if (listen(fd, 3) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    int ret;
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[BUF_SIZE] = {0};
    char *pos;
    PlayGoInfo playgo_info;
    SceAppInstallPkgInfo pkg_info;
    MetaInfo metainfo;

    syscall(SYS_thr_set_name, -1, "ezremote-dpi.elf");

    // Create the listening socket.
    server_fd = create_listen_socket(PORT);
    if (server_fd < 0)
    {
        notify("ezRemote DPI could not listen on port %d", PORT);
        return 0;
    }

    notify("ezRemote DPI listening on port %d", PORT);

    // Accepting incoming connections and handling requests
    while (true)
    {
        // After a rest-mode resume the network stack is torn down and the
        // listening socket goes stale, so accept() fails permanently. Rebuild
        // the listener instead of spinning on a dead fd.
        if (server_fd < 0)
        {
            sleep(1);
            server_fd = create_listen_socket(PORT);
            if (server_fd < 0)
                continue;
            klog_printf("ezRemote DPI re-listening on port %d\n", PORT);
        }

        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        if (new_socket < 0)
        {
            close(server_fd);
            server_fd = -1;
            continue;
        }

        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const void *>(&tv), sizeof(tv));
        int yes = 1;
		setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, (char *) &yes, sizeof(int));
        // Don't die with SIGPIPE if the client closed the connection before we
        // send() the result code back (matches elfldr's SO_NOSIGPIPE).
        setsockopt(new_socket, SOL_SOCKET, SO_NOSIGPIPE, (char *) &yes, sizeof(int));

        // Reading the request from the client
        memset(buffer, 0, sizeof(buffer));
        ret = -1;
        int valread = recv(new_socket, buffer, BUF_SIZE, 0);            
        if (valread > 0)
        {
            pos = strchr(buffer, '\r');
            if (pos != nullptr)
            {
                *pos = 0;
            }
            pos = strchr(buffer, '\n');
            if (pos != nullptr)
            {
                *pos = 0;
            }

            if (strncmp(buffer, "stop", 4) == 0)
            {
                close(new_socket);
                break;
            }
            klog_printf("ezRemote DPI Received: %s\n", buffer);

            memset(&playgo_info, 0, sizeof(playgo_info));

            for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++)
            {
                strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
            }
        
            for (size_t i = 0; i < SCE_NUM_IDS; i++)
            {
                strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
                strncpy(*playgo_info.content_ids, "", sizeof(content_id_t) - 1);
            }
        
            static char url_base[BUF_SIZE];
            static char content_id_buf[CONTENTID_SIZE];
            static char content_name[256];
            static char icon_url_buf[BUF_SIZE];
            content_id_buf[0] = 0;
            content_name[0] = 0;
            icon_url_buf[0] = 0;

            strncpy(url_base, buffer, sizeof(url_base) - 1);
            url_base[sizeof(url_base) - 1] = 0;

            char *query = strchr(url_base, '?');
            if (query != nullptr)
            {
                *query = 0;   // terminate the base URL at '?'
                query++;      // parameters begin here

                get_query_param(query, "content_id", content_id_buf, sizeof(content_id_buf));
                get_query_param(query, "name", content_name, sizeof(content_name));

                char icon_rel[BUF_SIZE];
                icon_rel[0] = 0;
                if (get_query_param(query, "icon", icon_rel, sizeof(icon_rel)) && icon_rel[0] != 0)
                {
                    build_absolute_url(url_base, icon_rel, icon_url_buf, sizeof(icon_url_buf));
                }
            }

            metainfo.uri = url_base;
            metainfo.ex_uri = "";
            metainfo.playgo_scenario_id = "";
            metainfo.content_id = content_id_buf;

            // Fall back to deriving the name from the URL if no name parameter
            // was provided. The installer rejects overly long content names, so
            // prefer the .pkg filename and otherwise cap at 255 chars.
            if (content_name[0] == 0)
            {
                size_t url_len = strlen(url_base);
                if (url_len >= 4 && strcasecmp(url_base + url_len - 4, ".pkg") == 0)
                {
                    char *slash = strrchr(url_base, '/');
                    const char *src = (slash != nullptr) ? slash + 1 : url_base;
                    strncpy(content_name, src, sizeof(content_name) - 1);
                }
                else
                {
                    strncpy(content_name, url_base, sizeof(content_name) - 1);
                }
                content_name[sizeof(content_name) - 1] = 0;
            }
            metainfo.content_name = content_name;
            metainfo.icon_url = icon_url_buf;

            klog_printf("ezRemote DPI Installing: Name: %s, Content ID: %s\n",
                        content_name, content_id_buf[0] ? content_id_buf : "(none)");

            ret = sceAppInstUtilInstallByPackage(&metainfo, &pkg_info, &playgo_info);
            if (ret != 0)
            {
                notify("Package install failed with\nError Code: 0x%08X\n", ret);
            }        
        }

        // Sending the response to the client
        sprintf(buffer, "%d", ret);
        send(new_socket, buffer, strlen(buffer), 0);

        // Closing the connection
        close(new_socket);
    }

    // Closing the listening socket (this part will not be reached in the current loop implementation)
    klog_printf("Closing ezRemote DPI\n");
    close(server_fd);

    return 0;
}
