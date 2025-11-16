/*
 * CMD2BIN
 * A tool for converting executable FLEX CMD files to binary files.
 * By Daniel Tufvesson 2015
 *
 * http://www.waveguide.se/?article=reading-flex-disk-images
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define false 0
#define true 1

int main(int argc, char **argv){
    FILE *flxFile;
    FILE *binFile;

#if 0
    unsigned short address, next_address;
    unsigned char  length, data;
    unsigned int   addressLo = 0x0000, addressHi = 0xFFFF, first_run = true;
#else
    uint16_t address, next_address;
    uint8_t  length, data;
    uint16_t addressLo = 0x0000, addressHi = 0xFFFF, first_run = true;
#endif

    if(argc < 3){
        printf("Usage: %s <infile.cmd> <outfile.bin> [start address] [stop address]\n",argv[0]);
        exit(0);
    }

    // Open files
    if( (flxFile = fopen( argv[1], "rb")) == NULL ){
        printf("Unable to open source file\n");
        exit(-1);
    }

    if( (binFile = fopen( argv[2], "wb")) == NULL ){
        printf("Unable to open destination file\n");
        exit(-1);
    }

    // Read start and stop addresses
    if(argc == 5){
        sscanf(argv[3], "%x", &addressLo);
        sscanf(argv[4], "%x", &addressHi);
    }

    // Read loop
    printf("Converting %04X to %04X\n", addressLo, addressHi);
    while(!feof(flxFile)){
        while(!feof(flxFile) && (data = fgetc(flxFile)) == 0); // Skip over chunks of zeroes

        if(data != 0x02) break; // Exit on unknown chunk ID

        // Read chunk header
        data    = fgetc(flxFile); //Address MSB
        address = data << 8; //Shift one byte
        data    = fgetc(flxFile); //Address LSB
        address = address | data;
        length  = fgetc(flxFile); //Length

        if(first_run){
            next_address = address;
        }

        // Only print the chunks we write
        if(address >= addressLo && address <= addressHi){
            if(address != next_address & !first_run)
                printf("\n");

            printf("\nAddress: %04X-%04X  Length: %03d",address,address+length-1, length);
            first_run = false;
            next_address = address+length;
        }

        // Read chunk
        for(;length > 0; length--){
            data = fgetc(flxFile); //Data

            if(address >= addressLo && address <= addressHi)
                fputc(data,binFile);
        }
    }

    printf("\n\n");
    fclose(flxFile);
    fclose(binFile);
}
