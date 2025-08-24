#pragma once
#include <stddef.h>
#include <sys/types.h>

typedef struct
{
    char method[8];
    char path[1024];
    int complete; // 헤더 파싱 완료 여부
} http_req_t;

int http_parse_request(char *buf, size_t len, http_req_t *out); // 완료=1, 더필요=0, 에러<0
const char *http_guess_type(const char *path);
int http_build_200(char *dst, size_t cap, long long content_len, const char *ctype);
int http_build_404(char *dst, size_t cap);

int http_safe_join(char *out, size_t outsz, const char *root, const char *rel);