#include "http.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>

int http_parse_request(char *buf, size_t len, http_req_t *out)
{
    if (!buf || !out || len == 0) {
        return -1;
    }
    
    memset(out, 0, sizeof(*out));
    
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) {
        return 0;
    }
    
    out->complete = 1;
    
    if (len < 14 || strncmp(buf, "GET ", 4) != 0) {
        return -1;
    }
    
    strncpy(out->method, "GET", sizeof(out->method) - 1);
    out->method[sizeof(out->method) - 1] = '\0';
    
    char *path_start = buf + 4;
    /* Skip spaces with bounds checking */
    while (path_start < header_end && *path_start == ' ') {
        path_start++;
    }
    
    if (path_start >= header_end) {
        return -1;
    }
    
    char *path_end = strchr(path_start, ' ');
    if (!path_end || path_end > header_end) {
        return -1;
    }
    
    size_t path_len = path_end - path_start;
    if (path_len == 0 || path_len >= sizeof(out->path)) {
        return -1;
    }
    
    memcpy(out->path, path_start, path_len);
    out->path[path_len] = '\0';
    
    if (strcmp(out->path, "/") == 0) {
        strcpy(out->path, "/index.html");
    }
    
    return 1;
}

const char *http_guess_type(const char *p)
{
    size_t n = strlen(p);
    if (n >= 5 && !strcasecmp(p + n - 5, ".html"))
        return "text/html";
    if (n >= 4 && !strcasecmp(p + n - 4, ".css"))
        return "text/css";
    if (n >= 3 && !strcasecmp(p + n - 3, ".js"))
        return "application/javascript";
    if (n >= 4 && !strcasecmp(p + n - 4, ".png"))
        return "image/png";
    if (n >= 4 && !strcasecmp(p + n - 4, ".jpg"))
        return "image/jpeg";
    if (n >= 4 && !strcasecmp(p + n - 4, ".gif"))
        return "image/gif";
    return "application/octet-stream";
}

int http_build_200(char *dst, size_t cap, long long content_len, const char *ctype)
{
    if (!dst || !ctype || cap == 0) {
        return -1;
    }
    
    int n = snprintf(dst, cap,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Length: %lld\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n\r\n",
                     content_len, ctype);
    
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int http_build_404(char *dst, size_t cap)
{
    if (!dst || cap == 0) {
        return -1;
    }
    
    const char *body = "Not Found";
    int n = snprintf(dst, cap,
                     "HTTP/1.1 404 Not Found\r\n"
                     "Content-Length: %zu\r\n"
                     "Content-Type: text/plain\r\n"
                     "Connection: close\r\n\r\n%s",
                     strlen(body), body);
    
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int http_safe_join(char *out, size_t outsz, const char *root, const char *rel)
{
    if (!out || !root || !rel || outsz == 0) {
        return -1;
    }
    
    char resolved[PATH_MAX];
    char temp_path[PATH_MAX];
    
    const char *clean_rel = rel;
    if (clean_rel[0] == '/') {
        clean_rel++;
    }
    
    if (strlen(clean_rel) == 0) {
        clean_rel = "index.html";
    }
    
    int n = snprintf(temp_path, sizeof(temp_path), "%s/%s", root, clean_rel);
    if (n < 0 || (size_t)n >= sizeof(temp_path)) {
        return -1;
    }
    
    if (!realpath(temp_path, resolved)) {
        n = snprintf(temp_path, sizeof(temp_path), "%s", root);
        if (n < 0 || (size_t)n >= sizeof(temp_path)) {
            return -1;
        }
        
        if (!realpath(temp_path, resolved)) {
            return -1;
        }
        
        n = snprintf(out, outsz, "%s/%s", resolved, clean_rel);
        if (n < 0 || (size_t)n >= outsz) {
            return -1;
        }
        return 0;
    }
    
    char root_real[PATH_MAX];
    if (!realpath(root, root_real)) {
        return -1;
    }
    
    if (strncmp(resolved, root_real, strlen(root_real)) != 0) {
        return -1;
    }
    
    size_t len = strlen(resolved);
    if (len >= outsz) {
        return -1;
    }
    
    strcpy(out, resolved);
    return 0;
}