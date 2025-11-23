#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

// Semantic Versioning
#define VERSION "1.0.2"

// --- Flex Disk Structures ---

// SECTOR size is 256 bytes (4 bytes header + 252 bytes data)
#define FLEX_SECTOR_SIZE 256
// Assumed standard 18 sectors per track for Flex
#define SECTORS_PER_TRACK 18
#define DISK_BLOCK_SIZE FLEX_SECTOR_SIZE

typedef struct{
    uint8_t  next_track;      // Points to next track in the link (Offset +0)
    uint8_t  next_sector;     // Points to next sector in the link (Offset +1)
    uint16_t File_logical;    // (Offset +2, +3)
    uint8_t  data[252];       // (Offset +4 to +255)
} SECTOR;

// SIR_struct and DIR_struct are included for context
typedef struct{
    uint8_t  volLabel[11];
    uint16_t volNumber;
    uint8_t  firstFreeTrack;
    uint8_t  firstFreeSector;
    uint8_t  lastFreeTrack;
    uint8_t  lastFreeSector;
    uint16_t freeSectors;
    uint8_t  dateMonth;
    uint8_t  dateDay;
    uint8_t  dateYear;
    uint8_t  endTrack;
    uint8_t  endSector;
} SIR_struct;

typedef struct{
    uint8_t  fileName[8];
    uint8_t  fileExt[3];
    uint16_t unused1;
    uint8_t  startTrack;
    uint8_t  startSector;
    uint8_t  endTrack;
    uint8_t  endSector;
    uint16_t totalSectors;
    uint8_t  randomFileFlag;
    uint8_t  unused2;
    uint8_t  dateMonth;
    uint8_t  dateDay;
    uint8_t  dateYear;
} DIR_struct;

// --- Global Variables ---
FILE *disk_file = NULL;
long file_size = 0;
long current_offset = 0; // Current byte offset for display (must be aligned to DISK_BLOCK_SIZE)
int rows, cols;

// --- Function Prototypes ---
void init_curses();
void close_curses();
void draw_hex_editor();
void display_help();
void handle_command(const char *cmd);
void goto_offset(long offset);
void goto_track_sector(int track, int sector);
void update_status_line(const SECTOR *sector_data);
long track_sector_to_offset(int track, int sector);
void offset_to_track_sector(long offset, int *track_out, int *sector_out);

// --- Core Logic ---

/**
 * @brief Initializes ncurses environment.
 */
void init_curses() {
    initscr();              // Start curses mode
    cbreak();               // Line buffering disabled, pass on characters immediately
    noecho();               // Don't echo input characters
    keypad(stdscr, TRUE);   // Enable special keys (like Page Up/Down)
    getmaxyx(stdscr, rows, cols); // Get screen dimensions
}

/**
 * @brief Cleans up ncurses environment.
 */
void close_curses() {
    endwin(); // End curses mode
}

/**
 * @brief Converts track and sector numbers to a file offset.
 */
long track_sector_to_offset(int track, int sector) {
    if (track < 0 || sector < 1 || sector > SECTORS_PER_TRACK) {
        return -1;
    }
    // offset = (Track * SECTORS_PER_TRACK + (Sector - 1)) * FLEX_SECTOR_SIZE
    return ((long)track * SECTORS_PER_TRACK + (sector - 1)) * FLEX_SECTOR_SIZE;
}

/**
 * @brief Converts a file offset to track and sector numbers.
 */
void offset_to_track_sector(long offset, int *track_out, int *sector_out) {
    long block_index = offset / FLEX_SECTOR_SIZE;
    *track_out = (int)(block_index / SECTORS_PER_TRACK);
    *sector_out = (int)(block_index % SECTORS_PER_TRACK) + 1; // 1-based sector
}


/**
 * @brief Draws the hex editor view based on the current_offset.
 */
void draw_hex_editor() {
    clear();
    // Use a temporary byte array to read the whole sector block (256 bytes)
    // This allows us to treat the header fields (next_track, etc.)
    // as part of the data block for display purposes.
    uint8_t sector_block[DISK_BLOCK_SIZE];
    SECTOR *current_sector_ptr = (SECTOR *)sector_block;

    // Read a full sector (256 bytes) starting at current_offset
    fseek(disk_file, current_offset, SEEK_SET);
    size_t bytes_read = fread(sector_block, 1, DISK_BLOCK_SIZE, disk_file);

    if (bytes_read != DISK_BLOCK_SIZE) {
        mvprintw(0, 0, "Error reading sector at offset %06lX. Read %zu/%d bytes.",
                 current_offset, bytes_read, DISK_BLOCK_SIZE);
        update_status_line(NULL);
        refresh();
        return;
    }

    // Determine how many lines of hex to draw (16 bytes per line)
    // 256 bytes / 16 bytes/line = 16 lines
    int max_data_lines = DISK_BLOCK_SIZE / 16;
    int display_lines = (rows - 3 > max_data_lines) ? max_data_lines : rows - 3; // Leave space for header/footer

    // --- Header ---
    mvprintw(0, 0, " Addr  00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F   0123456789ABCDEF");
    mvprintw(1, 0, "------ ------------------------------------------------  ------------------");

    // --- Hex/ASCII Dump ---
    for (int i = 0; i < display_lines; i++) {
        long addr = current_offset + (i * 16);
        int line_start_index = i * 16;
        char hex_line[50] = {0}; // Enough space for 16 * 3 + 1 space + null
        char ascii_line[17] = {0}; // 16 chars + null terminator
        int hex_pos = 0;

        // Iterate over 16 bytes for the current line
        for (int j = 0; j < 16; j++) {
            int data_index = line_start_index + j;

            if (data_index < DISK_BLOCK_SIZE) {
                // Hex part
                hex_pos += sprintf(hex_line + hex_pos, "%02x%s",
                                   sector_block[data_index], (j == 7) ? "  " : " ");

                // ASCII part
                char c = sector_block[data_index];
                ascii_line[j] = isprint(c) ? c : '.';
            } else {
                // Should not happen if DISK_BLOCK_SIZE is 256 and i*16 < 256
                hex_pos += sprintf(hex_line + hex_pos, "   ");
                ascii_line[j] = ' ';
            }
        }
        ascii_line[16] = '\0'; // Null-terminate ASCII string

        // Print the line
        mvprintw(2 + i, 0, "%06lX %s |%s|", addr, hex_line, ascii_line);
    }

    // --- Status Line ---
    update_status_line(current_sector_ptr);

    // --- Command Prompt ---
    mvprintw(rows - 1, 0, "> ");

    move(rows - 1, 2); // Put cursor at the prompt
    refresh();
}

/**
 * @brief Updates the status line with track/sector info.
 */
void update_status_line(const SECTOR *sector_data) {
    int track, sector;
    offset_to_track_sector(current_offset, &track, &sector);

    if (sector_data) {
        mvprintw(rows - 2, 0,
                 "Track %d Sector: %d Next_t: %d Next_s: %d (Offset: %06lX) | Version: %s",
                 track, sector,
                 sector_data->next_track, sector_data->next_sector,
                 current_offset, VERSION);
    } else {
        mvprintw(rows - 2, 0, "Track %d Sector: %d (Offset: %06lX) - Error Reading Data | Version: %s",
                 track, sector, current_offset, VERSION);
    }
    clrtoeol(); // Clear to end of line
}

/**
 * @brief Handles the 'goto' commands.
 */
void handle_command(const char *cmd) {
    if (cmd[0] == 'q') {
        // Handled in main loop
    } else if (cmd[0] == 'h') {
        display_help();
    } else if (cmd[0] == 'g') {
        long offset;
        if (sscanf(cmd + 1, "%li", &offset) == 1 || sscanf(cmd + 1, "0x%lx", &offset) == 1) {
            goto_offset(offset);
        } else {
            mvprintw(rows - 1, 2, "Invalid offset format. Use: g <dec> or g <0xhex>");
            clrtoeol();
            getch();
        }
    } else if (cmd[0] == 't') {
        int track, sector;
        if (sscanf(cmd + 1, "%d %d", &track, &sector) == 2) {
            goto_track_sector(track, sector);
        } else {
            mvprintw(rows - 1, 2, "Invalid track/sector format. Use: t <track> <sector>");
            clrtoeol();
            getch();
        }
    }
}

/**
 * @brief Pages down to the next sector.
 */
void page_down() {
    long new_offset = current_offset + FLEX_SECTOR_SIZE;
    if (new_offset < file_size) {
        current_offset = new_offset;
    } else {
        // Stay at the last possible sector
        current_offset = file_size - FLEX_SECTOR_SIZE;
        if (current_offset < 0) current_offset = 0;
    }
}

/**
 * @brief Pages up to the previous sector.
 */
void page_up() {
    current_offset -= FLEX_SECTOR_SIZE;
    if (current_offset < 0) {
        current_offset = 0;
    }
}

/**
 * @brief Sets the display offset to the requested byte offset.
 */
void goto_offset(long offset) {
    long new_offset = offset;
    // Clamp to sector boundary
    new_offset = (new_offset / FLEX_SECTOR_SIZE) * FLEX_SECTOR_SIZE;

    // Clamp to file size
    if (new_offset < 0) new_offset = 0;
    if (new_offset >= file_size) {
        new_offset = file_size - FLEX_SECTOR_SIZE;
        if (new_offset < 0) new_offset = 0;
    }

    current_offset = new_offset;
}

/**
 * @brief Sets the display offset to the requested track and sector.
 */
void goto_track_sector(int track, int sector) {
    long new_offset = track_sector_to_offset(track, sector);
    if (new_offset >= 0 && new_offset < file_size) {
        current_offset = new_offset;
    } else {
        mvprintw(rows - 1, 2, "Invalid Track/Sector location: T%d S%d", track, sector);
        clrtoeol();
        getch(); // Wait for key press to clear message
    }
}

/**
 * @brief Displays the help menu in the main window.
 */
void display_help() {
    clear();
    mvprintw(0, 0, "--- Flex Disk Hex Editor Help (v%s) ---", VERSION);
    mvprintw(2, 0, "Navigation Keys:");
    mvprintw(3, 2, "Page Up/KEY_PPAGE: Go to previous sector");
    mvprintw(4, 2, "Page Down/KEY_NPAGE: Go to next sector");
    mvprintw(6, 0, "Command Prompt (at '>'):");
    mvprintw(7, 2, "h - Display this help screen");
    mvprintw(8, 2, "q - Quit the program");
    // FIX: Line 319 was mvptintw, corrected to mvprintw
    mvprintw(9, 2, "g <offset> - Go to byte offset (e.g., 'g 1024' or 'g 0x400')");
    mvprintw(10, 2, "t <track> <sector> - Go to track and sector (e.g., 't 1 1')");
    mvprintw(12, 0, "Press any key to return to the editor.");
    refresh();
    getch(); // Wait for user input
}

/**
 * @brief Main function.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_image_file>\n", argv[0]);
        return 1;
    }

    disk_file = fopen(argv[1], "rb");
    if (!disk_file) {
        perror("Could not open disk image file");
        return 1;
    }

    // Get file size
    fseek(disk_file, 0, SEEK_END);
    file_size = ftell(disk_file);
    fseek(disk_file, 0, SEEK_SET);

    if (file_size == 0) {
        fprintf(stderr, "Disk image file is empty.\n");
        fclose(disk_file);
        return 1;
    }

    // Ensure initial offset is sector-aligned
    current_offset = 0;

    init_curses();

    int ch;
    int running = 1;
    char command_buffer[80];

    while (running) {
        draw_hex_editor();
        
        // Wait for input
        ch = getch();

        if (ch == KEY_NPAGE) {
            page_down();
        } else if (ch == KEY_PPAGE) {
            page_up();
        } else if (ch == 'q') {
            running = 0;
        } else if (ch == 'h') {
            display_help();
        } else if (ch == 'g' || ch == 't') {
            // Enter command mode for 'g' or 't'
            echo(); // Enable echo for command input
            curs_set(1); // Show cursor
            mvprintw(rows - 1, 0, "> %c", ch);
            clrtoeol();
            
            // Read command arguments
            getnstr(command_buffer, sizeof(command_buffer) - 2); 
            
            noecho(); // Disable echo
            curs_set(0); // Hide cursor
            
            // Prepend the initial command char and handle it
            char full_cmd[80];
            snprintf(full_cmd, sizeof(full_cmd), "%c%s", ch, command_buffer);
            handle_command(full_cmd);
        }
    }

    close_curses();
    fclose(disk_file);

    return 0;
}
