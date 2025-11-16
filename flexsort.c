#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

// --- Disk/Sector Constants ---
typedef unsigned char u_byte;

#define PROGRAM_VERSION     "1.1.4"
#define SECTOR_SIZE         256
#define SIR_SIZE            24
#define SIR_OFFSET          16
#define MAX_VOL_NAME_LEN    11
#define DIR_ENTRY_SIZE      24
#define DIR_ENTRIES_PER_SECTOR ((SECTOR_SIZE - 16) / DIR_ENTRY_SIZE) 
#define DIR_START_SECTOR    5       // Directory structure starts at T0, S5
#define DIR_START_TRACK     0       // Directory structure is always on T0

// --- FLEX Structures ---

// System Information Record (SIR) structure
typedef struct {
    u_byte volLabel[MAX_VOL_NAME_LEN]; 
    u_byte volNumberHi;     
    u_byte volNumberLo;     
    u_byte firstFreeTrack;  
    u_byte firstFreeSector; 
    u_byte lastFreeTrack;   
    u_byte lastFreeSector;  
    u_byte freeSectorsHi;   
    u_byte freeSectorsLo;   
    u_byte dateMonth;       
    u_byte dateDay;         
    u_byte dateYear;        
    u_byte endTrack;        
    u_byte endSector;       
} SIR_struct;

// Directory Entry structure
typedef struct{
    char      fileName[8];    // 8 byte --- File name
    char      fileExt[3];     // 3 byte --- File extension
    uint16_t  unused;         // 2 byte --- Not used         
    uint8_t   startTrack;     // 1 byte --- Start track
    uint8_t   startSector;    // 1 byte --- Start sector
    uint8_t   endTrack;       // 1 byte --- End track
    uint8_t   endSector;      // 1 byte --- End sector
    uint16_t  totalSectors;   // 2 byte --- Total number of sectors
    uint8_t   randomFileFlag; // 1 byte --- Random file flag
    uint8_t   unused3;        // 1 byte --- Not used
    uint8_t   dateMonth;      // 1 byte --- Date month
    uint8_t   dateDay;        // 1 byte --- Date day
    uint8_t   dateYear;       // 1 byte --- Date year
} DIR_struct;

// --- Global Disk Info ---
u_byte SIR_buffer[SECTOR_SIZE];
u_byte TRACK_COUNT;
u_byte SECTORS_PER_TRACK;

// --- Utility Functions ---

/**
 * @brief Reads a sector from the disk image.
 */
int read_sector(FILE *disk_file, u_byte track, u_byte sector, u_byte *buffer) {
    if (track >= TRACK_COUNT || sector > SECTORS_PER_TRACK || sector == 0) {
        return -1;
    }
    
    // Offset calculation: (Track * SectorsPerTrack + (Sector - 1)) * SectorSize
    long offset = (long)track * SECTORS_PER_TRACK * SECTOR_SIZE + (long)(sector - 1) * SECTOR_SIZE;
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
int write_sector(FILE *disk_file, u_byte track, u_byte sector, const u_byte *buffer) {
    if (track >= TRACK_COUNT || sector > SECTORS_PER_TRACK || sector == 0) {
        return -1;
    }

    long offset = (long)track * SECTORS_PER_TRACK * SECTOR_SIZE + (long)(sector - 1) * SECTOR_SIZE;
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
    // We must read S3 to get SECTORS_PER_TRACK for later calculations.
    u_byte temp_buffer[SECTOR_SIZE];
    long sir_offset = (long)0 * 0 * SECTOR_SIZE + (long)(3 - 1) * SECTOR_SIZE; 
    
    if (fseek(disk_file, sir_offset, SEEK_SET) != 0) return -1;
    if (fread(temp_buffer, 1, SECTOR_SIZE, disk_file) != SECTOR_SIZE) return -1;
    
    memcpy(SIR_buffer, temp_buffer, SECTOR_SIZE);

    SIR_struct *sir = (SIR_struct *)(SIR_buffer + SIR_OFFSET);
    
    SECTORS_PER_TRACK = sir->endSector;
    TRACK_COUNT = sir->endTrack + 1;

    if (SECTORS_PER_TRACK < 5 || TRACK_COUNT < 1) {
        fprintf(stderr, "Error: Invalid disk parameters found in SIR (S/T=%d, T/C=%d).\n", SECTORS_PER_TRACK, TRACK_COUNT);
        return -1;
    }

    return 0;
}

/**
 * @brief Comparison function for qsort (for alphabetical sorting).
 * Ensures correct lexicographical comparison across the 8-byte name and 3-byte extension.
 */
int compare_dir_entries(const void *a, const void *b) {
    const DIR_struct *dir_a = (const DIR_struct *)a;
    const DIR_struct *dir_b = (const DIR_struct *)b;

    // 1. Compare the 8-byte Filename field.
    int name_cmp = strncmp(dir_a->fileName, dir_b->fileName, 8);
    if (name_cmp != 0) {
        return name_cmp;
    }
    
    // 2. If names are identical, compare the 3-byte Extension field.
    return strncmp(dir_a->fileExt, dir_b->fileExt, 3);
}


// --- Main Logic Functions ---

/**
 * @brief Reads all directory entries by following the sector linkage chain.
 * Traverses the directory chain using Bytes 0-1 of each sector.
 * @param disk_file File pointer.
 * @param active_entries Output array of active DIR_structs.
 * @return Total count of active entries.
 */
int read_directory(FILE *disk_file, DIR_struct **active_entries) {
    u_byte current_track = DIR_START_TRACK;
    u_byte current_sector = DIR_START_SECTOR;
    // Estimate max possible entries for initial allocation
    int max_entries_possible = (SECTORS_PER_TRACK - (DIR_START_SECTOR - 1)) * DIR_ENTRIES_PER_SECTOR; 
    
    DIR_struct *entries = (DIR_struct *)malloc(max_entries_possible * sizeof(DIR_struct));
    if (!entries) {
        perror("Error allocating memory for directory entries");
        return -1;
    }

    int active_count = 0;
    u_byte sector_buffer[SECTOR_SIZE];
    
    // Iterate through the directory chain by following links (T0 S0 is end-of-chain)
    while (current_track != 0 || current_sector != 0) {
        if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            fprintf(stderr, "Error reading directory chain link T%d S%d. Stopping read.\n", current_track, current_sector);
            free(entries);
            return -1;
        }

        // The link to the next sector is in bytes 0 and 1
        u_byte next_track = sector_buffer[0];
        u_byte next_sector = sector_buffer[1];

        // Extract 10 directory entries from offset 16
        for (int i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
            size_t offset = 16 + (i * DIR_ENTRY_SIZE);
            DIR_struct *dir_ptr = (DIR_struct *)(sector_buffer + offset);
            
            // Check file status: not unused (0x00) and not deleted (MSB set)
            if (dir_ptr->fileName[0] != 0x00 && !(dir_ptr->fileName[0] & 0x80)) {
                if (active_count < max_entries_possible) {
                    memcpy(&entries[active_count], dir_ptr, DIR_ENTRY_SIZE);
                    active_count++;
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
    // Shrink the memory block to the actual count of active entries
    *active_entries = (DIR_struct *)realloc(entries, active_count * sizeof(DIR_struct));
    
    return active_count;
}

/**
 * @brief Writes the sorted/repacked directory back to the disk, following the original chain.
 * @param disk_file File pointer.
 * @param entries Array of DIR_structs to write.
 * @param count Number of entries to write.
 * @return 0 on success, -1 on failure.
 */
int write_directory(FILE *disk_file, const DIR_struct *entries, int count) {
    u_byte current_track = DIR_START_TRACK;
    u_byte current_sector = DIR_START_SECTOR;
    int entry_index = 0;
    u_byte sector_buffer[SECTOR_SIZE];

    // Read the link for the starting sector (T0, S5) once
    if (read_sector(disk_file, DIR_START_TRACK, DIR_START_SECTOR, sector_buffer) != 0) {
        fprintf(stderr, "Error: Could not read starting directory sector T%d S%d.\n", current_track, current_sector);
        return -1;
    }
    
    // Initial next link read from T0, S5
    u_byte next_track = sector_buffer[0];
    u_byte next_sector = sector_buffer[1];

    // Traverse the original chain and overwrite with new data
    while (current_track != 0 || current_sector != 0) {
        u_byte sector_to_write_track = current_track;
        u_byte sector_to_write_sector = current_sector;
        
        // 1. For sectors after the first one, we must read the link from the disk *before* writing over it.
        // We defer this read until the end of the previous loop iteration in the 'Move' step.
        
        // 2. Prepare the new sector buffer: zero everything
        // This ensures unused entries are marked 0x00
        memset(sector_buffer, 0, SECTOR_SIZE);

        // 3. Populate directory entries
        for (int i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
            size_t offset = 16 + (i * DIR_ENTRY_SIZE);
            if (entry_index < count) {
                // Copy active entry
                memcpy(sector_buffer + offset, &entries[entry_index], DIR_ENTRY_SIZE);
                entry_index++;
            } else {
                // All active files have been written. Stop populating.
                break; 
            }
        }
        
        // 4. Write the link back to the buffer (Bytes 0-1)
        if (entry_index < count) {
             // If there are still entries to write, the link should point to the next sector in the original chain
             sector_buffer[0] = next_track; 
             sector_buffer[1] = next_sector;
        } else {
             // If all entries are written, the directory chain ends here.
             sector_buffer[0] = 0; 
             sector_buffer[1] = 0;
             next_track = 0; // Ensures the loop terminates after this write
             next_sector = 0;
        }
        
        // 5. Write the repacked sector back
        if (write_sector(disk_file, sector_to_write_track, sector_to_write_sector, sector_buffer) != 0) {
            fprintf(stderr, "Error: Failed to write repacked directory sector T%d S%d.\n", sector_to_write_track, sector_to_write_sector);
            return -1;
        }

        // 6. Move to the next sector in the chain
        current_track = next_track;
        current_sector = next_sector;
        
        // 7. If we are moving to the next sector (and not exiting), read its link for the *next* iteration
        if (current_track != 0 || current_sector != 0) {
            if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
                fprintf(stderr, "Error: Could not read original directory sector T%d S%d to find next link.\n", current_track, current_sector);
                return -1;
            }
            next_track = sector_buffer[0];
            next_sector = sector_buffer[1];
        }
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
    long disk_size = (long)TRACK_COUNT * SECTORS_PER_TRACK * SECTOR_SIZE;
    uint16_t free_sectors = (sir->freeSectorsHi << 8) | sir->freeSectorsLo;

    char vol_label[MAX_VOL_NAME_LEN + 1];
    strncpy(vol_label, (char *)sir->volLabel, MAX_VOL_NAME_LEN);
    vol_label[MAX_VOL_NAME_LEN] = '\0';
    
    for (int i = MAX_VOL_NAME_LEN - 1; i >= 0 && vol_label[i] == ' '; i--) {
        vol_label[i] = '\0';
    }

    printf("\nImage size is %ld bytes - %d tracks, %d sectors/track\n\n", 
           disk_size, TRACK_COUNT, SECTORS_PER_TRACK);
    
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
               entries[i].totalSectors,
               entries[i].dateMonth, entries[i].dateDay, entries[i].dateYear,
               entries[i].randomFileFlag);
    }
}

// --- Usage ---

void print_usage(const char *prog_name) {
    fprintf(stderr, "flexsort version %s\n", PROGRAM_VERSION);
    fprintf(stderr, "Usage: %s <disk_image_file> [-a]\n", prog_name);
    fprintf(stderr, "  -a: Sort all active directory entries alphabetically by filename/extension.\n");
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

    // --- 3. Read and Filter Directory Entries ---
    DIR_struct *active_entries = NULL;
    int active_count = read_directory(disk_file, &active_entries);

    if (active_count < 0) {
        // Error handling for allocation failure in read_directory
        fclose(disk_file);
        return 1;
    }
    
    printf("Read %d active file entries.\n", active_count);

    // --- 4. Sort Entries (if requested) ---
    if (sort_flag && active_count > 1) {
        qsort(active_entries, active_count, sizeof(DIR_struct), compare_dir_entries);
        printf("Directory entries sorted alphabetically.\n");
    }

    // --- 5. Repack and Write Directory ---
    if (write_directory(disk_file, active_entries, active_count) != 0) {
        fprintf(stderr, "Error: Failed to write repacked directory.\n");
        free(active_entries);
        fclose(disk_file);
        return 1;
    }
    printf("Directory successfully repacked and written back to '%s'.\n", disk_path);

    // --- 6. Display Results ---
    display_results(active_entries, active_count);

    // --- 7. Cleanup ---
    free(active_entries);
    fclose(disk_file);

    return 0;
}
