#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#define SIZE (4UL * 1024 * 1024) // 16GB


int main(int argc, char *argv[])
{
    int fd = open("/dev/nvmev_device", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    void *mapped_mem = mmap(NULL, 4*SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_mem == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    if (argc > 1) {
        // 如果有命令行输入，则将输入的字符串写入映射的内存
        const char *input_str = argv[1];
        size_t input_len = strlen(input_str);

        if (input_len > SIZE) {
            fprintf(stderr, "Input string is too large!\n");
            munmap(mapped_mem, SIZE);
            close(fd);
            return -1;
        }

        // 将字符串写入内存
        memcpy(mapped_mem, input_str, input_len + 1); // 包括终止符
        printf("Written to memory: %s\n", input_str);
    } else {
        // 没有命令行输入，则读取内存中的字符串并打印
        printf("Read from memory: %s\n", (char *)mapped_mem);
        printf("Read from memory: %s\n", (char *)mapped_mem+SIZE);
        printf("Read from memory: %s\n", (char *)mapped_mem);
    }

    munmap(mapped_mem, 4*SIZE);
    close(fd);
    return 0;
}