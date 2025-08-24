#include <stddef.h>
#include "kqueue_srv/kqueue_server.h"

int main()
{
    return run_kqueue_server(NULL, 8080, "./www");
}