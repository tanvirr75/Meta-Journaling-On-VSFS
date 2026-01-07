#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>

// --- Configuration ---
#define BLOCK_SIZE 4096
#define DEFAULT_IMAGE "vsfs.img"
#define JOURNAL_MAGIC 0x4A524E4C

// --- File System Structures (Matches mkfs.c) ---

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;  // 1=file, 2=dir
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  _pad[128 - (2 + 2 + 4 + 8 * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};

// --- Journal Structures ---

#define REC_DATA 1
#define REC_COMMIT 2

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t  data[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

// --- Helper Functions ---

void fail(char *msg) {
    perror(msg);
    exit(1);
}

// Wrapper to grab a block safely
void read_block(int fd, uint32_t index, void *buf) {
    off_t offset = (off_t)index * BLOCK_SIZE;
    if (pread(fd, buf, BLOCK_SIZE, offset) != BLOCK_SIZE) {
        fail("Error reading block");
    }
}

// Wrapper to write a block safely
void write_block(int fd, uint32_t index, void *buf) {
    off_t offset = (off_t)index * BLOCK_SIZE;
    if (pwrite(fd, buf, BLOCK_SIZE, offset) != BLOCK_SIZE) {
        fail("Error writing block");
    }
}

// Bitmap tools
int check_bit(uint8_t *map, int i) {
    return (map[i / 8] >> (i % 8)) & 1;
}

void mark_bit(uint8_t *map, int i) {
    map[i / 8] |= (1 << (i % 8));
}

// --- Main Logic ---

void handle_create(int fd, char *filename) {
    struct superblock sb;
    read_block(fd, 0, &sb);

    // 1. Find a free spot in the Inode Bitmap
    uint8_t imap[BLOCK_SIZE];
    read_block(fd, sb.inode_bitmap, imap);

    int free_ino = -1;
    for (int i = 1; i < sb.inode_count; i++) {
        if (check_bit(imap, i) == 0) {
            free_ino = i;
            break;
        }
    }
    
    if (free_ino < 0) {
        printf("Error: No free inodes available.\n");
        exit(1);
    }

    // Mark the inode as taken
    mark_bit(imap, free_ino);

    // 2. Prepare the Inode Table Block
    int inodes_per = BLOCK_SIZE / sizeof(struct inode);
    uint32_t blk_idx = sb.inode_start + (free_ino / inodes_per);
    int offset = free_ino % inodes_per;

    uint8_t itable_buf[BLOCK_SIZE];
    read_block(fd, blk_idx, itable_buf);
    
    struct inode *table = (struct inode *)itable_buf;

    // Set up the new file inode
    memset(&table[offset], 0, sizeof(struct inode));
    table[offset].type = 1; // File type
    table[offset].links = 1;

    // IMPORTANT: Update Root Directory size (Inode 0)
    // Root is always at index 0. Since we are adding an entry, it grows.
    table[0].size += sizeof(struct dirent);

    // 3. Update the Root Directory Data Block
    uint32_t root_data_blk = sb.data_start;
    uint8_t dir_buf[BLOCK_SIZE];
    read_block(fd, root_data_blk, dir_buf);

    struct dirent *entries = (struct dirent *)dir_buf;
    int max_entries = BLOCK_SIZE / sizeof(struct dirent);
    int slot = -1;

    for (int i = 0; i < max_entries; i++) {
        // Find a slot that is empty (inode 0) and has no name
        if (entries[i].inode == 0 && entries[i].name[0] == '\0') {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        printf("Error: Root directory is full.\n");
        exit(1);
    }

    // Fill the directory slot
    entries[slot].inode = free_ino;
    strncpy(entries[slot].name, filename, 27);
    entries[slot].name[27] = '\0'; // Safety null-terminator

    // 4. Write the Transaction to the Journal
    struct journal_header jh;
    off_t j_start = (off_t)sb.journal_block * BLOCK_SIZE;

    // Read current header state
    if (pread(fd, &jh, sizeof(jh), j_start) != sizeof(jh)) fail("Read journal header");
    
    // Initialize header if it's fresh
    if (jh.magic != JOURNAL_MAGIC) {
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(jh);
    }

    off_t cursor = j_start + jh.nbytes_used;

    // Basic space check
    if (jh.nbytes_used + 4 * BLOCK_SIZE > 16 * BLOCK_SIZE) {
        printf("Journal is full. Please run install.\n");
        exit(1);
    }

    // --- Write Data Records ---

    struct data_record rec; 

    // Record 1: Inode Bitmap
    rec.hdr.type = REC_DATA;
    rec.hdr.size = sizeof(rec);
    rec.block_no = sb.inode_bitmap;
    memcpy(rec.data, imap, BLOCK_SIZE);
    
    if (pwrite(fd, &rec, sizeof(rec), cursor) != sizeof(rec)) fail("Write log 1");
    cursor += sizeof(rec);
    jh.nbytes_used += sizeof(rec);

    // Record 2: Inode Table (New File + Updated Root Size)
    rec.block_no = blk_idx;
    memcpy(rec.data, itable_buf, BLOCK_SIZE);

    if (pwrite(fd, &rec, sizeof(rec), cursor) != sizeof(rec)) fail("Write log 2");
    cursor += sizeof(rec);
    jh.nbytes_used += sizeof(rec);

    // Record 3: Directory Data
    rec.block_no = root_data_blk;
    memcpy(rec.data, dir_buf, BLOCK_SIZE);

    if (pwrite(fd, &rec, sizeof(rec), cursor) != sizeof(rec)) fail("Write log 3");
    cursor += sizeof(rec);
    jh.nbytes_used += sizeof(rec);

    // --- Write Commit Record ---
    
    struct commit_record crec;
    crec.hdr.type = REC_COMMIT;
    crec.hdr.size = sizeof(crec);

    if (pwrite(fd, &crec, sizeof(crec), cursor) != sizeof(crec)) fail("Write commit");
    cursor += sizeof(crec);
    jh.nbytes_used += sizeof(crec);

    // Update the header on disk
    if (pwrite(fd, &jh, sizeof(jh), j_start) != sizeof(jh)) fail("Update header");

    fsync(fd); // Flush to disk

    printf("Logged create file '%s' (inode %d)\n", filename, free_ino);
}

void handle_install(int fd) {
    struct superblock sb;
    read_block(fd, 0, &sb);

    struct journal_header jh;
    off_t j_start = (off_t)sb.journal_block * BLOCK_SIZE;

    if (pread(fd, &jh, sizeof(jh), j_start) != sizeof(jh)) fail("Read header");

    if (jh.magic != JOURNAL_MAGIC) {
        printf("No journal found.\n");
        return;
    }

    // Process transactions sequentially
    off_t curr = j_start + sizeof(jh);
    off_t end = j_start + jh.nbytes_used;

    while (curr < end) {
        off_t txn_start = curr;
        off_t scanner = curr;
        int is_valid = 0;

        // Pass 1: Scan forward for a COMMIT
        while (scanner < end) {
            struct rec_header h;
            if (pread(fd, &h, sizeof(h), scanner) != sizeof(h)) break;

            if (h.type == REC_COMMIT) {
                is_valid = 1;
                break;
            }
            scanner += h.size;
        }

        // Pass 2: Replay if valid
        if (is_valid) {
            off_t play_head = txn_start;
            while (play_head <= scanner) {
                struct rec_header h;
                pread(fd, &h, sizeof(h), play_head);

                if (h.type == REC_DATA) {
                    struct data_record d;
                    pread(fd, &d, sizeof(d), play_head);
                    // Write the data to its real home location
                    write_block(fd, d.block_no, d.data);
                }
                play_head += h.size;
            }
            curr = play_head; // Move to the start of the next transaction
        } else {
            // No commit found? We hit the end of valid data.
            break; 
        }
    }

    // Clear the journal
    jh.nbytes_used = sizeof(jh);
    if (pwrite(fd, &jh, sizeof(jh), j_start) != sizeof(jh)) fail("Reset journal");
    
    fsync(fd);
    printf("Journal transactions installed and cleared.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s create <name> | install\n", argv[0]);
        return 1;
    }

    int fd = open(DEFAULT_IMAGE, O_RDWR);
    if (fd < 0) fail("Could not open image");

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s create <name>\n", argv[0]);
            return 1;
        }
        handle_create(fd, argv[2]);
    } 
    else if (strcmp(argv[1], "install") == 0) {
        handle_install(fd);
    } 
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    close(fd);
    return 0;
}
