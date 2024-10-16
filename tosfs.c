#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "tosfs.h"

void print_filesystem_info(struct tosfs_superblock *superblock) {
    printf("Filesystem Information:\n");
    printf("Magic Number: 0x%x\n", superblock->magic);
    printf("Block Size: %u bytes\n", superblock->block_size);
    printf("Total Blocks: %u\n", superblock->blocks);
    printf("Total Inodes: %u\n", superblock->inodes);
    printf("Root Inode: %u\n", superblock->root_inode);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filesystem_image>\n", argv[0]);
        return EXIT_FAILURE;
    }


    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("Failed to open filesystem image");
        return EXIT_FAILURE;
    }


    struct tosfs_superblock *superblock = mmap(NULL, TOSFS_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, TOSFS_SUPERBLOCK * TOSFS_BLOCK_SIZE);
    if (superblock == MAP_FAILED) {
        perror("Failed to map superblock");
        close(fd);
        return EXIT_FAILURE;
    }


    if (superblock->magic != TOSFS_MAGIC) {
        fprintf(stderr, "Invalid filesystem: wrong magic number\n");
        munmap(superblock, TOSFS_BLOCK_SIZE);
        close(fd);
        return EXIT_FAILURE;
    }


    print_filesystem_info(superblock);


    munmap(superblock, TOSFS_BLOCK_SIZE);
    close(fd);
    return EXIT_SUCCESS;
}

