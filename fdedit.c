#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include <unistd.h>
#include <sys/types.h>

// Semantic Versioning
#define VERSION "1.1.13" 

// --- Flex Disk Structures and Constants ---
#define FLEX_SECTOR_SIZE 256
#define SECTORS_PER_TRACK 18
#define DISK_BLOCK_SIZE FLEX_SECTOR_SIZE
#define HEX_FIELD 0
#define ASCII_FIELD 1
#define BYTES_PER_LINE 16

// Sector structure for data interpretation
typedef struct{
    uint8_t  next_track;      // Points to next track in the link (Offset +0)
    uint8_t  next_sector;     // Points to next sector in the link (Offset +1)
    uint16_t File_logical;    // (Offset +2, +3)
    uint8_t  data[252];       // (Offset +4 to +255)
} SECTOR;

// Global State
char *file_path = NULL;
long file_size = 0;
long current_offset = 0; 
int rows, cols;
int unsaved_changes = 0; 

// --- New Global Memory Buffer ---
uint8_t *disk_memory = NULL;

// mode = 1 (View), mode = 0 (Edit)
int mode = 1; 

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
int edit_sector(uint8_t *sector_block);
int save_file(int save_as);
int prompt_save_on_exit();
uint8_t hex_char_to_int(char c);
void page_down();
void page_up();
void sw_mode(int n);

/**
 * @brief Switches the mode between View (1) and Edit (0) and updates the prompt.
 */
void sw_mode(int n) {
    if(n != (-1)) {
        mode = n;
    }

    // --- Command Prompt ---
    mvprintw(rows - 1, 0, "%s> ", mode ? "View" : "Edit");
    if(mode) {
        move(rows - 1, 6);
    }
}

/**
 * @brief Initializes ncurses environment.
 */
void init_curses() {
    initscr();              
    cbreak();               
    noecho();               
    keypad(stdscr, TRUE);   
    getmaxyx(stdscr, rows, cols); 
}

/**
 * @brief Cleans up ncurses environment.
 */
void close_curses() {
    endwin(); 
}

/**
 * @brief Converts a hex character to its integer value.
 */
uint8_t hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/**
 * @brief Saves the current disk image from memory to the specified path.
 */
int save_file(int save_as) {
    char new_path[256];
    char *target_path = file_path;

    if (save_as || !file_path) {
        curs_set(1);
        echo();
        mvprintw(rows - 1, 0, "Save As: ");
        clrtoeol();
        getnstr(new_path, sizeof(new_path) - 1);
        noecho();
        curs_set(0);

        if (strlen(new_path) == 0) {
            mvprintw(rows - 1, 0, "Save cancelled.");
            clrtoeol();
            return 0;
        }
        target_path = new_path;
    }

    FILE *out_file = fopen(target_path, "wb"); // Always use 'wb' or 'w+b' when writing the whole buffer
    if (!out_file) {
        mvprintw(rows - 1, 0, "Error: Could not open file for writing: %s", target_path);
        clrtoeol();
        return 0;
    }
    
    // Update the file path if a 'Save As' was successful
    if (target_path != file_path) {
        if (file_path) free(file_path);
        file_path = strdup(target_path);
    }
    
    // Write the in-memory buffer to the file
    size_t bytes_written = fwrite(disk_memory, 1, file_size, out_file);

    if (bytes_written != file_size) {
        mvprintw(rows - 1, 0, "Warning: Only wrote %zu of %ld bytes to %s", bytes_written, file_size, file_path);
    }

    fclose(out_file);
    unsaved_changes = 0;
    mvprintw(rows - 1, 0, "File saved successfully to: %s", file_path);
    clrtoeol();
    return 1;
}

/**
 * @brief Prompts the user to save before exiting.
 */
int prompt_save_on_exit() {
    if (!unsaved_changes) return 1;

    curs_set(1);
    echo();
    mvprintw(rows - 1, 0, "Unsaved changes! Save (s), Save As (a), or Quit without saving (q)? ");
    clrtoeol();
    char response = getch();
    noecho();
    curs_set(0);

    if (response == 's' || response == 'S') {
        if (save_file(0)) return 1; 
        return 0; 
    } else if (response == 'a' || response == 'A') {
        if (save_file(1)) return 1; 
        return 0; 
    } else if (response == 'q' || response == 'Q') {
        return 1; 
    }

    mvprintw(rows - 1, 0, "Quit cancelled.");
    return 0; 
}

/**
 * @brief Enters the interactive sector editing mode, operating on disk_memory.
 */
int edit_sector(uint8_t *sector_block) {
    int modified = 0;
    int line_offset = 2; 
    int cursor_byte_index = 0;
    int cursor_field = HEX_FIELD;
    int cursor_sub_index = 0; 

    // The sector data is a direct pointer into the global disk_memory buffer
    uint8_t *editable_data = &disk_memory[current_offset];
    size_t bytes_read = DISK_BLOCK_SIZE;

    sw_mode(0); // Set mode to Edit (0)
    curs_set(1); 
    nodelay(stdscr, FALSE); 

    while (1) {
        int byte_on_line = cursor_byte_index % BYTES_PER_LINE;
        int current_line = line_offset + (cursor_byte_index / BYTES_PER_LINE);
        
        int x, y = current_line;
        
        // Calculate X position for the cursor prompt
        if (cursor_field == HEX_FIELD) {
            // Hex coordinates: 7 (start) + (byte index * 3 spaces) + (space after 8th byte) + sub_index
            x = 7 + (byte_on_line * 3) + (byte_on_line >= 8 ? 1 : 0) + cursor_sub_index;
        } else { // ASCII_FIELD
            // ASCII coordinates: 6 (addr) + 1 (space) + 48 (hex width) + 3 (separator width) + byte_on_line
            x = 6 + 1 + 48 + 3 + byte_on_line;
        }
        
        move(y, x);
        refresh();

        int ch = getch();

        if (ch == 27) { // ESCAPE: Exit edit mode
            sw_mode(1); // Set mode to View (1)
            curs_set(0);
            // The changes were already made directly to disk_memory, so we return 1 if anything was modified
            return modified;
        } else if (ch == '\t') { // TAB: Switch field
            cursor_field = 1 - cursor_field;
            cursor_sub_index = 0; 
        } else if (ch == KEY_RIGHT) {
            cursor_byte_index = (cursor_byte_index + 1) % bytes_read; 
            cursor_sub_index = 0;
        } else if (ch == KEY_LEFT) {
            cursor_byte_index = (cursor_byte_index - 1 + bytes_read) % bytes_read; 
            cursor_sub_index = 0;
        } else if (ch == KEY_UP) {
            cursor_byte_index = (cursor_byte_index - BYTES_PER_LINE + bytes_read) % bytes_read; 
        } else if (ch == KEY_DOWN) {
            cursor_byte_index = (cursor_byte_index + BYTES_PER_LINE) % bytes_read; 
        } else if (cursor_byte_index < bytes_read) { 
            uint8_t *byte_to_edit = &editable_data[cursor_byte_index];
            
            if (cursor_field == HEX_FIELD && isxdigit(ch)) {
                modified = 1;
                unsaved_changes = 1;
                
                uint8_t val = hex_char_to_int(ch);
                
                if (cursor_sub_index == 0) { // High nibble
                    *byte_to_edit = (*byte_to_edit & 0x0F) | (val << 4); 
                    cursor_sub_index = 1;
                } else { // Low nibble
                    *byte_to_edit = (*byte_to_edit & 0xF0) | val; 
                    cursor_sub_index = 0;
                    cursor_byte_index = (cursor_byte_index + 1) % bytes_read; 
                }
                
                // --- LIVE UPDATE: Update Hex and ASCII at once ---
                int hex_x_start = 7 + (byte_on_line * 3) + (byte_on_line >= 8 ? 1 : 0);
                mvprintw(y, hex_x_start, "%02X", *byte_to_edit);

                int asc_x = 6 + 1 + 48 + 3 + byte_on_line;
                char asc_c = isprint(*byte_to_edit) ? *byte_to_edit : '.';
                mvaddch(y, asc_x, asc_c);
                // --- END LIVE UPDATE ---
                
            } else if (cursor_field == ASCII_FIELD && isprint(ch)) {
                modified = 1;
                unsaved_changes = 1;
                
                *byte_to_edit = (uint8_t)ch;
                cursor_byte_index = (cursor_byte_index + 1) % bytes_read; 
                
                // --- LIVE UPDATE: Update Hex and ASCII at once ---
                int hex_x_start = 7 + (byte_on_line * 3) + (byte_on_line >= 8 ? 1 : 0);
                mvprintw(y, hex_x_start, "%02X", *byte_to_edit);

                int asc_x = 6 + 1 + 48 + 3 + byte_on_line;
                char asc_c = isprint(*byte_to_edit) ? *byte_to_edit : '.';
                mvaddch(y, asc_x, asc_c);
                // --- END LIVE UPDATE ---
            }
        }
    }
}

/**
 * @brief Converts track and sector numbers to a file offset.
 */
long track_sector_to_offset(int track, int sector) {
    if (track < 0 || sector < 1 || sector > SECTORS_PER_TRACK) {
        return -1;
    }
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
 * @brief Pages down to the next sector.
 */
void page_down() {
    long new_offset = current_offset + FLEX_SECTOR_SIZE;
    if (new_offset < file_size) {
        current_offset = new_offset;
    } else {
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
    new_offset = (new_offset / FLEX_SECTOR_SIZE) * FLEX_SECTOR_SIZE;

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
        getch();
    }
}

/**
 * @brief Draws the hex editor view based on the current_offset, reading from disk_memory.
 */
void draw_hex_editor() {
    clear();
    // Use the pointer directly into the memory buffer for the current sector
    uint8_t *sector_block = &disk_memory[current_offset];
    SECTOR *current_sector_ptr = (SECTOR *)sector_block;
    size_t bytes_read = DISK_BLOCK_SIZE;

    // We assume the file is a multiple of FLEX_SECTOR_SIZE. 
    // If not, we should calculate the actual remaining bytes, but 
    // for a disk editor, assuming fixed sector size is typical.
    if (current_offset + bytes_read > file_size) {
         // Adjust bytes_read for the last partial sector
         bytes_read = file_size - current_offset; 
         if (bytes_read == 0 && file_size > 0) { // If we scrolled past the end
            page_up(); // Go back one sector
            draw_hex_editor();
            return;
         } else if (file_size == 0) {
             mvprintw(0, 0, "Error: Disk memory is empty.");
             update_status_line(NULL);
             refresh();
             return;
         }
    }


    int max_data_lines = DISK_BLOCK_SIZE / BYTES_PER_LINE;
    int display_lines = (rows - 3 > max_data_lines) ? max_data_lines : rows - 3;

    // --- Header ---
    mvprintw(0, 0, " Addr  00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  0123456789ABCDEF");
    mvprintw(1, 0, "------ ------------------------------------------------  ------------------");

    // --- Hex/ASCII Dump ---
    for (int i = 0; i < display_lines; i++) {
        long addr = current_offset + (i * BYTES_PER_LINE);
        int line_start_index = i * BYTES_PER_LINE;
        char hex_line[50] = {0x20};
        char ascii_line[17] = {0x20};
        int hex_pos = 0;

        for (int j = 0; j < BYTES_PER_LINE; j++) {
            int data_index = line_start_index + j;

            if (data_index < bytes_read) {
                // Hex part
                hex_pos += sprintf(hex_line + hex_pos, "%02x%s", sector_block[data_index],
                                 (j == 7) ? "  " : " ");

                // ASCII part
                char c = sector_block[data_index];
                ascii_line[j] = isprint(c) ? c : '.';
            } else {
                // Print padding for lines that don't have a full 16 bytes
                hex_pos += sprintf(hex_line + hex_pos, "   ");
                ascii_line[j] = ' ';
            }
        }
        ascii_line[BYTES_PER_LINE] = '\0';

        // Here's wher we actually print the line
        mvprintw(2 + i, 0, "%06lX %s |%s|", addr, hex_line, ascii_line);
    }

    // --- Status Line ---
    update_status_line(current_sector_ptr);

    // --- Command Prompt ---
    sw_mode((-1));

    move(rows - 1, 6); 
    refresh();
}

/**
 * @brief Updates the status line with track/sector info.
 */
void update_status_line(const SECTOR *sector_data) {
    int track, sector;
    offset_to_track_sector(current_offset, &track, &sector);

    const char *modified_status = unsaved_changes ? " [MODIFIED]" : "";
    
    if (sector_data) {
        mvprintw(rows - 2, 0,
                 "Track %d Sector: %d Next_t: %d Next_s: %d (Offset: %06lX) | File: %s%s | Version: %s",
                 track, sector,
                 sector_data->next_track, sector_data->next_sector,
                 current_offset, file_path ? file_path : "[New File]", modified_status, VERSION);
    } else {
        mvprintw(rows - 2, 0, "Track %d Sector: %d (Offset: %06lX) - Error Reading Data | Version: %s",
                 track, sector, current_offset, VERSION);
    }
    clrtoeol(); 
}

/**
 * @brief Handles the main commands.
 */
void handle_command(const char *cmd) {
    if (cmd[0] == 'q' || cmd[0] == 'h') {
        // Handled in main loop or display_help
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
    } else if (cmd[0] == 'e') {
        // No longer reads from disk_file, operates directly on disk_memory
        if (disk_memory != NULL) {
            // Pass a dummy pointer; edit_sector knows to use disk_memory[current_offset]
            if (edit_sector(NULL)) {
                // Modified flag is set, unsaved_changes is handled inside edit_sector
                // No need for file I/O here, just rely on the in-memory changes
            }
        } else {
            mvprintw(rows - 1, 2, "Cannot edit: Disk memory not loaded.");
            clrtoeol();
            getch();
        }
    } else if (cmd[0] == 's') {
        if (cmd[1] == 'a') {
            save_file(1); // Save As
        } else {
            save_file(0); // Save
        }
    }
}

/**
 * @brief Displays the help menu in the main window.
 */
void display_help() {
    clear();
    mvprintw(0, 0, "--- Flex Disk Hex Editor Help (v%s) ---", VERSION);
    mvprintw(2, 0, "Navigation Keys:");
    mvprintw(3, 2, "Page Up/KEY_PPAGE/b: Go to previous sector");
    mvprintw(4, 2, "Page Down/KEY_NPAGE/space: Go to next sector");
    mvprintw(6, 0, "Editing Mode (press 'e' at prompt):");
    mvprintw(7, 2, "TAB: Switch between Hex and ASCII fields.");
    mvprintw(8, 2, "Arrow Keys: Move cursor within the sector.");
    mvprintw(9, 2, "ESCAPE: Exit edit mode.");
    mvprintw(11, 0, "Command Prompt (at '>'):");
    mvprintw(12, 2, "e - Enter Edit Mode for the current sector.");
    mvprintw(13, 2, "s - Save the file (uses current filename).");
    mvprintw(14, 2, "sa - Save As (prompts for new filename).");
    mvprintw(15, 2, "g <offset> - Go to byte offset (e.g., 'g 0x400')");
    mvprintw(16, 2, "t <track> <sector> - Go to track and sector (e.g., 't 1 1')");
    mvprintw(17, 2, "h - Display this help screen");
    mvprintw(18, 2, "q - Quit the program (prompts to save if modified)");
    mvprintw(20, 0, "Press any key to return to the editor.");
    refresh();
    getch();
}


/**
 * @brief Main function.
 */
int main(int argc, char *argv[]) {
    FILE *disk_file_handle = NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_image_file>\n", argv[0]);
        return 1;
    }
    
    // 1. Open file to determine size and read contents
    disk_file_handle = fopen(argv[1], "rb"); 
    if (!disk_file_handle) {
        perror("Could not open disk image file for reading");
        return 1;
    }
    
    file_path = strdup(argv[1]);
    
    fseek(disk_file_handle, 0, SEEK_END);
    file_size = ftell(disk_file_handle);
    fseek(disk_file_handle, 0, SEEK_SET);

    if (file_size == 0) {
        fprintf(stderr, "Disk image file is empty.\n");
        fclose(disk_file_handle);
        free(file_path);
        return 1;
    }

    // 2. Allocate memory and read entire file into it
    disk_memory = (uint8_t *)malloc(file_size);
    if (!disk_memory) {
        fprintf(stderr, "Failed to allocate %ld bytes for disk image.\n", file_size);
        fclose(disk_file_handle);
        free(file_path);
        return 1;
    }

    if (fread(disk_memory, 1, file_size, disk_file_handle) != file_size) {
        fprintf(stderr, "Error reading entire file into memory.\n");
        free(disk_memory);
        fclose(disk_file_handle);
        free(file_path);
        return 1;
    }
    
    // Close the file handle; we now work exclusively in memory
    fclose(disk_file_handle);

    current_offset = 0;

    init_curses();

    int ch;
    int running = 1;
    char command_buffer[80];

    while (running) {
        draw_hex_editor();
        nodelay(stdscr, FALSE);
        
        ch = getch();

        if (ch == KEY_NPAGE || ch == ' ') {
            page_down();
        } else if (ch == KEY_PPAGE || ch == 'b') {
            page_up();
        } else if (ch == 'q') {
            if (prompt_save_on_exit()) {
                running = 0;
            }
        } else if (ch == 'h') {
            display_help();
        } else if (ch == 'e') {
            handle_command("e");
        } else if (ch == 's') {
            nodelay(stdscr, TRUE);
            int next_ch = getch();
            nodelay(stdscr, FALSE);
            if (next_ch == 'a') {
                handle_command("sa");
            } else {
                if (next_ch != ERR) ungetch(next_ch); 
                handle_command("s");
            }
        } else if (ch == 'g' || ch == 't') {
            // Enter command mode for 'g' or 't'
            echo(); 
            curs_set(1); 
            mvprintw(rows - 1, 0, "> %c", ch);
            clrtoeol();
            
            getnstr(command_buffer, sizeof(command_buffer) - 2); 
            
            noecho(); 
            curs_set(0); 
            
            char full_cmd[80];
            snprintf(full_cmd, sizeof(full_cmd), "%c%s", ch, command_buffer);
            handle_command(full_cmd);
        }
    }

    close_curses();
    // 3. Free the allocated memory
    if (disk_memory) free(disk_memory);
    if (file_path) free(file_path);

    return 0;
}
