#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h> 

// --- Constants and Definitions ---
#ifndef uint8_t
typedef unsigned char uint8_t;
#endif

#define PROGRAM_VERSION     "1.0.15" // Version

extern void print_usage(const char *prog_name);
extern void write_sector(FILE *disk_file, uint16_t track, uint8_t sector, uint8_t next_track,
                         uint8_t next_sector);
extern void write_sir_sector(FILE *disk_file, const char *vol_name, uint16_t tracks,
                             uint8_t sectors_per_track, uint16_t vol_number,
                             const struct tm *current_time);

#ifndef NJC
#include "flexfs.h"
#else
#define SECTOR_SIZE         256
#define SIR_SIZE            24      
#define SIR_OFFSET          16      
#define MAX_VOL_NAME_LEN    11      
#define DEFAULT_VOL_NUMBER  1       
#define MAX_TRACKS          256     // Maximum tracks allowed (0-255)
#define MAX_SECTORS         255     // Maximum sectors allowed (1-255)
#define MIN_SECTORS         5       // Minimum sectors required (1-4 special, 5+ directory)

// 000200 00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
// 000210 46 6c 65 78 20 48 44 00  00 00 00 00 01 01 01 ff  |Flex HD.........|
// 000220 ff ff 01 0b 12 19 ff ff  00 00 00 00 00 00 00 00  |................|

// Define the SIR structure 
typedef struct {
    uint8_t  volLabel[MAX_VOL_NAME_LEN]; 
    uint16_t volNumber;
    uint8_t  firstFreeTrack;  
    uint8_t  firstFreeSector; 
    uint8_t  lastFreeTrack;   
    uint8_t  lastFreeSector;  
    uint8_t  freeSectorsHi;   
    uint8_t  freeSectorsLo;   
    uint8_t  dateMonth;       
    uint8_t  dateDay;         
    uint8_t  dateYear;        
    uint8_t  endTrack;        
    uint8_t  endSector;       
} SIR_struct;
#endif

// Function to display usage and version
void print_usage(const char *prog_name) {
    fprintf(stderr, "flexdsk version %s\n", PROGRAM_VERSION);
    fprintf(stderr, "Usage: %s <output_filename> -v <volume_name> -t <num_tracks> -s <num_sectors> [-n <volume_number>] [-b <boot_loader_file>]\n", prog_name);
    fprintf(stderr, "\nRequired Options:\n");
    fprintf(stderr, "  -v <volume_name> : The disk volume label (max %d characters).\n", MAX_VOL_NAME_LEN);
    fprintf(stderr, "  -t <num_tracks>  : Number of tracks (1-%d).\n", MAX_TRACKS);
    fprintf(stderr, "  -s <num_sectors> : Number of sectors per track (%d-%d).\n", MIN_SECTORS, MAX_SECTORS);
    fprintf(stderr, "\nOptional Options:\n");
    fprintf(stderr, "  -n <volume_number>: The disk volume number (1-255, defaults to %d).\n", DEFAULT_VOL_NUMBER);
    fprintf(stderr, "  -b <boot_loader_file>: Path to a file to load into T0, S1 and S2 (512 bytes).\n");
}

// Function to write a single sector of 256 bytes
void write_sector(FILE *disk_file, uint16_t track, uint8_t sector, uint8_t next_track, uint8_t next_sector)
{
    uint8_t sector_data[SECTOR_SIZE] = {0};

    // Bytes 0-1 Link to the next sector (Req 1.2)
    // Exclude special sectors T0, S1, S2, S3, S4 (Req 1.1)
    if (track != 0 || (sector > 4 && sector <= 255)) {
        sector_data[0] = next_track;
        sector_data[1] = next_sector;
    }

    fwrite(sector_data, 1, SECTOR_SIZE, disk_file);
}

/*
 * Print FLEX volume label
 */
void printVolumeLabel(uint8_t *s){
    int i;

    for(i = 0; i < 11; i++)
        if(s[i] > 0x1f && s[i] < 0x7f)
            fprintf(stderr, "%c",s[i]);
}

// Function to write the System Information Record (SIR) sector (T0, S3)
void write_sir_sector(FILE *disk_file, const char *vol_name, uint16_t tracks, uint8_t sectors_per_track, uint16_t vol_number, const struct tm *current_time) {
    uint8_t sir_sector_data[SECTOR_SIZE] = {0};
    
    // Calculate free sectors and start/end of free chain
    int total_sectors      = (int)tracks * (int)sectors_per_track;
    int used_sectors_on_t0 = sectors_per_track;
    // @FIXME: This should be total sector - dir sectors == free sectors
    int free_sectors       = total_sectors - used_sectors_on_t0;

    // First free sector (T1, S1 or T0, S(n+1))
    uint8_t first_free_track  = 1;
    uint8_t first_free_sector = 1;

    // Last physical track/sector is tracks-1 and sectors_per_track
    uint16_t last_physical_track  = tracks - 1;
    uint8_t  last_physical_sector = sectors_per_track;
    
    // Initialize the 24-byte SIR structure
    SIR_struct sir_struct = {0};

    // 1. volLabel (11 bytes) 0-10
    //memset(sir_struct.volLabel, ' ', MAX_VOL_NAME_LEN); 
    strncpy((char *)sir_struct.volLabel, vol_name, MAX_VOL_NAME_LEN);

    // 2. volNumber (2 bytes) 11-12
    sir_struct.volNumberHi = (vol_number >> 8) & 0xFF;
    sir_struct.volNumberLo = vol_number & 0xFF;

    // 3. firstFreeTrack/Sector (2 bytes) 13-14
    sir_struct.firstFreeTrack  = first_free_track;
    sir_struct.firstFreeSector = first_free_sector;

    // 4. lastFreeTrack/Sector (2 bytes) 15-16
    // The last sector of the disk is the last sector of the free chain
    sir_struct.lastFreeTrack  = last_physical_track;
    sir_struct.lastFreeSector = last_physical_sector;

    // 5. freeSectorsHi/Lo (2 bytes) 17-18
    sir_struct.freeSectorsHi = (free_sectors >> 8) & 0xFF;
    sir_struct.freeSectorsLo = free_sectors & 0xFF;

    // 6. Date (3 bytes) - Current Date 19-21
    sir_struct.dateMonth = (uint8_t)(current_time->tm_mon + 1);
    sir_struct.dateDay   = (uint8_t)current_time->tm_mday;
    sir_struct.dateYear  = (uint8_t)(current_time->tm_year % 100);

    // 7. endTrack/Sector (2 bytes) 22-23
    sir_struct.endTrack  = (uint8_t)last_physical_track;
    sir_struct.endSector = (uint8_t)last_physical_sector;

    fprintf(stderr, "\nTracks:  %d\n", tracks);
    fprintf(stderr, "Sectors: %d\n", sectors_per_track);
    fprintf(stderr, "Total:   %d\n", total_sectors);
    fprintf(stderr, "Free:    %d\n\n", free_sectors);

    fprintf(stderr, "%u tracks, %u sectors/track\n", sir_struct.endTrack, sir_struct.endSector);
    fprintf(stderr, "Struct size      %02d-%02d\n", sizeof(sir_struct), SIR_SIZE);
    fprintf(stderr, "\nVolume label     "); printVolumeLabel(sir_struct.volLabel); fprintf(stderr, "\n");
    fprintf(stderr, "Volume number    %02x%02x(%04x)\n",sir_struct.volNumberHi, sir_struct.volNumberLo, vol_number);
    fprintf(stderr, "Free area        t%u s%u - t%u s%u\n",
            sir_struct.firstFreeTrack, sir_struct.firstFreeSector,
            sir_struct.lastFreeTrack, sir_struct.lastFreeSector);
    fprintf(stderr, "Free sectors     %u\n",((sir_struct.freeSectorsHi << 8) + sir_struct.freeSectorsLo));
    fprintf(stderr, "End sector       t%u s%u\n", sir_struct.endTrack, sir_struct.endSector);
    fprintf(stderr, "Creation date    %02d-%02d-%02d\n\n",
            sir_struct.dateYear,sir_struct.dateMonth,sir_struct.dateDay);

    // Copy the 24-byte SIR structure into the sector data, starting at offset 16 (Byte 17)
    memcpy(sir_sector_data + SIR_OFFSET, &sir_struct, SIR_SIZE); 

    int i = 0;
    for( i = 0; i < SIR_SIZE; i++) {
        fprintf(stderr, "%02x ", sir_sector_data[i+SIR_OFFSET]);
    }
    fprintf(stderr, "\n");
    
    fwrite(sir_sector_data, 1, SECTOR_SIZE, disk_file);
}

// Main function
int main(int argc, char *argv[]) {
    // Required parameters, initialized to invalid states
    char    *vol_name_arg     = NULL;
    int     num_tracks        = -1;
    int     num_sectors       = -1;
    uint16_t vol_number        = DEFAULT_VOL_NUMBER;
    char    *boot_loader_file = NULL;
    char    *output_filename  = NULL;
    
    // Variables for getopt
    int opt;
    
    // Check for output filename (first non-option argument)
    if (argc < 2 || argv[1][0] == '-') {
        print_usage(argv[0]);
        return 1;
    }
    
    output_filename = argv[1];

    // Reset optind for getopt to parse arguments starting from argv[2]
    optind = 2; 

    // Parse command line options using getopt
    while ((opt = getopt(argc, argv, "v:t:s:b:n:e:")) != -1) {
        switch (opt) {
            case 'v':
                vol_name_arg = optarg;
                break;
            case 't':
                num_tracks = atoi(optarg);
                break;
            case 's':
                num_sectors = atoi(optarg);
                break;
            case 'b':
                boot_loader_file = optarg;
                break;
            case 'n':
                {
                    int temp_vol_num = atoi(optarg);
                    if (temp_vol_num < 1 || temp_vol_num > 64 * 1024) {
                        fprintf(stderr, "Error: Volume number (-n) must be between 0 and 65535.\n");
                        return 1;
                    }
                    vol_number = (uint16_t)temp_vol_num;
                }
                break;
            case '?':
                print_usage(argv[0]);
                return 1;
            case 'e':
                fprintf(stderr, "# of entries not enabled\n");
            default:
                break;
        }
    }

    // --- 1. Final Argument Validation ---

    // Validate presence of required arguments
    if (vol_name_arg == NULL || num_tracks == -1 || num_sectors == -1) {
        fprintf(stderr, "Error: Missing required argument(s) (-v, -t, or -s).\n");
        print_usage(argv[0]);
        return 1;
    }

    // Validate volume name length
    if (strlen(vol_name_arg) > MAX_VOL_NAME_LEN) {
        fprintf(stderr, "Error: Volume name string must be no more than %d characters long.\n", MAX_VOL_NAME_LEN);
        return 1;
    }

    // Validate track/sector counts (Tracks: 1 to 256)
    if (num_tracks < 1 || num_tracks > MAX_TRACKS) {
        fprintf(stderr, "Error: Number of tracks (-t) must be between 1 and %d.\n", MAX_TRACKS);
        return 1;
    }
    // Validate sector count (Sectors: 5 to 255)
    if (num_sectors < MIN_SECTORS || num_sectors > MAX_SECTORS) {
        fprintf(stderr, "Error: Number of sectors (-s) must be between %d and %d.\n", MIN_SECTORS, MAX_SECTORS);
        return 1;
    }

    // --- 2. Get Current Date ---
    time_t timer;
    struct tm *tm_info;
    time(&timer);
    tm_info = localtime(&timer);

    // --- 3. Open Disk Image File ---
    FILE *disk_file = fopen(output_filename, "wb");
    if (disk_file == NULL) {
        perror("Error opening output disk file");
        return 1;
    }

    printf("flexdsk version %s: Creating disk image '%s'...\n", PROGRAM_VERSION, output_filename);

    // --- 4. Populate Disk Image ---
    
    // T0, S1 & S2 (Boot Loader)
    if (boot_loader_file) {
        FILE *boot_file = fopen(boot_loader_file, "rb");
        uint8_t boot_data[SECTOR_SIZE] = {0};

        if (boot_file) {
            fread(boot_data, 1, SECTOR_SIZE, boot_file);
            fwrite(boot_data, 1, SECTOR_SIZE, disk_file);
            
            memset(boot_data, 0, SECTOR_SIZE);
            fread(boot_data, 1, SECTOR_SIZE, boot_file);
            fwrite(boot_data, 1, SECTOR_SIZE, disk_file);
            
            fclose(boot_file);
        } else {
            fprintf(stderr, "Warning: Error opening boot loader file '%s'. Writing empty sectors for T0, S1 & S2.\n", boot_loader_file);
            uint8_t empty_sector[SECTOR_SIZE] = {0};
            fwrite(empty_sector, 1, SECTOR_SIZE, disk_file);
            fwrite(empty_sector, 1, SECTOR_SIZE, disk_file);
        }
    } else {
        uint8_t empty_sector[SECTOR_SIZE] = {0};
        fwrite(empty_sector, 1, SECTOR_SIZE, disk_file); // T0, S1
        fwrite(empty_sector, 1, SECTOR_SIZE, disk_file); // T0, S2
    }
    
    fprintf(stderr, "NTracks: %d\n", num_tracks);

    // T0, S3 (SIR)
    // Note: The maximum track number is (num_tracks - 1), which fits in a uint8_t (0-255).
    write_sir_sector(disk_file, vol_name_arg, (uint16_t)num_tracks, (uint8_t)num_sectors, (uint16_t)vol_number, tm_info);

    // T0, S4 (Unused)
    write_sector(disk_file, 0, 4, 0, 0); 
    
    // T0, S5 up to T0, Sn (Directory - zeroed)
    for (int s = 5; s <= num_sectors; ++s) {
        write_sector(disk_file, 0, (uint8_t)s, 0, s+1);
    }
    
    // Remaining Free Chain Sectors (T1, S1 onwards)
    // Loop up to the total number of tracks (1 to MAX_TRACKS)
    for (int t = 1; t < num_tracks; ++t) {
        for (int s = 1; s <= num_sectors; ++s) {
            uint8_t next_track  = (uint8_t)t;
            uint8_t next_sector = (uint8_t)s + 1;

            if (s == num_sectors) {
                // Last sector of the current track: link to next track, sector 1
                next_track = (uint8_t)t + 1; 
                next_sector = 1;
            }

            if (t == num_tracks - 1 && s == num_sectors) {
                // Last sector of the disk: link to (0, 0)
                next_track  = 0;
                next_sector = 0;
            }
            
            write_sector(disk_file, (uint8_t)t, (uint8_t)s, next_track, next_sector);
        }
    }
    
    // 5. Cleanup
    fclose(disk_file);
    
    // 6. Output Summary
    printf("✅ Success! Disk image details:\n");
    printf("   Program Version: %s\n", PROGRAM_VERSION);
    printf("   File: %s\n", output_filename);
    printf("   Volume: %s (Number: %d)\n", vol_name_arg, vol_number);
    printf("   Size: %d tracks (0-%d), %d sectors/track (Total %ld bytes)\n", num_tracks, num_tracks - 1, num_sectors, (long)num_tracks * num_sectors * SECTOR_SIZE);
    printf("   Creation Date: %02d/%02d/%d\n", tm_info->tm_mon + 1, tm_info->tm_mday, tm_info->tm_year % 100);

    return 0;
}

