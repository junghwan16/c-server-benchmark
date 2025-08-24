#include <stddef.h>
#include "aio_srv/aio_server.h"

int main()
{
  return run_aio_server(NULL, 8080, "./www");
}
