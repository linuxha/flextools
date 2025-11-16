#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h> 

// --- Constants and Definitions ---
typedef unsigned char u_byte;

#define PROGRAM_VERSION     "1.0.8" // Version updated to 1.0.8
#define SECTOR_SIZE         256
#define SIR_SIZE            24      
#define SIR_OFFSET          16      
#define MAX_VOL_NAME_LEN    11      
#define DEFAULT_VOL_NUMBER  1       
#define MAX_TRACKS          256     // Maximum tracks allowed (0-255)
#define MAX_SECTORS         255     // Maximum sectors allowed (1-255)
#define MIN_SECTORS         5       // Minimum sectors required (1-4 special, 5+ directory)

// Define the SIR structure 
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

// Function to display usage and version
void print_usage(const char *prog_name) {
    fprintf(stderr, "flexdsk version %s\n", PROGRAM_VERSION);
    fprintf(stderr, "Usage: %s <output_filename> -v <volume_name> -t <num_tracks> -s <num_sectors> [-n <volume_number>] [-b <boot_loader_file>]\n", prog_name);
    fprintf(stderr, "\nRequired Options:\n");
    fprintf(stderr, "  -v <volume_name> : The disk volume label (max %d characters).\n", MAX_VOL_NAME_LEN);
    fprintf(stderr, "  -t <num_tracks>  : Number of tracks (1-%d, starting at 0).\n", MAX_TRACKS);
    fprintf(stderr, "  -s <num_sectors> : Number of sectors per track (%d-%d).\n", MIN_SECTORS, MAX_SECTORS);
    fprintf(stderr, "\nOptional Options:\n");
    fprintf(stderr, "  -n <volume_number>: The disk volume number (1-255, defaults to %d).\n", DEFAULT_VOL_NUMBER);
    fprintf(stderr, "  -b <boot_loader_file>: Path to a file to load into T0, S1 and S2 (512 bytes).\n");
}

// Function to write a single sector of 256 bytes
void write_sector(FILE *disk_file, u_byte track, u_byte sector, u_byte next_track, u_byte next_sector) {
    u_byte sector_data[SECTOR_SIZE] = {0};

    // Bytes 0-1 Link to the next sector (Req 1.2)
    // Exclude special sectors T0, S1, S2, S3, S4 (Req 1.1)
    if (track != 0 || (sector > 4 && sector <= 255)) {
        sector_data[0] = next_track;
        sector_data[1] = next_sector;
    }

    fwrite(sector_data, 1, SECTOR_SIZE, disk_file);
}

// Function to write the System Information Record (SIR) sector (T0, S3)
void write_sir_sector(FILE *disk_file, const char *vol_name, u_byte tracks, u_byte sectors_per_track, u_byte vol_number, const struct tm *current_time) {
    u_byte sir_sector_data[SECTOR_SIZE] = {0};
    
    // Calculate free sectors and start/end of free chain
    int total_sectors = (int)tracks * (int)sectors_per_track;
    int used_sectors_on_t0 = sectors_per_track;
    int free_sectors = total_sectors - used_sectors_on_t0;

    // First free sector (T1, S1 or T0, S(n+1))
    u_byte first_free_track = (sectors_per_track + 1 > sectors_per_track) ? 1 : 0;
    u_byte first_free_sector = (sectors_per_track + 1 > sectors_per_track) ? 1 : (sectors_per_track + 1);
    
    // Last physical track/sector is tracks-1 and sectors_per_track
    u_byte last_physical_track = tracks - 1;
    u_byte last_physical_sector = sectors_per_track;
    
    // Initialize the 24-byte SIR structure
    SIR_struct sir_struct = {0};

    // 1. volLabel (11 bytes)
    memset(sir_struct.volLabel, ' ', MAX_VOL_NAME_LEN); 
    strncpy((char *)sir_struct.volLabel, vol_name, MAX_VOL_NAME_LEN);

    // 2. volNumberHi/Lo (2 bytes)
    sir_struct.volNumberHi = 0x00;
    sir_struct.volNumberLo = vol_number; 

    // 3. firstFreeTrack/Sector (2 bytes)
    sir_struct.firstFreeTrack = first_free_track;
    sir_struct.firstFreeSector = first_free_sector;

    // 4. lastFreeTrack/Sector (2 bytes)
    // The last sector of the disk is the last sector of the free chain
    sir_struct.lastFreeTrack = last_physical_track;
    sir_struct.lastFreeSector = last_physical_sector;

    // 5. freeSectorsHi/Lo (2 bytes)
    sir_struct.freeSectorsHi = (free_sectors >> 8) & 0xFF; 
    sir_struct.freeSectorsLo = free_sectors & 0xFF;        

    // 6. Date (3 bytes) - Current Date
    sir_struct.dateMonth = (u_byte)(current_time->tm_mon + 1);
    sir_struct.dateDay = (u_byte)current_time->tm_mday;
    sir_struct.dateYear = (u_byte)(current_time->tm_year % 100);

    // 7. endTrack/Sector (2 bytes)
    sir_struct.endTrack = last_physical_track;
    sir_struct.endSector = last_physical_sector;

    // Copy the 24-byte SIR structure into the sector data, starting at offset 16 (Byte 17)
    memcpy(sir_sector_data + SIR_OFFSET, &sir_struct, SIR_SIZE); 
    
    fwrite(sir_sector_data, 1, SECTOR_SIZE, disk_file);
}

// Main function
int main(int argc, char *argv[]) {
    // Required parameters, initialized to invalid states
    char *vol_name_arg = NULL;
    int num_tracks = -1;
    int num_sectors = -1;
    u_byte vol_number = DEFAULT_VOL_NUMBER;
    char *boot_loader_file = NULL;
    char *output_filename = NULL;
    
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
    while ((opt = getopt(argc, argv, "v:t:s:b:n:")) != -1) {
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
                    if (temp_vol_num < 1 || temp_vol_num > 255) {
                        fprintf(stderr, "Error: Volume number (-n) must be between 1 and 255.\n");
                        return 1;
                    }
                    vol_number = (u_byte)temp_vol_num;
                }
                break;
            case '?':
                print_usage(argv[0]);
                return 1;
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
        u_byte boot_data[SECTOR_SIZE] = {0};

        if (boot_file) {
            fread(boot_data, 1, SECTOR_SIZE, boot_file);
            fwrite(boot_data, 1, SECTOR_SIZE, disk_file);
            
            memset(boot_data, 0, SECTOR_SIZE);
            fread(boot_data, 1, SECTOR_SIZE, boot_file);
            fwrite(boot_data, 1, SECTOR_SIZE, disk_file);
            
            fclose(boot_file);
        } else {
            fprintf(stderr, "Warning: Error opening boot loader file '%s'. Writing empty sectors for T0, S1 & S2.\n", boot_loader_file);
            u_byte empty_sector[SECTOR_SIZE] = {0};
            fwrite(empty_sector, 1, SECTOR_SIZE, disk_file);
            fwrite(empty_sector, 1, SECTOR_SIZE, disk_file);
        }
    } else {
        u_byte empty_sector[SECTOR_SIZE] = {0};
        fwrite(empty_sector, 1, SECTOR_SIZE, disk_file); // T0, S1
        fwrite(empty_sector, 1, SECTOR_SIZE, disk_file); // T0, S2
    }
    
    // T0, S3 (SIR)
    // Note: The maximum track number is (num_tracks - 1), which fits in a u_byte (0-255).
    write_sir_sector(disk_file, vol_name_arg, (u_byte)num_tracks, (u_byte)num_sectors, vol_number, tm_info);

    // T0, S4 (Unused)
    write_sector(disk_file, 0, 4, 0, 0); 
    
    // T0, S5 up to T0, Sn (Directory - zeroed)
    for (int s = 5; s <= num_sectors; ++s) {
        write_sector(disk_file, 0, (u_byte)s, 0, 0);
    }
    
    // Remaining Free Chain Sectors (T1, S1 onwards)
    // Loop up to the total number of tracks (1 to MAX_TRACKS)
    for (int t = 1; t < num_tracks; ++t) {
        for (int s = 1; s <= num_sectors; ++s) {
            u_byte next_track = (u_byte)t;
            u_byte next_sector = (u_byte)s + 1;

            if (s == num_sectors) {
                // Last sector of the current track: link to next track, sector 1
                next_track = (u_byte)t + 1; 
                next_sector = 1;
            }

            if (t == num_tracks - 1 && s == num_sectors) {
                // Last sector of the disk: link to (0, 0)
                next_track = 0;
                next_sector = 0;
            }
            
            write_sector(disk_file, (u_byte)t, (u_byte)s, next_track, next_sector);
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
