#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum 
{
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block 
{
    /** Block memory. */
    char *memory;
    /** How many bytes are occupied. */
    int occupied;
    /** Next block in the file. */
    struct block *next;
    /** Previous block in the file. */
    struct block *prev;

    /* PUT HERE OTHER MEMBERS */
};

struct file 
{
    /** Double-linked list of file blocks. */
    struct block *block_list;
    /**
     * Last block in the list above for fast access to the end
     * of file.
     */
    struct block *last_block;
    /** How many file descriptors are opened on the file. */
    int refs;
    /** File name. */
    char *name;
    /** Files are stored in a double-linked list. */
    struct file *next;
    struct file *prev;

    /* PUT HERE OTHER MEMBERS */

    /** Flag to show if the file is deleted. */
    int deleted;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc 
{
    struct file *file;

    /* PUT HERE OTHER MEMBERS */

    /** Current block number. */
    int block_number;
    /** Current offset in the block. */
    int block_offset;
    /** Mode in which the file was opened. */
    enum open_flags flags;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

static struct file*
find_file(const char *name) 
{
    for (struct file *f = file_list; f; f = f->next) {
        if (strcmp(f->name, name) == 0 && !f->deleted)
            return f;
    }

    return NULL;
}

static int 
is_writable(const struct filedesc *desc) 
{
    return desc->flags == 0 || desc->flags & (UFS_CREATE | UFS_WRITE_ONLY | UFS_READ_WRITE);
}

static int 
is_readable(const struct filedesc *desc) 
{
    return desc->flags == 0 || desc->flags & (UFS_CREATE | UFS_READ_ONLY | UFS_READ_WRITE);
}

static int 
validate_desc(const struct filedesc *desc, int (*prog) (const struct filedesc*)) 
{
    if (desc == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    
    if (!prog(desc)) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }

    return 0;
}

static int 
validate_empty_file(const struct block* block)
{
    if (!block) {
        ufs_error_code = UFS_ERR_NO_ERR;
        return 0;
    }

    return -2;
}

static struct filedesc* 
ufs_find_filedesc(const int file_desc) 
{
    if (file_desc < 0 || file_desc >= file_descriptor_count || !file_descriptors[file_desc])
        return NULL;
    
    return file_descriptors[file_desc];
}

static enum ufs_error_code 
ufs_add_block(struct file *file) 
{
    struct block *new_block = malloc(sizeof(struct block));
    if (!new_block)
        return UFS_ERR_NO_MEM;

    new_block->memory = malloc(BLOCK_SIZE);
    if (!new_block->memory) {
        free(new_block);
        return UFS_ERR_NO_MEM;
    }

    new_block->occupied = 0;
    new_block->next = NULL;
    new_block->prev = file->last_block;
    
    if (file->last_block)
        file->last_block->next = new_block;
    else
        file->block_list = new_block;
    file->last_block = new_block;

    return UFS_ERR_NO_ERR;
}

static void 
free_file(struct file *file) 
{
    struct block *blk = file->block_list;

    while (blk) {
        struct block *next_blk = blk->next;
        free(blk->memory);
        free(blk);
        blk = next_blk;
    }

    if (file->prev)
        file->prev->next = file->next;
    else
        file_list = file->next;
    if (file->next)
        file->next->prev = file->prev;

    free(file->name);
    free(file);
}

enum ufs_error_code
ufs_errno() 
{
    return ufs_error_code;
}

int 
ufs_open(const char *filename, int flags) 
{
    if (!filename) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct file *f = find_file(filename);

    if (!f && (flags & UFS_CREATE)) {
        f = malloc(sizeof(struct file));
        if (!f) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

        f->name = strdup(filename);
        if (!f->name) {
            free(f);
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

        f->block_list = f->last_block = NULL;
        f->refs = 0;
        f->deleted = 0;
        f->next = file_list;
        f->prev = NULL;
        if (file_list)
            file_list->prev = f;
	    
        file_list = f;
    }
    
    if (!f) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (f->deleted && (flags & UFS_CREATE)) {
        f->deleted = 0;
        f->block_list = f->last_block = NULL;
        f->refs = 0;
    }

    if (file_descriptor_count == file_descriptor_capacity) {
        int new_capacity = file_descriptor_capacity ? file_descriptor_capacity * 2 : 10;
        struct filedesc **new_descriptors = realloc(file_descriptors, new_capacity * sizeof(struct filedesc *));
        if (!new_descriptors) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }
        
        file_descriptors = new_descriptors;
        file_descriptor_capacity = new_capacity;
    }

    struct filedesc *file_desc = malloc(sizeof(struct filedesc));
    if (!file_desc) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    file_desc->file = f;
    file_desc->block_number = 0;
    file_desc->block_offset = 0;
    file_desc->flags = flags & (UFS_READ_ONLY | UFS_WRITE_ONLY | UFS_READ_WRITE);

    file_descriptors[file_descriptor_count] = file_desc;
    f->refs++;

    return file_descriptor_count++;
}

ssize_t 
ufs_write(int file_desc, const char *buf, size_t size) 
{
    struct filedesc *desc = ufs_find_filedesc(file_desc);
    
    if (validate_desc(desc, is_writable) != 0)
        return -1;
		
    struct file *f = desc->file;
    struct block *file_block = f->block_list;

    if (!file_block) {
        ufs_error_code = ufs_add_block(f);
        if (ufs_error_code != UFS_ERR_NO_ERR)
            return -1;
        
        file_block = f->block_list;
        desc->block_number = 0;
        desc->block_offset = 0;
    }

    for (int i = 0; i < desc->block_number; ++i)
        file_block = file_block->next;

    size_t byte_size = file_block->occupied + desc->block_number * BLOCK_SIZE;
    if (byte_size + size > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    ssize_t total_written = 0;
    const char *src = buf;

    while (size) {
        if (desc->block_offset >= BLOCK_SIZE) {
            if (!file_block->next) {
                if (ufs_add_block(f) != UFS_ERR_NO_ERR) 
                    break;
            }
            file_block = file_block->next;
            desc->block_number++;
            desc->block_offset = 0;
        }

        size_t space_left = BLOCK_SIZE - desc->block_offset;
        size_t write_data_size = size < space_left ? size : space_left;

        memcpy(file_block->memory + desc->block_offset, src, write_data_size);

        desc->block_offset += write_data_size;
        total_written += write_data_size;
        src += write_data_size;
        size -= write_data_size;

        file_block->occupied = (desc->block_offset > file_block->occupied) ? desc->block_offset : file_block->occupied;
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return total_written;
}

ssize_t 
ufs_read(int file_desc, char *buf, size_t size) 
{
    struct filedesc *desc = ufs_find_filedesc(file_desc);

    if (validate_desc(desc, is_readable) != 0)
        return -1;

    struct block *file_block = desc->file->block_list;
    if (validate_empty_file(file_block) == 0) 
        return 0;

    for (int i = 0; i < desc->block_number; ++i) {
        file_block = file_block->next;
        if (validate_empty_file(file_block) == 0) 
            return 0;
    }

    ssize_t total_read = 0;
    char *dest = buf;

    while (size) {
        if (desc->block_offset >= file_block->occupied) {
            file_block = file_block->next;
            
            if (!file_block) 
                break;
            
            desc->block_number++;
            desc->block_offset = 0;
        }

        size_t available_space = file_block->occupied - desc->block_offset;
        size_t read_data_size = size < available_space ? size : available_space;

        memcpy(dest, file_block->memory + desc->block_offset, read_data_size);

        desc->block_offset += read_data_size;
        total_read += read_data_size;
        dest += read_data_size;
        size -= read_data_size;
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return total_read;
}

int 
ufs_close(int file_desc) 
{
    struct filedesc *desc = ufs_find_filedesc(file_desc);
    if (!desc) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct file *f = desc->file;
    f->refs--;

    free(desc);
    file_descriptors[file_desc] = NULL;

    if (f->refs == 0 && f->deleted)
        free_file(f);

    return 0;
}

int 
ufs_delete(const char *filename)
{
    if (!filename) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct file *f = find_file(filename);
    if (!f) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    f->deleted = 1;
    if (f->refs == 0)
        free_file(f);
    
    return 0;
}

void 
ufs_destroy(void) 
{
    while (file_list) {
        struct file *next_file = file_list->next;
        free_file(next_file);
    }

    if (file_descriptors) {
        for (int i = 0; i < file_descriptor_count; i++) {
            if (file_descriptors[i]) {
                free(file_descriptors[i]);
                file_descriptors[i] = NULL;
            }
        }

        free(file_descriptors);
        file_descriptors = NULL;
    }

    file_descriptor_capacity = 0;
    file_descriptor_count = 0;
}
