#include <stddef.h>
#include "thread_srv/thread_server.h"

int main()
{
  return run_thread_server(NULL, 8080, "./www");
}