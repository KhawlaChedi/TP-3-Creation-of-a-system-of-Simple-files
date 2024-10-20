#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "tosfs.h"

// Global variables
struct tosfs_superblock *superblock;
struct tosfs_inode *inode_cache;
char *mmap_buffer;

// Load the filesystem
int load_filesystem(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("Failed to open filesystem image");
        return -1;
    }

    superblock = mmap(NULL, sizeof(struct tosfs_superblock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (superblock == MAP_FAILED) {
        perror("Failed to map superblock");
        close(fd);
        return -1;
    }

    inode_cache = mmap(NULL, superblock->inodes * sizeof(struct tosfs_inode), PROT_READ | PROT_WRITE, MAP_SHARED, fd, TOSFS_INODE_BLOCK * TOSFS_BLOCK_SIZE);
    if (inode_cache == MAP_FAILED) {
        perror("Failed to map inodes");
        munmap(superblock, sizeof(struct tosfs_superblock));
        close(fd);
        return -1;
    }

    mmap_buffer = mmap(NULL, superblock->blocks * superblock->block_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, TOSFS_BLOCK_SIZE * TOSFS_BLOCK_SIZE);
    if (mmap_buffer == MAP_FAILED) {
        perror("Failed to map filesystem blocks");
        munmap(inode_cache, superblock->inodes * sizeof(struct tosfs_inode));
        munmap(superblock, sizeof(struct tosfs_superblock));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

// Function my_getattr
static void my_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct stat stbuf;
    memset(&stbuf, 0, sizeof(stbuf));

    if (ino == TOSFS_ROOT_INODE) { 
        stbuf.st_ino = TOSFS_ROOT_INODE;
        stbuf.st_mode = S_IFDIR | 0755;
        stbuf.st_nlink = 2;
    } else if (ino < superblock->inodes) {
        struct tosfs_inode *inode = &inode_cache[ino];
        stbuf.st_ino = ino;
        stbuf.st_mode = inode->mode;
        stbuf.st_nlink = inode->nlink;
        stbuf.st_size = inode->size;
    } else {
        fuse_reply_err(req, ENOENT);
        return;
    }

    fuse_reply_attr(req, &stbuf, 1.0);
}

// Function my_readdir
static void my_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    if (ino != TOSFS_ROOT_INODE) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    struct dirbuf {
        char *p;
        size_t size;
    };

    struct dirbuf b = { NULL, 0 };
    size_t oldsize;

  
    oldsize = b.size;
    b.size += fuse_add_direntry(req, NULL, 0, ".", NULL, 0);
    b.p = realloc(b.p, b.size);
    fuse_add_direntry(req, b.p + oldsize, b.size - oldsize, ".", NULL, 0);

    oldsize = b.size;
    b.size += fuse_add_direntry(req, NULL, 0, "..", NULL, 0);
    b.p = realloc(b.p, b.size);
    fuse_add_direntry(req, b.p + oldsize, b.size - oldsize, "..", NULL, 0);

    for (int i = 2; i < superblock->inodes; i++) {
        if (inode_cache[i].mode) { 
            char name[TOSFS_MAX_NAME_LENGTH];
            snprintf(name, TOSFS_MAX_NAME_LENGTH, "file%d", i);
            oldsize = b.size;
            b.size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
            b.p = realloc(b.p, b.size);
            fuse_add_direntry(req, b.p + oldsize, b.size - oldsize, name, NULL, 0);
        }
    }

    fuse_reply_buf(req, b.p, b.size);
    free(b.p);
}

// Function my_lookup
static void my_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    if (parent != TOSFS_ROOT_INODE) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    int inode_number = atoi(name + 4); 
    if (inode_number < 2 || inode_number >= superblock->inodes || !inode_cache[inode_number].mode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = inode_number;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    my_getattr(req, inode_number, NULL); 
    fuse_reply_entry(req, &e);
}

// Function my_read
static void my_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    if (ino < 2 || ino >= superblock->inodes || inode_cache[ino].mode == 0) {
        fuse_reply_err(req, ENOENT); 
        return;
    }

    struct tosfs_inode *inode = &inode_cache[ino];
    size_t len = inode->size; 

    if (off >= len) {
        fuse_reply_buf(req, NULL, 0); 
        return;
    }



    size_t to_read = (len - off < size) ? (len - off) : size;


    char *buffer = malloc(to_read);
    if (!buffer) {
        fuse_reply_err(req, ENOMEM); 
        return;
    }


    off_t block_offset = inode->block_no * TOSFS_BLOCK_SIZE; 
    memcpy(buffer, mmap_buffer + block_offset + off, to_read); 

    // Reply with the read content
    fuse_reply_buf(req, buffer, to_read);
    free(buffer); // Free the buffer after use
}

// Function my_create
static void my_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, struct fuse_file_info *fi) {
    if (parent != TOSFS_ROOT_INODE) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    for (int i = 2; i < superblock->inodes; i++) {
        if (inode_cache[i].mode == 0) { 
            inode_cache[i].mode = mode;
            inode_cache[i].size = 0;
            inode_cache[i].nlink = 1;
            
            fuse_reply_entry(req, &((struct fuse_entry_param){.ino = i}));
            return;
        }
    }

    fuse_reply_err(req, ENOSPC); // No space for new files
}


int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_chan *ch;
    int err = -1;


    if (fuse_parse_cmdline(&args, NULL, NULL, NULL) == -1) {
        fprintf(stderr, "Failed to parse command line\n");
        return EXIT_FAILURE;
    }


    if (load_filesystem(argv[1]) != 0) {
        return EXIT_FAILURE;
    }


    struct fuse_lowlevel_ops tosfs_oper = {
        .getattr = my_getattr,
        .readdir = my_readdir,
        .lookup = my_lookup,
        .read = my_read,
        .create = my_create,
    };


    ch = fuse_mount(argv[2], &args);
    if (ch == NULL) {
        perror("fuse_mount");
        return EXIT_FAILURE;
    }


    struct fuse_session *se = fuse_lowlevel_new(&args, &tosfs_oper, sizeof(tosfs_oper), NULL);
    if (se == NULL) {
        perror("fuse_lowlevel_new");
        fuse_unmount(argv[2], ch);
        return EXIT_FAILURE;
    }

    fuse_session_add_chan(se, ch);
    err = fuse_session_loop(se);


    fuse_session_remove_chan(ch);
    fuse_session_destroy(se);
    fuse_unmount(argv[2], ch);
    fuse_opt_free_args(&args);
    
    return err ? 1 : 0;
}

