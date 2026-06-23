/**
 * flexadd - Add files from the host system to FLEX disk images
 *
 * Purpose:
 *   flexadd adds or replaces files on FLEX disk images, supporting optional text
 *   translation (LF to CR). It implements existing-file detection with interactive
 *   confirmation and safe sector reclamation for deleted files.
 *
 * Directory Alignment (Critical):
 *   FLEX directory sectors (T0,S5 onwards) have a 16-byte header (sector link and LRN),
 *   followed by 24-byte directory entries:
 *   - Entry 0: bytes 16-39
 *   - Entry 1: bytes 40-63
 *   - Entry 2: bytes 64-87
 *   Entry offset: 16 + (i * 24) where i is the 0-based slot index.
 *
 *   NOTE: Previous versions incorrectly started from i=1, placing the first entry at
 *   byte 40 instead of byte 16. This caused directory misalignment and file readability
 *   issues. Now fixed to start iteration at i=0.
 *
 * Sector Layout (Data Sectors):
 *   - Bytes 0-1: Next track/sector link (0,0 indicates end of chain)
 *   - Bytes 2-3: Logical record number (1-based counter: 1st, 2nd, 3rd sector, etc.)
 *   - Bytes 4-255: Data payload (252 bytes)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#define VERSION "1.1.2"

// Sector size constants
#define SECTOR_SIZE_128     128
#define SECTOR_SIZE_256     256
#define DEFAULT_SECTOR_SIZE 256

#include "flexfs.h"

// --- Global Data/State ---
int        sector_size = DEFAULT_SECTOR_SIZE;
uint8_t   *SIR_buffer = NULL;
uint16_t   track_count;
uint8_t    sectors_per_track;
uint8_t    dir_start_sector = 5;

uint8_t    start_tracks;
uint8_t    start_sectors;

uint8_t    end_track, end_sector;

// --- Utility Functions ---

/**
 * @brief Converts a Linux filename and extension to the 8.3 FLEX format.
 * @param linux_filename The source filename (e.g., my_file.txt).
 * @param flex_name Output 8-byte buffer for the name.
 * @param flex_ext Output 3-byte buffer for the extension.
 */
void convert_filename(const char *linux_filename, char *flex_name, char *flex_ext) {
    memset(flex_name, 0x00, 8); // Don't fill with spaces
    memset(flex_ext, 0x00, 3);

    const char *dot = strrchr(linux_filename, '.');
    size_t name_len;
    size_t ext_len = 0;

    if (dot) {
        name_len = (dot - linux_filename);
        ext_len = strlen(dot + 1);
    } else {
        name_len = strlen(linux_filename);
    }

    // Convert name (max 8 chars) to uppercase
    for (size_t i = 0; i < name_len && i < 8; i++) {
        flex_name[i] = toupper(linux_filename[i]);
    }

    // Convert extension (max 3 chars) to uppercase
    if (dot) {
        for (size_t i = 0; i < ext_len && i < 3; i++) {
            flex_ext[i] = toupper(dot[1 + i]);
        }
    }
}

/**
 * @brief Reads a sector from the disk image.
 * @param disk_file File pointer to the disk image.
 * @param track Track number (0-255).
 * @param sector Sector number (1-255).
 * @param buffer Buffer to store sector_size bytes of data.
 * @return 0 on success, -1 on failure.
 */
int read_sector(FILE *disk_file, uint8_t track, uint8_t sector, uint8_t *buffer) {
    long offset = (long)track * sectors_per_track * sector_size + (long)(sector - 1) * sector_size;
    if (fseek(disk_file, offset, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to T%d S%d.\n", track, sector);
        return -1;
    }
    if (fread(buffer, 1, sector_size, disk_file) != sector_size) {
        fprintf(stderr, "Error: Cannot read T%d S%d.\n", track, sector);
        return -1;
    }
    return 0;
}

/**
 * @brief Writes a sector to the disk image.
 * @param disk_file File pointer to the disk image.
 * @param track Track number (0-255).
 * @param sector Sector number (1-255).
 * @param buffer Buffer containing sector_size bytes of data.
 * @return 0 on success, -1 on failure.
 */
int write_sector(FILE *disk_file, uint16_t track, uint8_t sector, const uint8_t *buffer) {
    long offset = (long)track * sectors_per_track * sector_size + (long)(sector - 1) * sector_size;
    if (fseek(disk_file, offset, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to write T%d S%d.\n", track, sector);
        return -1;
    }
    if (fwrite(buffer, 1, sector_size, disk_file) != sector_size) {
        fprintf(stderr, "Error: Cannot write T%d S%d.\n", track, sector);
        return -1;
    }
    return 0;
}

/**
 * @brief Reads the SIR and sets global variables.
 * @param disk_file File pointer to the disk image.
 * @return 0 on success, -1 on failure.
 */
int init_disk_info(FILE *disk_file) {
    // Allocate SIR buffer based on sector size
    if (SIR_buffer) {
        free(SIR_buffer);
    }
    SIR_buffer = calloc(1, sector_size);
    if (!SIR_buffer) {
        fprintf(stderr, "Error: Failed to allocate SIR buffer.\n");
        return -1;
    }

    // Read SIR sector (Track 0, Sector 3)
    if (read_sector(disk_file, 0, 3, SIR_buffer) != 0) {
        fprintf(stderr, "Error: Failed to read SIR sector (T0 S3).\n");
        return -1;
    }

    // Extract sectors per track (needed for addressing) and total tracks
    SIR_struct *sir = (SIR_struct *)(SIR_buffer + SIR_OFFSET);
    
    track_count       = sir->endTrack + 1;
    sectors_per_track = sir->endSector;
    start_tracks      = sir->firstFreeTrack;
    start_sectors     = sir->firstFreeSector;

    if (sectors_per_track < 5 || track_count < 1) {
        fprintf(stderr, "Error: Invalid disk parameters found in SIR (T%d/S%d).\n", track_count, sectors_per_track);
        return -1;
    }

    return 0;
}

/**
 * @brief Finds the first available free sector from the free chain.
 * @param disk_file File pointer to the disk image.
 * @param track Output: Track of the free sector.
 * @param sector Output: Sector of the free sector.
 * @return 0 on success, -1 if no free sectors found.
 */
int find_free_sector(FILE *disk_file, uint8_t *track, uint8_t *sector) {
    SIR_struct *sir = (SIR_struct *)(SIR_buffer + SIR_OFFSET);
    
    int i = 0;
    for( i = 0; i < SIR_SIZE; i++) {
        fprintf(stderr, "%02x ", SIR_buffer[i+SIR_OFFSET]);
    }
    fprintf(stderr, "\n");

    *track  = sir->firstFreeTrack;
    *sector = sir->firstFreeSector;
    
    if (*track == 0 && *sector == 0) {
        return -1; // No free sectors left
    }
    
    // Read the current free sector to find the link to the next one
    uint8_t *sector_data = calloc(1, sector_size);
    if (!sector_data) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }
    
    if (read_sector(disk_file, *track, *sector, sector_data) != 0) {
        free(sector_data);
        return -1;
    }
    
    // The next free sector is stored in bytes 0 and 1
    uint8_t next_track  = sector_data[0];
    uint8_t next_sector = sector_data[1];
    
    // Update SIR with the new head of the free chain
    sir->firstFreeTrack  = next_track;
    sir->firstFreeSector = next_sector;

    // Decrement free sector count
    uint16_t free_sectors = (uint16_t)((sir->freeSectorsHi << 8) + (sir->freeSectorsLo));
    free_sectors--;
    sir->freeSectorsHi = (uint8_t)((free_sectors >> 8) & 0xFF);
    sir->freeSectorsLo = (uint8_t)(free_sectors & 0xFF);
    
    // Write the updated SIR back to the disk
    if (write_sector(disk_file, 0, 3, SIR_buffer) != 0) {
        fprintf(stderr, "Error: Failed to update SIR free chain info.\n");
        free(sector_data);
        return -1;
    }
    
    free(sector_data);
    return 0;
}

/**
 * @brief Writes the given file content to a chain of newly allocated sectors.
 * @param disk_file File pointer to the disk image.
 * @param source_file_content The file data buffer.
 * @param content_size Size of the file data.
 * @param is_text_file Flag indicating if text translation is needed.
 * @param start_track Output: Starting track of the file's data chain.
 * @param start_sector Output: Starting sector of the file's data chain.
 * @param sector_count Output: Total number of sectors used.
 * @return 0 on success, -1 on failure.
 */
int write_file_data(FILE *disk_file, const uint8_t *source_file_content, long content_size, int is_text_file, uint8_t *start_track, uint8_t *start_sector, uint16_t *sector_count) {
    const uint8_t *current_data = source_file_content;
    long bytes_remaining = content_size;
    *sector_count = 0;

    uint8_t current_track, current_sector;
    // Okay, this works by updating the previous sector's next_track, next_sector
    // when the sector_count > 0
    uint8_t prev_track = 0, prev_sector = 0;
    
    // Buffer for the sector being written
    uint8_t *sector_buffer = calloc(1, sector_size);
    if (!sector_buffer) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }

    // Main loop: Write data sector by sector
    while (bytes_remaining > 0 || *sector_count == 0) {
        // 1. Allocate a free sector
        if (find_free_sector(disk_file, &current_track, &current_sector) != 0) {
            fprintf(stderr, "Error: Out of free disk sectors!\n");
            // NOTE: In a real utility, rollback/cleanup logic would be needed here.
            return -1;
        }

    fprintf(stderr, "Current: T%d/S%d\n", current_track, current_sector);

    end_track  = current_track;
    end_sector = current_sector;

    fprintf(stderr, "Current: T%d/S%d\n", end_track, end_sector);

        if (*sector_count == 0) {
            *start_track  = current_track;
            *start_sector = current_sector;
        }

        // 2. Link the previous sector to this new sector
        if (*sector_count > 0) {
            // Read previous sector to update its link field (Bytes 0-1)
            uint8_t *prev_sector_buffer = calloc(1, sector_size);
            if (!prev_sector_buffer) {
                fprintf(stderr, "Error: Failed to allocate previous sector buffer.\n");
                free(sector_buffer);
                return -1;
            }
            
            if (read_sector(disk_file, prev_track, prev_sector, prev_sector_buffer) != 0) {
                free(prev_sector_buffer);
                free(sector_buffer);
                return -1;
            }
            
            prev_sector_buffer[0] = current_track;  // Link Track
            prev_sector_buffer[1] = current_sector; // Link Sector
            
            // Write the updated previous sector back
            if (write_sector(disk_file, prev_track, prev_sector, prev_sector_buffer) != 0) {
                free(prev_sector_buffer);
                free(sector_buffer);
                return -1;
            }
            
            free(prev_sector_buffer);
        }

        // 3. Prepare data for the current sector
        memset(sector_buffer, 0, sector_size);
        
        // Bytes 0-1: Link (next track/sector), Bytes 2-3: Logical Record Number (LRN)
        // These are zeroed initially; will be updated below.
        long data_space = sector_size - 4;
        long bytes_to_copy = (bytes_remaining > data_space) ? data_space : bytes_remaining;

        // Copy data into bytes 4 onwards (payload area)
        memcpy(sector_buffer + 4, current_data, bytes_to_copy);
        
        // 4. Set the logical record number (1-based) in bytes 2-3.
        // This is the sector's position in the file chain (1st, 2nd, 3rd, etc.).
        // FLEX extractors (flextract, flexfs) validate this field to detect truncated files.
        // Must match the directory entry's sector count to pass integrity checks.
        uint16_t logical_record = (uint16_t)(*sector_count + 1);
        sector_buffer[2] = (logical_record >> 8) & 0xFF;  // High byte (big-endian)
        sector_buffer[3] = logical_record & 0xFF;         // Low byte

        // 5. Update state and write current sector
        current_data    += bytes_to_copy;
        bytes_remaining -= bytes_to_copy;
        (*sector_count)++;

        // Check if this is the last sector of the file
        if (bytes_remaining <= 0) {
            // Set Link to 0, 0 in bytes 0-1 (already zeroed by memset)
            // LRN (bytes 2-3) remains 0 by default.
        } else {
            // Placeholder: Link field will be updated in the NEXT loop iteration
            //
            sector_buffer[0] = 0;
            sector_buffer[1] = 0;
        }

        // Write the current sector
        if (write_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            free(sector_buffer);
            return -1;
        }
        
        // Update previous pointer for the next iteration
        prev_track  = current_track;
        prev_sector = current_sector;
        
        // Break if we finished writing an empty file (sector_count will be 1)
        if (*sector_count > 0 && content_size == 0) break;
    }

    free(sector_buffer);
    return 0;
}

/**
 * @brief Finds an existing directory entry by FLEX name/ext.
 * 
 * Scans directory sectors starting at T0,S5, checking each 24-byte entry slot.
 * Directory entries begin at byte 16 (after 16-byte sector header), with each
 * entry at offset: 16 + (i * 24) where i is the slot index (0-based).
 * 
 * Skips empty entries (first byte == 0x00) and deleted entries (first byte == 0xFF
 * or high-bit set). Chains to next directory sector via link bytes (0-1) if needed.
 * 
 * @param found_sector Output: sector number (T0) where entry was found
 * @param found_index Output: slot index (0-based) within that sector
 * @param found_entry Output: copy of the 24-byte DIR_struct
 * @return 1 if found, 0 if not found, -1 on error
 */
int find_directory_entry(FILE *disk_file, const char *flex_name, const char *flex_ext,
        uint8_t *found_sector, int *found_index, DIR_struct *found_entry) {
    uint8_t current_track  = 0;
    uint8_t current_sector = DIR_START_SECTOR;
    uint8_t *sector_buffer = calloc(1, sector_size);
    if (!sector_buffer) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }

    int dir_entries_per_sector = (sector_size - 16) / DIR_ENTRY_SIZE;

    // Scan directory sectors, stopping at sector 0 (wrap-around guard)
    while (current_sector <= sectors_per_track && current_sector != 0) {
        if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            free(sector_buffer);
            return -1;
        }

        // Iterate through all directory entry slots in this sector (0-based)
        for (int i = 0; i < dir_entries_per_sector; i++) {
            size_t entry_offset = 16 + (i * DIR_ENTRY_SIZE);  // First entry at byte 16
            DIR_struct *dir_ptr = (DIR_struct *)(sector_buffer + entry_offset);

            if (dir_ptr->fileName[0] == 0x00 || (dir_ptr->fileName[0] & 0x80)) {
                continue;
            }

            if (memcmp(dir_ptr->fileName, flex_name, 8) == 0 &&
                memcmp(dir_ptr->fileExt, flex_ext, 3) == 0) {
                if (found_sector) {
                    *found_sector = current_sector;
                }
                if (found_index) {
                    *found_index = i;
                }
                if (found_entry) {
                    memcpy(found_entry, dir_ptr, DIR_ENTRY_SIZE);
                }
                free(sector_buffer);
                return 1;
            }
        }

        current_sector++;
    }

    free(sector_buffer);
    return 0;
}

/**
 * @brief Marks a directory entry as deleted (high-bit set on first filename char).
 * @return 0 on success, -1 on failure.
 */
/**
 * @brief Marks a directory entry as deleted by setting first byte to 0xFF.
 * 
 * Standard FLEX format: deleted entries have first byte of filename == 0xFF.
 * This prevents directory walkers (flextract, flexfs) from displaying stale entries.
 * 
 * Entry is located at: offset 16 + (entry_index * 24) within the sector.
 * 
 * @param sector Directory sector number (typically T0,S5 or higher)
 * @param entry_index Slot index (0-based) within that sector
 * @return 0 on success, -1 on failure
 */
int delete_directory_entry(FILE *disk_file, uint8_t sector, int entry_index) {
    uint8_t *sector_buffer = calloc(1, sector_size);
    if (!sector_buffer) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }

    if (read_sector(disk_file, 0, sector, sector_buffer) != 0) {
        free(sector_buffer);
        return -1;
    }

    // Calculate entry offset: first entry at byte 16, then 24-byte intervals
    size_t entry_offset = 16 + (entry_index * DIR_ENTRY_SIZE);
    DIR_struct *dir_ptr = (DIR_struct *)(sector_buffer + entry_offset);
    // Mark deleted: set first byte to 0xFF (FLEX standard deleted marker)
    ((uint8_t *)dir_ptr->fileName)[0] = 0xFF;

    if (write_sector(disk_file, 0, sector, sector_buffer) != 0) {
        free(sector_buffer);
        return -1;
    }

    free(sector_buffer);
    return 0;
}

/**
 * @brief Normalizes legacy deleted markers (high-bit set first char) to 0xFF.
 * @return 0 on success, -1 on failure.
 */
int normalize_deleted_directory_entries(FILE *disk_file) {
    uint8_t current_track  = 0;
    uint8_t current_sector = DIR_START_SECTOR;
    uint8_t *sector_buffer = calloc(1, sector_size);
    if (!sector_buffer) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }

    int dir_entries_per_sector = (sector_size - 16) / DIR_ENTRY_SIZE;

    while (current_sector <= sectors_per_track && current_sector != 0) {
        int changed = 0;

        if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            free(sector_buffer);
            return -1;
        }

        for (int i = 0; i < dir_entries_per_sector; i++) {
            size_t entry_offset = 16 + (i * DIR_ENTRY_SIZE);
            DIR_struct *dir_ptr = (DIR_struct *)(sector_buffer + entry_offset);
            uint8_t first = ((uint8_t *)dir_ptr->fileName)[0];

            if (first != 0xFF && (first & 0x80)) {
                ((uint8_t *)dir_ptr->fileName)[0] = 0xFF;
                changed = 1;
            }
        }

        if (changed) {
            if (write_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
                free(sector_buffer);
                return -1;
            }
        }

        current_sector++;
    }

    free(sector_buffer);
    return 0;
}

/**
 * @brief Returns a deleted file's sector chain to the free list.
 * @return 0 on success, -1 on failure.
 */
int reclaim_deleted_file_chain(FILE *disk_file, const DIR_struct *entry) {
    uint16_t sector_total = (uint16_t)(((uint16_t)entry->totalSectorsHi << 8) | entry->totalSectorsLo);
    if (sector_total == 0 || (entry->startTrack == 0 && entry->startSector == 0)) {
        return 0;
    }

    SIR_struct *sir = (SIR_struct *)(SIR_buffer + SIR_OFFSET);
    uint8_t *sector_buffer = calloc(1, sector_size);
    if (!sector_buffer) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }

    uint8_t current_track = entry->startTrack;
    uint8_t current_sector = entry->startSector;
    uint8_t last_track = 0;
    uint8_t last_sector = 0;

    for (uint16_t i = 0; i < sector_total; i++) {
        if (current_track == 0 || current_sector == 0 || current_sector > sectors_per_track) {
            fprintf(stderr, "Error: Invalid chain while reclaiming deleted file.\n");
            free(sector_buffer);
            return -1;
        }

        if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            free(sector_buffer);
            return -1;
        }

        last_track = current_track;
        last_sector = current_sector;
        current_track = sector_buffer[0];
        current_sector = sector_buffer[1];
    }

    if (read_sector(disk_file, last_track, last_sector, sector_buffer) != 0) {
        free(sector_buffer);
        return -1;
    }
    sector_buffer[0] = sir->firstFreeTrack;
    sector_buffer[1] = sir->firstFreeSector;
    if (write_sector(disk_file, last_track, last_sector, sector_buffer) != 0) {
        free(sector_buffer);
        return -1;
    }

    if (sir->firstFreeTrack == 0 && sir->firstFreeSector == 0) {
        sir->lastFreeTrack = last_track;
        sir->lastFreeSector = last_sector;
    }
    sir->firstFreeTrack = entry->startTrack;
    sir->firstFreeSector = entry->startSector;

    uint16_t free_sectors = (uint16_t)(((uint16_t)sir->freeSectorsHi << 8) | sir->freeSectorsLo);
    free_sectors += sector_total;
    sir->freeSectorsHi = (uint8_t)((free_sectors >> 8) & 0xFF);
    sir->freeSectorsLo = (uint8_t)(free_sectors & 0xFF);

    if (write_sector(disk_file, 0, 3, SIR_buffer) != 0) {
        fprintf(stderr, "Error: Failed to update SIR after delete.\n");
        free(sector_buffer);
        return -1;
    }

    free(sector_buffer);
    return 0;
}

/**
 * @brief Finds the first available (zeroed) directory entry and updates it.
 * @param disk_file File pointer to the disk image.
 * @param entry The fully populated DIR_struct to write.
 * @return 0 on success, -1 on failure.
 */
/**
 * @brief Finds the first free directory entry slot and writes the new entry.
 * 
 * Scans directory sectors starting at T0,S5 for an unused slot (first byte == 0x00).
 * Once found, writes the DIR_struct (24 bytes) at that offset and returns.
 * 
 * Directory entries are positioned at: offset 16 + (i * 24) for slot i (0-based).
 * Continues to next sector if current sector is full, stops if reaches sector 0 (wrap).
 * 
 * @param entry Fully populated DIR_struct to write
 * @return 0 on success, -1 on failure
 */
int write_directory_entry(FILE *disk_file, const DIR_struct *entry) {
    uint8_t current_track  = 0;
    uint8_t current_sector = DIR_START_SECTOR;
    uint8_t *sector_buffer = calloc(1, sector_size);
    if (!sector_buffer) {
        fprintf(stderr, "Error: Failed to allocate sector buffer.\n");
        return -1;
    }
    
    // Directory sectors start at T0, S5 and continue up to T0, S(sectors_per_track)
    // We assume the directory does not span multiple tracks for simplicity, as per flexdsk.c
    int dir_entries_per_sector = (sector_size - 16) / DIR_ENTRY_SIZE;

    while (current_sector <= sectors_per_track && current_sector != 0) {
        if (read_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
            free(sector_buffer);
            return -1;
        }

        // Scan for first free slot (first byte == 0x00). Slots start at index 0.
        for (int i = 0; i < dir_entries_per_sector; i++) {
            // Directory entries start at offset 16 (after 16-byte header), then 24-byte intervals
            size_t entry_offset = 16 + (i * DIR_ENTRY_SIZE);
            DIR_struct *dir_ptr = (DIR_struct *)(sector_buffer + entry_offset);
            
            // Check if entry is unused (first byte of filename is 0)
            if (dir_ptr->fileName[0] == 0x00) {
                // Found a free spot! Copy the new entry data.
                memcpy(dir_ptr, entry, DIR_ENTRY_SIZE);

                // Write the updated directory sector back to disk
                if (write_sector(disk_file, current_track, current_sector, sector_buffer) != 0) {
                    fprintf(stderr, "Error: Failed to write updated directory sector T%d S%d.\n", current_track, current_sector);
                    free(sector_buffer);
                    return -1;
                }
                printf("Directory updated at T%d S%d, entry %d.\n", current_track, current_sector, i + 1);
                free(sector_buffer);
                return 0; // Success!
            }
        }
        
        // If we reached the end of the current track's directory sectors (T0, Sn), stop.
        current_sector++;
    }

    fprintf(stderr, "Error: Directory is full. Cannot add file.\n");
    free(sector_buffer);
    return -1;
}

/**
 * @brief Translates Linux text file content to FLEX format.
 * Replaces LF ($0A) with CR ($0D), drops CR ($0D), and applies FLEX
 * space compression using 0x09 + count for space runs of length 2-127.
 * @param content_in Input buffer.
 * @param size_in Input size.
 * @param content_out Output buffer (must be large enough).
 * @return Size of the translated content.
 */
long translate_text_content(const uint8_t *content_in, long size_in, uint8_t *content_out) {
    long size_out = 0;

    for (long i = 0; i < size_in; i++) {
        uint8_t c = content_in[i];

        if (c == '\r') {
            // Ignore Windows/Mac CR ($0D) if present in host file.
            continue;
        }
        if (c == '\n') {
            c = 0x0D; // FLEX newline is CR
        }

        if (c == ' ') {
            int run = 1;
            while ((i + 1) < size_in && content_in[i + 1] == ' ' && run < 127) {
                run++;
                i++;
            }

            if (run >= 2) {
                content_out[size_out++] = 0x09;          // FLEX compression marker
                content_out[size_out++] = (uint8_t)run;  // Space count (2-127)
            } else {
                content_out[size_out++] = ' ';
            }
        } else {
            content_out[size_out++] = c;
        }
    }
    return size_out;
}

// --- Main Function ---

int main(int argc, char *argv[]) {
    // Check for help or insufficient arguments
    if (argc < 4) {
        fprintf(stderr, "flexadd version %s\n", VERSION);
        fprintf(stderr, "Usage: flexadd <disk_image_file> <host_file_path> <FLEX_FILENAME.EXT> [-t] [-y] [-z <sector_size>]\n");
        fprintf(stderr, "  -t: Enable text translation (LF->CR, space compression 0x09+count).\n");
        fprintf(stderr, "  -y: Auto-replace existing file without confirmation prompt.\n");
        fprintf(stderr, "  -z <sector_size>: Sector size in bytes (128 or 256, defaults to 256).\n");
        return 1;
    }

    // Set arguments based on usage: flexadd <disk_image> <host_file> <flex_file.ext>
    const char *disk_path     = argv[1];
    const char *host_path     = argv[2];
    const char *flex_name_ext = argv[3];

    int translate_mode = 0;
    int auto_yes = 0;
    
    // Parse optional arguments
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            translate_mode = 1;
        } else if (strcmp(argv[i], "-y") == 0) {
            auto_yes = 1;
        } else if (strcmp(argv[i], "-z") == 0 && i + 1 < argc) {
            int temp_sector_size = atoi(argv[i + 1]);
            if (temp_sector_size != SECTOR_SIZE_128 && temp_sector_size != SECTOR_SIZE_256) {
                fprintf(stderr, "Error: Sector size (-z) must be either 128 or 256 bytes.\n");
                return 1;
            }
            sector_size = temp_sector_size;
            i++; // Skip the next argument since it's the sector size value
        }
    }


    // --- 1. Open Files ---
    FILE *disk_file = fopen(disk_path, "r+b"); // Read/Write binary
    if (!disk_file) {
        perror("Error opening disk image file");
        return 1;
    }

    FILE *host_file = fopen(host_path, "rb"); // Read binary
    if (!host_file) {
        perror("Error opening host file");
        fclose(disk_file);
        return 1;
    }

    // --- 2. Read Host File Content ---
    fseek(host_file, 0, SEEK_END);
    long file_size = ftell(host_file);
    fseek(host_file, 0, SEEK_SET);

    uint8_t *raw_content = (uint8_t *)malloc(file_size + 1);
    if (!raw_content) {
        perror("Error allocating memory for file content");
        fclose(disk_file);
        fclose(host_file);
        return 1;
    }
    fread(raw_content, 1, file_size, host_file);
    fclose(host_file);

    // --- 3. Translation (if enabled) ---
    uint8_t *translated_content = NULL;
    long final_size = file_size;

    if (translate_mode) {
        // Maximum size after translation is file_size (in the worst case, size_in == size_out)
        translated_content = (uint8_t *)malloc(file_size + 1); 
        if (!translated_content) {
            perror("Error allocating memory for translation");
            free(raw_content);
            fclose(disk_file);
            return 1;
        }
        final_size = translate_text_content(raw_content, file_size, translated_content);
        free(raw_content);
        raw_content = translated_content;
    }

    // --- 4. Initialize Disk Info ---
    if (init_disk_info(disk_file) != 0) {
        free(raw_content);
        fclose(disk_file);
        return 1;
    }

    if (normalize_deleted_directory_entries(disk_file) != 0) {
        free(raw_content);
        fclose(disk_file);
        return 1;
    }

    // Convert filename once so we can check if it already exists.
    char flex_name[8], flex_ext[3];
    convert_filename(flex_name_ext, flex_name, flex_ext);

    // Existing name handling: require explicit confirmation before delete/re-add.
    uint8_t existing_sector = 0;
    int existing_index = 0;
    DIR_struct existing_entry = {0};
    int exists = find_directory_entry(disk_file, flex_name, flex_ext,
                                      &existing_sector, &existing_index, &existing_entry);
    if (exists < 0) {
        free(raw_content);
        fclose(disk_file);
        return 1;
    }
    if (exists == 1) {
        if (!auto_yes) {
            int answer = 0;
            printf("File %.8s.%.3s exists. Delete and re-add? [y/N]: ", flex_name, flex_ext);
            fflush(stdout);
            answer = getchar();
            while (answer != '\n' && getchar() != '\n') {
                /* consume rest of line */
            }

            if (!(answer == 'y' || answer == 'Y')) {
                fprintf(stderr, "Aborted: existing file was not replaced.\n");
                free(raw_content);
                fclose(disk_file);
                return 1;
            }
        } else {
            printf("File %.8s.%.3s exists. Auto-replacing due to -y.\n", flex_name, flex_ext);
        }

        if (reclaim_deleted_file_chain(disk_file, &existing_entry) != 0) {
            free(raw_content);
            fclose(disk_file);
            return 1;
        }
        if (delete_directory_entry(disk_file, existing_sector, existing_index) != 0) {
            free(raw_content);
            fclose(disk_file);
            return 1;
        }
    }

    // --- 5. Write Data and Update Directory ---
    //uint8_t  start_track, start_sector, end_track, end_sector;
    uint8_t  start_track, start_sector;
    uint16_t total_sectors;
    
    // Write data to the file chain
    printf("Writing %ld bytes (%s) to disk...\n", final_size, translate_mode ? "translated text" : "binary");

    // We pass the final_size (which might be 0 for an empty file)
    if (write_file_data(disk_file, raw_content, final_size, translate_mode, &start_track, &start_sector, &total_sectors) != 0) {
        // Rollback is skipped for this example
        fprintf(stderr, "File addition failed during data write. Disk state may be corrupted.\n");
        free(raw_content);
        fclose(disk_file);
        return 1;
    }
    
    // End track/sector of the file is the last sector written
    // This is a simplification; a full FMS would track the last sector used. 
    // Since write_file_data stops the link at (0,0) in the last sector, we use the SIR's lastFree
    // pointer to find the physical end of the block we just allocated.

    // A simple, correct way to get the end of the file chain is to read the sector
    // linked by the last allocated sector in the file, and that should be (0,0).
    // For simplicity, we assume the last sector allocated by write_file_data is the start of the 
    // new free chain head's *previous* sector. This is too messy.
    
    // Instead, we simplify: The total_sectors count and start T/S are the most critical part. 
    // FLEX systems sometimes rely on counting sectors from the start T/S.
    // For this implementation, we will assume endTrack/endSector points to the LAST sector *OF* the file.
    
    // Finding the true end sector requires walking the newly created file chain, but that's expensive.
    // Given the data sector format: Link (T,S) is (0,0) for the last sector.
    
    // The current implementation of write_file_data returns the start T/S and total_sectors.
    // We'll set endTrack/endSector to the start track/sector for single-sector files,
    // and rely on `totalSectors` for file access. 
    // This part is highly dependent on the target FMS implementation. For now, we will
    // use the simple T/S to show data has been written.
    
    // Since we don't track the last T/S allocated perfectly, we leave endT/S as 0/0 and rely on totalSectors.
    // The specification for endTrack/endSector is highly specific to FMS. We will rely on totalSectors.
    //end_track  = 0; 
    //end_sector = 0;
    
    if (total_sectors > 0) {
        printf("File data written: T%d S%d to T%d S%d, Total Sectors: %d\n", start_track, start_sector, end_track, end_sector, total_sectors);
    } else {
        printf("Empty file added (0 sectors).\n");
        // For an empty file, the start/end T/S must be 0/0
        start_track = 0; start_sector = 0;
        end_track = 0; end_sector = 0;
    }


    // --- 6. Create Directory Entry ---
    DIR_struct new_dir_entry = {0};
    
    memcpy(new_dir_entry.fileName, flex_name, 8);
    memcpy(new_dir_entry.fileExt,  flex_ext,  3);
    
    // File attributes
    //new_dir_entry.unused         = 0;
    new_dir_entry.startTrack     = start_track;
    new_dir_entry.startSector    = start_sector;

    // @FIXME: Need to set these end_track/end_sector
    new_dir_entry.endTrack       = end_track;   // Simplified
    new_dir_entry.endSector      = end_sector;  // Simplified

    new_dir_entry.totalSectorsHi = (uint8_t)((total_sectors >> 8) & 0xFF);
    new_dir_entry.totalSectorsLo = (uint8_t)(total_sectors & 0xFF);
    // Keep random flag cleared for all files.
    new_dir_entry.randomFileFlag = 0x00;

    // Date
    time_t timer;
    struct tm *tm_info;

    time(&timer);
    tm_info = localtime(&timer);

    new_dir_entry.dateMonth = (uint8_t )(tm_info->tm_mon + 1);
    new_dir_entry.dateDay   = (uint8_t ) tm_info->tm_mday;
    new_dir_entry.dateYear  = (uint8_t )(tm_info->tm_year % 100);

    fprintf(stderr, "%s.%s\n", new_dir_entry.fileName, new_dir_entry.fileExt);
    fprintf(stderr, "Start: T%d/S%d\n", new_dir_entry.startTrack, new_dir_entry.startSector);
    fprintf(stderr, "End:   T%d/S%d\n", new_dir_entry.endTrack, new_dir_entry.endSector);
    fprintf(stderr, "Size:  %02x%02x (%d)\n\n", new_dir_entry.totalSectorsHi,
            new_dir_entry.totalSectorsLo,
            ((new_dir_entry.totalSectorsHi << 8) + (new_dir_entry.totalSectorsLo)));

    fprintf(stderr, "%2d/%2d/%2d\n", (tm_info->tm_mon + 1), tm_info->tm_mday,
            (tm_info->tm_year % 100));
    fprintf(stderr, "%2d/%2d/%2d (dec)\n", new_dir_entry.dateMonth, new_dir_entry.dateDay,
            new_dir_entry.dateYear);
    fprintf(stderr, "%2x/%2x/%2x (hex)\n", new_dir_entry.dateMonth, new_dir_entry.dateDay,
            new_dir_entry.dateYear);

    // --- 7. Write Directory Entry ---
    if (write_directory_entry(disk_file, &new_dir_entry) != 0) {
        fprintf(stderr, "Error: Failed to create directory entry.\n");
        // Data is written, but not accessible.
        free(raw_content);
        fclose(disk_file);
        return 1;
    }

    // --- 8. Cleanup and Finalize ---
    free(raw_content);
    if (SIR_buffer) {
        free(SIR_buffer);
    }
    fclose(disk_file);
    printf("✅ Success! File '%s' added to disk image '%s' (sector size: %d bytes).\n", flex_name_ext, disk_path, sector_size);

    return 0;
}
