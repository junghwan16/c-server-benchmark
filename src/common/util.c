#include "util.h"
#include <fcntl.h>
#include <errno.h>

int set_nonblock(int fd)
{
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}