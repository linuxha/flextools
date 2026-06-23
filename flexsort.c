#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

// --- Disk/Sector Constants ---
typedef unsigned char u_byte;

// @FIXME: sorts okay but messes up the dir next_* links
#define PROGRAM_VERSION     "1.1.6"

#include "flexfs.h"

// --- Global Disk Info ---
uint8_t  SIR_buffer[SECTOR_SIZE];
uint16_t track_count;
uint8_t  sectors_per_track;

// --- Utility Functions ---

/**
 * @brief Reads a sector from the disk image.
 */
int read_sector(FILE *disk_file, uint16_t track, uint8_t sector, uint8_t *buffer) {
    if (track >= track_count || sector > sectors_per_track || sector == 0) {
        return -1;
    }
    
    // Offset calculation: (Track * SectorsPerTrack + (Sector - 1)) * SectorSize
    long offset = (long)track * sectors_per_track * SECTOR_SIZE + (long)(sector - 1) * SECTOR_SIZE;
    if (fseek(disk_file, offset, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buffer, 1, SECTOR_SIZE, disk_file) != SECTOR_SIZE) {
        return -1;
    }
    return 0;
}

/**
 * @brief Writes a sector to the disk image.
 */
int write_sector(FILE *disk_file, uint16_t track, uint8_t sector, const uint8_t *buffer) {
    if (track >= track_count || sector > sectors_per_track || sector == 0) {
        return -1;
    }

    long offset = (long)track * sectors_per_track * SECTOR_SIZE + (long)(sector - 1) * SECTOR_SIZE;
    if (fseek(disk_file, offset, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buffer, 1, SECTOR_SIZE, disk_file) != SECTOR_SIZE) {
        return -1;
    }
    return 0;
}

/**
 * @brief Reads the SIR and sets global disk parameters.
 */
int init_disk_info(FILE *disk_file) {
    // We must read S3 to get sectors_per_track for later calculations.
    uint8_t temp_buffer[SECTOR_SIZE];
    long sir_offset = (long)0 * 0 * SECTOR_SIZE + (long)(3 - 1) * SECTOR_SIZE; 
    
    if (fseek(disk_file, sir_offset, SEEK_SET) != 0) return -1;
    if (fread(temp_buffer, 1, SECTOR_SIZE, disk_file) != SECTOR_SIZE) return -1;
    
    memcpy(SIR_buffer, temp_buffer, SECTOR_SIZE);

    SIR_struct *sir = (SIR_struct *)(SIR_buffer + SIR_OFFSET);
    
    sectors_per_track = sir->endSector;
    track_count = sir->endTrack + 1;

    if (sectors_per_track < 5 || track_count < 1) {
        fprintf(stderr, "Error: Invalid disk parameters found in SIR (S/T=%d, T/C=%d).\n", sectors_per_track, track_count);
        return -1;
    }

    return 0;
}

/**
 * @brief Returns non-zero if a directory entry is marked deleted.
 */
int is_deleted_entry(const DIR_struct *dir) {
    uint8_t first = (uint8_t)dir->fileName[0];
    return (first == 0xFF) || ((first & 0x80) != 0);
}

/**
 * @brief Comparison function for qsort.
 * Active entries are sorted alphabetically first, deleted entries are always last.
 */
int compare_dir_entries(const void *a, const void *b) {
    const DIR_struct *dir_a = (const DIR_struct *)a;
    const DIR_struct *dir_b = (const DIR_struct *)b;

    int deleted_a = is_deleted_entry(dir_a);
    int deleted_b = is_deleted_entry(dir_b);

    if (deleted_a != deleted_b) {
        return deleted_a - deleted_b;
    }

    int name_cmp = strncmp(dir_a->fileName, dir_b->fileName, 8);
    if (name_cmp != 0) {
        return name_cmp;
    }

    return strncmp(dir_a->fileExt, dir_b->fileExt, 3);
}


// --- Main Logic Functions ---

/**
 * @brief Reads all directory entries by following the sector linkage chain.
 * Traverses the directory chain using Bytes 0-1 of each sector.
 * @param disk_file File pointer.
 * @param entries_out Output array of non-empty DIR_structs (active + deleted).
 * @return Total count of non-empty entries.
 */
int read_directory(FILE *disk_file, DIR_struct **entries_out) {
    uint8_t current_track = DIR_START_TRACK;
    uint8_t current_sector = DIR_START_SECTOR;
    // Estimate max possible entries for initial allocation
    int max_entries_possible = (sectors_per_track - (DIR_START_SECTOR - 1)) * DIR_ENTRIES_PER_SECTOR; 
    
    DIR_struct *entries = (DIR_struct *)malloc(max_entries_possible * sizeof(DIR_struct));
    if (!entries) {
        perror("Error allocating memory for directory entries");
        return -1;
    }

    int entry_count = 0;
    uint8_t sector_buffer[SECTOR_SIZE];
    
    // Iterate through the directory chain by following links (T0 S0 is end-of-chain)
    while (current_track != 0 || current_sector != 0) {
        if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            fprintf(stderr, "Error reading directory chain link T%d S%d. Stopping read.\n", current_track, current_sector);
            free(entries);
            return -1;
        }

        // The link to the next sector is in bytes 0 and 1
        uint8_t next_track = sector_buffer[0];
        uint8_t next_sector = sector_buffer[1];

        // Extract 10 directory entries from offset 16
        for (int i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
            size_t offset = 16 + (i * DIR_ENTRY_SIZE);
            DIR_struct *dir_ptr = (DIR_struct *)(sector_buffer + offset);
            
            // Keep all non-empty entries (active + deleted). Deleted entries are
            // sorted to the end later.
            if ((uint8_t)dir_ptr->fileName[0] != 0x00) {
                if (entry_count < max_entries_possible) {
                    memcpy(&entries[entry_count], dir_ptr, DIR_ENTRY_SIZE);
                    entry_count++;
                } else {
                    fprintf(stderr, "Warning: Maximum directory capacity reached during read. Some files may be skipped.\n");
                    goto cleanup;
                }
            }
        }
        
        // Move to the next sector in the chain
        current_track = next_track;
        current_sector = next_sector;
    }

cleanup:
    // Shrink the memory block to the actual count of non-empty entries.
    *entries_out = (DIR_struct *)realloc(entries, entry_count * sizeof(DIR_struct));
    
    return entry_count;
}

/**
 * @brief Writes the sorted/repacked directory back to the disk, following the original chain.
 * @param disk_file File pointer.
 * @param entries Array of DIR_structs to write.
 * @param count Number of entries to write.
 * @return 0 on success, -1 on failure.
 */
int write_directory(FILE *disk_file, const DIR_struct *entries, int count) {
    uint8_t current_track = DIR_START_TRACK;
    uint8_t current_sector = DIR_START_SECTOR;
    int entry_index = 0;
    uint8_t sector_buffer[SECTOR_SIZE];
    uint8_t original_sector[SECTOR_SIZE];

    // Traverse the original chain and rewrite every directory sector while
    // preserving original next links in bytes 0-1.
    while (current_track != 0 || current_sector != 0) {
        if (read_sector(disk_file, current_track, current_sector, original_sector) != 0) {
            fprintf(stderr, "Error: Could not read original directory sector T%d S%d.\n", current_track, current_sector);
            return -1;
        }

        uint8_t next_track = original_sector[0];
        uint8_t next_sector = original_sector[1];

        memset(sector_buffer, 0, SECTOR_SIZE);
        sector_buffer[0] = next_track;
        sector_buffer[1] = next_sector;

        // Populate directory entries for this sector, remaining slots are 0x00.
        for (int i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
            size_t offset = 16 + (i * DIR_ENTRY_SIZE);
            if (entry_index < count) {
                memcpy(sector_buffer + offset, &entries[entry_index], DIR_ENTRY_SIZE);
                entry_index++;
            }
        }

        if (write_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            fprintf(stderr, "Error: Failed to write repacked directory sector T%d S%d.\n", current_track, current_sector);
            return -1;
        }

        current_track = next_track;
        current_sector = next_sector;
    }

    if (entry_index < count) {
        fprintf(stderr, "Error: Directory chain is too short! Could not write %d file(s).\n", count - entry_index);
        return -1;
    }

    return 0;
}

/**
 * @brief Prints the SIR and directory listing in the requested format.
 */
void display_results(const DIR_struct *entries, int count) {
    SIR_struct *sir = (SIR_struct *)(SIR_buffer + SIR_OFFSET);
    
    // --- Display SIR Info ---
    long disk_size = (long)track_count * sectors_per_track * SECTOR_SIZE;
    uint16_t free_sectors = (sir->freeSectorsHi << 8) | sir->freeSectorsLo;

    char vol_label[MAX_VOL_NAME_LEN + 1];
    strncpy(vol_label, (char *)sir->volLabel, MAX_VOL_NAME_LEN);
    vol_label[MAX_VOL_NAME_LEN] = '\0';
    
    for (int i = MAX_VOL_NAME_LEN - 1; i >= 0 && vol_label[i] == ' '; i--) {
        vol_label[i] = '\0';
    }

    printf("\nImage size is %ld bytes - %d tracks, %d sectors/track\n\n", 
           disk_size, track_count, sectors_per_track);
    
    printf("Volume label      %-11s\n", vol_label);
    printf("Volume number     %04d\n", sir->volNumberLo);
    printf("Free area         t%d s%d - t%d s%d\n", 
           sir->firstFreeTrack, sir->firstFreeSector, 
           sir->lastFreeTrack, sir->lastFreeSector);
    printf("Free sectors      %d\n", free_sectors);
    printf("End sector        t%d s%d\n", sir->endTrack, sir->endSector);
    printf("Creation date     %02d-%02d-%02d\n", 
           sir->dateYear, sir->dateMonth, sir->dateDay); 
    
    printf("\n");
    printf("NAME              START     END      SIZE     DATE       FLAG\n");
    
    // --- Display Directory Entries ---
    for (int i = 0; i < count; i++) {
        char name[9];
        char ext[4];
        
        strncpy(name, entries[i].fileName, 8);
        name[8] = '\0';
        strncpy(ext, entries[i].fileExt, 3);
        ext[3] = '\0';

        char full_name[13];
        snprintf(full_name, sizeof(full_name), "%.8s.%.3s", name, ext);

        printf("%-17s t%02d s%02d - t%02d s%02d %6d %02d-%02d-%02d %04X\n",
               full_name,
               entries[i].startTrack, entries[i].startSector,
               entries[i].endTrack, entries[i].endSector,
               ((entries[i].totalSectorsHi >>8) + (entries[i].totalSectorsLo)),
               entries[i].dateMonth, entries[i].dateDay, entries[i].dateYear,
               entries[i].randomFileFlag);
    }
}

// --- Usage ---

void print_usage(const char *prog_name) {
    fprintf(stderr, "flexsort version %s\n", PROGRAM_VERSION);
    fprintf(stderr, "Usage: %s <disk_image_file> [-a]\n", prog_name);
    fprintf(stderr, "  -a: Sort all non-empty directory entries alphabetically; deleted entries are placed last.\n");
}

// --- Main Function ---

int main(int argc, char *argv[]) {
    if (argc < 2 || (argc == 2 && argv[1][0] == '-')) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *disk_path = argv[1];
    int sort_flag = 0;
    
    if (argc == 3 && strcmp(argv[2], "-a") == 0) {
        sort_flag = 1;
    } else if (argc > 3 || (argc == 2 && argv[1][0] == '-')) {
        print_usage(argv[0]);
        return 1;
    }

    // --- 1. Open Disk Image ---
    FILE *disk_file = fopen(disk_path, "r+b"); 
    if (!disk_file) {
        perror("Error opening disk image file");
        return 1;
    }

    // --- 2. Initialize Disk Info ---
    if (init_disk_info(disk_file) != 0) {
        fclose(disk_file);
        return 1;
    }

    // --- 3. Read non-empty directory entries (active + deleted) ---
    DIR_struct *entries = NULL;
    int entry_count = read_directory(disk_file, &entries);

    if (entry_count < 0) {
        // Error handling for allocation failure in read_directory
        fclose(disk_file);
        return 1;
    }

    int deleted_count = 0;
    for (int i = 0; i < entry_count; i++) {
        if (is_deleted_entry(&entries[i])) {
            deleted_count++;
        }
    }

    printf("Read %d non-empty directory entries (%d active, %d deleted).\n",
           entry_count, entry_count - deleted_count, deleted_count);

    // --- 4. Sort Entries (if requested) ---
    if (sort_flag && entry_count > 1) {
        qsort(entries, entry_count, sizeof(DIR_struct), compare_dir_entries);
        printf("Directory entries sorted alphabetically (deleted entries last).\n");
    }

    // --- 5. Repack and Write Directory ---
    if (write_directory(disk_file, entries, entry_count) != 0) {
        fprintf(stderr, "Error: Failed to write repacked directory.\n");
        free(entries);
        fclose(disk_file);
        return 1;
    }
    printf("Directory successfully repacked and written back to '%s'.\n", disk_path);

    // --- 6. Display Results ---
    display_results(entries, entry_count);

    // --- 7. Cleanup ---
    free(entries);
    fclose(disk_file);

    return 0;
}
