#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "pointset.h"

pointset::pointset(const char* filename, bool write) : write(write) {
    if (write) {
        fd = open(filename, O_RDWR | O_CREAT, 0644);
        if (fd == -1) write = false;
    } 
    if (!write) {
        fd = open(filename, O_RDONLY);
    }
    if (fd == -1) {perror("Could not open file"); exit(1);}
    size = lseek(fd, 0, SEEK_END);
    assert(size % sizeof(point) == 0);
    length = size / sizeof(point);
    list = (point*)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (list == MAP_FAILED) {perror("Could not map file to memory"); exit(1);} 
}

pointset::~pointset() {
    if (list!=MAP_FAILED)
        munmap(list, size);
    if (fd!=-1)
        close(fd);
}

/**
 * Enables write access for the mapped memory region. 
 * This is used to avoid file corruption due to invalid memory access.
 * Note that writing to the memory while it does not has read permission 
 * will cause a segfault.
 */
void pointset::enable_write(bool flag) {
    if (write) {
        int ret = mprotect(list, size, PROT_READ | (write?PROT_WRITE:0));
        if (ret) {perror("Could not change read/write memory protection"); exit(1);}
    } else{
        fprintf(stderr, "Not opened in write mode");
    }
}

static const int point_buffer_size = 1<<16;
pointfile::pointfile(const char* filename) {
    fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd == -1) {perror("Could not open/create file"); exit(1);}
    buffer = new point[point_buffer_size];
    if (!buffer) {
        fprintf(stderr, "Could not allocate larger file buffer");
    }
    cnt = 0;
}

pointfile::~pointfile() {
    write(fd, buffer, cnt * sizeof(point));
    free(buffer);
    if (fd!=-1)
        close(fd);
}

void pointfile::add(const point& p) {
    buffer[cnt] = p;
    cnt++;
    if (cnt >= point_buffer_size) {
        write(fd, buffer, point_buffer_size * sizeof(point));
        cnt = 0;
    }
}
