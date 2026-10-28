#ifndef WORKLOAD_IO_H
#define WORKLOAD_IO_H

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define IO_OPERATIONS 2000
#define IO_BLOCK_SIZE 4096

static void io_task(int id)
{
char path[64];
char block[IO_BLOCK_SIZE];
int fd, i;

    snprintf(path, sizeof(path),
            "/tmp/io_worker_%d.dat", id);
    memset(block, 'x', sizeof(block));

    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC,
            0644);
    if (fd < 0)
        return;

    for (i = 0; i < IO_OPERATIONS; i++) {
        if (write(fd, block, sizeof(block)) !=
            sizeof(block))
            break;
        fsync(fd);
    }

close(fd);
unlink(path);
}

#endif