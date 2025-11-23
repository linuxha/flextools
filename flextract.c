/*
 * FLEXTRACT
 * A tool for extracting files from FLEX floppy images
 * By Daniel Tufvesson 2015-2020
 *
 * http://www.waveguide.se/?article=reading-flex-disk-images
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char u_byte;
typedef unsigned char bool;

#define true 1
#define false 0

/*
 * FLEX sector size
 */
#define SECTOR_SIZE 256

/*
 * Global variables
 */
u_byte *dskFileData;
int dskFileSize;
int dskTracks;
int dskSectors;
u_byte sector[SECTOR_SIZE];

/*
 * Definition of SIR sector structure
 */
#define SIR_SECTOR_PADDING 16
typedef struct{
    u_byte volLabel[11];        // 0 - 10
    u_byte volNumberHi;         /* 11-12 */
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

/*
 * Definition of directory entry
 */
#define DIR_SECTOR_PADDING 16
typedef struct{
    u_byte fileName[8];
    u_byte fileExt[3];
    u_byte unused1;
    u_byte unused2;
    u_byte startTrack;
    u_byte startSector;
    u_byte endTrack;
    u_byte endSector;
    u_byte totalSectorsHi;
    u_byte totalSectorsLo;
    u_byte randomFileFlag;
    u_byte unused3;
    u_byte dateMonth;
    u_byte dateDay;
    u_byte dateYear;
} DIR_struct;

/*
 * Cleanly exit program
 */
bool program_exit(int rc){
    free(dskFileData);
    exit(rc);
}

/*
 * Calculate image structure based on file size, sector size and FLEX linking bytes
 */
bool calcDiskStructure(){
    int i, tr, se;

    // Check if file big enough to be a disk image
    if(dskFileSize < SECTOR_SIZE*10)
        return false;
  
    // Method 1 - Follow entire sector chain on disk
    tr = 0;
    for(i = SECTOR_SIZE*2; i < dskFileSize; i = i + SECTOR_SIZE){
        if(dskFileData[i] > tr && dskFileData[i] - tr == 1)
            tr = dskFileData[i];
    }

    dskTracks = tr+1;
    dskSectors = dskFileSize/dskTracks/SECTOR_SIZE;

    if(dskTracks*dskSectors*SECTOR_SIZE == dskFileSize)
        return true;

    // Method 2 - Find the longest consecutive sector chain on one track
    se = 0;
    dskSectors = 0;
    for(i = SECTOR_SIZE*2; i < dskFileSize - SECTOR_SIZE; i = i + SECTOR_SIZE){
        if(dskFileData[i+1] == se + 1)
            se++;
        else if(se > dskSectors){
            dskSectors = se;
            se = 0;
        }
    }

    if(dskSectors){
        dskTracks = dskFileSize/dskSectors/SECTOR_SIZE;
        if(dskTracks*dskSectors*SECTOR_SIZE == dskFileSize)
            return true;
    }

    // Method 3 - Determine geometry based on SIR information
    dskTracks  = dskFileData[SECTOR_SIZE*2 + SIR_SECTOR_PADDING + 22]; // SIR[22]
    dskSectors = dskFileData[SECTOR_SIZE*2 + SIR_SECTOR_PADDING + 23]; // SIR[23]

    if(dskTracks >= 34 && dskSectors >= 10)
        return true;

    return false;
}

/*
 * Return a specific sector from the loaded image
 */
void readSector(void *data, int track, int sector){
    if(track < dskTracks && sector <= dskSectors)
        memcpy(data, &dskFileData[track*dskSectors*SECTOR_SIZE+(sector-1)*SECTOR_SIZE], SECTOR_SIZE);
    else
        memset(data,0,SECTOR_SIZE);
}

/*
 * Print a sector as a HEX + ASCII dump
 */
void printSector(void *data){
    int i, j;
    u_byte *d;

    d = data;
    for(i = 0; i < 16; i++){
        for(j = 0; j < 16; j++)
            printf("%0.2X ", d[i*8+j]);
        for(j = 0; j < 16; j++)
            if(d[i*8+j] > 0x1F && d[i*8+j] < 0x7f)
                printf("%c", d[i*8+j]);
            else
                printf(".");
        printf("\n");
    }
}

/*
 * Calcuate size of given file
 */
int getFileSize(FILE *fp){
    int fSize;
    int oPos; 

    oPos = ftell(fp);
    fseek(fp,0,SEEK_END);

    fSize = ftell(fp);
    fseek(fp,oPos,SEEK_SET);

    return fSize;
}

/*
 * Print FLEX volume label
 */
void printVolumeLabel(u_byte *s){
    int i;

    for(i = 0; i < 11; i++)
        if(s[i] > 0x1f && s[i] < 0x7f)
            printf("%c",s[i]);
}

/*
 * Print FLEX file name (name + ext)
 */
void printFileName(u_byte *s, u_byte *e){
    int i,n;

    n = 0;
    for(i = 0; i < 8; i++)
        if(s[i] > 0x2f && s[i] < 0x7f)
            printf("%c",s[i]);
        else
            n++;
    printf(".");
    for(i = 0; i < 3; i++)
        if(e[i] > 0x1F && e[i] < 0x7f)
            printf("%c",e[i]);
        else
            n++;
    for(i = 0; i < n; i++)
        printf(" ");
}

/*
 * Compare a FLEX file name with given filename string
 */
bool matchFileName(u_byte *n, u_byte *e, unsigned char *fname){
    int i;
    unsigned char name[13], cs[2];

    name[0] = 0;
    cs[1] = 0;
    for(i = 0; i < 8; i++){
        if(n[i] > 0x2f && n[i] < 0x7f){
            cs[0] = n[i];
            strcat(name,cs);
        }
    }
    strcat(name,".");
    for(i = 0; i < 3; i++){
        if(e[i] > 0x2f && e[i] < 0x7f){
            cs[0] = e[i];
            strcat(name,cs);
        }
    }
    if(strcmp(name,fname) == 0)
        return true;
    return false;
}

/*
 * Export RAW file starting at track/sector
 */
int exportFile(FILE *outFile, u_byte startTrack, u_byte startSector, bool checkSequence){
    // Loop through file sector chain
    int i;
    int t = startTrack;
    int s = startSector;
    int seq = 1;

    while(1){
        readSector(&sector,t,s);
        // Write sector to file
        for(i = 4; i < SECTOR_SIZE; i++)
            fputc(sector[i],outFile);
        // Prepare for next sector
        if(t == sector[0] && s == sector[1]) return seq; // Faulty chain?
        t = sector[0];
        s = sector[1];
        if(t == 0 && s == 0) // End of file?
            break;
        if(checkSequence)
            if(seq == sector[2]*256+sector[3]) // Verify sector sequence
                seq++;
            else
                break;
    }
    return seq;
}

/*
 * Export ASCII text file starting at track/sector
 */
int exportTextFile(FILE *outFile, u_byte startTrack, u_byte startSector, bool checkSequence){
    // Loop through file sector chain
    int i,j;
    int t = startTrack;
    int s = startSector;
    int seq = 1;
    u_byte last_char = 0;
    bool spacecomp = 0;

    while(1){
        readSector(&sector,t,s);
        // Write sector to file and parse as text file
        for(i = 4; i < SECTOR_SIZE; i++){
            if(spacecomp){
                for(j = 0; j < sector[i]; j++)
                    fputs(" ",outFile);
                spacecomp = 0;
            }
            else
                switch(sector[i]){
                case 0x0A: // New line
                    if(last_char != 0x0D)
                        fputs("\n",outFile);
                    break;
                case 0x0D: // New line
                    if(last_char != 0x0A)
                        fputs("\n",outFile);
                    break;
                case 0x09: // Space compression (0x09+nn = add nn spaces)
                    spacecomp = 1;
                    break;
                default: // Printable ASCII char
                    if(sector[i] > 0x1F && sector[i] < 0x7F)
                        fputc(sector[i],outFile);
                    break;
                }
            last_char = sector[i];
        }
        // Prepare for next sector
        if(t == sector[0] && s == sector[1]) return seq; // Faulty chain?
        t = sector[0];
        s = sector[1];
        if(t == 0 && s == 0) // End of file?
            break;
        if(checkSequence)
            if(seq == sector[2]*256+sector[3]) // Verify sector sequence
                seq++;
            else
                break;
    }
    return seq;
}

/*
 * Program begin here
 */
int main(int argc, char **argv){
    FILE *dskFile, *outFile;
    SIR_struct *sir;
    DIR_struct *dir;
    bool flag_verbose = true, flag_list = true, flag_onecol = false, flag_extract = false, flag_text = false, flag_debug = false;

    if(argc < 2){
        printf("Usage: flextract <image file> [options] [file name] [output file]\n",argv[0]);
        printf("Options: v - Verbose\n");
        printf("         l - List directory\n");
        printf("         1 - List directory as single column\n");
        printf("         x - Extract file\n");
        printf("         t - Do FLEX text to ASCII conversion\n");
        printf("         d - Print SIR/DIR sector dumps\n");
        printf("Output file \"-\" means console (stdout)\n");
        printf("Version 1.5 by Daniel Tufvesson 2015-2020\n");
        program_exit(-1);
    }

    // Reads flags
    if(argc > 2){
        if(memchr(argv[2],'v',strlen(argv[2])) || memchr(argv[2],'V',strlen(argv[2])))
            flag_verbose = true;
        else
            flag_verbose = false;
        if(memchr(argv[2],'l',strlen(argv[2])) || memchr(argv[2],'L',strlen(argv[2])))
            flag_list = true;
        else
            flag_list = false;
        if(memchr(argv[2],'1',strlen(argv[2])))
            flag_onecol = true;
        else
            flag_onecol = false;
        if(memchr(argv[2],'x',strlen(argv[2])) || memchr(argv[2],'X',strlen(argv[2])))
            flag_extract = true;
        else
            flag_extract = false;
        if(memchr(argv[2],'t',strlen(argv[2])) || memchr(argv[2],'T',strlen(argv[2])))
            flag_text = true;
        else
            flag_text = false;
        if(memchr(argv[2],'d',strlen(argv[2])) || memchr(argv[2],'D',strlen(argv[2])))
            flag_debug = true;
        else
            flag_debug = false;
    }
  
    if(flag_extract && argc < 5){
        printf("Parameter(s) missing\n");
        program_exit(-1);
    }

    if( (dskFile = fopen( argv[1], "rb")) == NULL ){
        printf("Unable to open image file\n");
        program_exit(-1);
    }

    // Read image file
    dskFileSize = getFileSize(dskFile);

    if(flag_verbose)
        printf("Image size is %d bytes - ", dskFileSize);
    dskFileData = (u_byte*)malloc(dskFileSize);

    if(!dskFileData){
        printf("Unable to allocate memory\n");
        fclose(dskFile);
        program_exit(-2);
    }

    if(fread(dskFileData,1,dskFileSize,dskFile)!=(size_t)dskFileSize){
        free(dskFileData);
        printf("Unable to read file\n");
        fclose(dskFile);
        program_exit(-2);
    }

    fclose(dskFile);

    // Determine image file structure
    if(!calcDiskStructure()){
        printf("Unable to determine image structure\n");
        program_exit(-3);
    }
    if(flag_verbose)
        printf("%u tracks, %u sectors/track\n", dskTracks, dskSectors);

    // Read SIR as track 0 sector 3
    readSector(&sector,0,3);
    if(flag_debug){
        printf(" -- Track 0 Sector 3 --\n");
        printSector(&sector);
    }
    sir = (void*)&sector[SIR_SECTOR_PADDING];

    if(flag_list){
        printf("\nVolume label     ");
        printVolumeLabel(sir->volLabel);
        printf("\n");
        printf("Volume number    %02X%02X\n",sir->volNumberHi,sir->volNumberLo);
        printf("Free area        t%u s%u - t%u s%u\n",
               sir->firstFreeTrack, sir->firstFreeSector,
               sir->lastFreeTrack, sir->lastFreeSector);
        printf("Free sectors     %u\n",sir->freeSectorsHi*256+sir->freeSectorsLo);
        printf("End sector       t%u s%u\n", sir->endTrack, sir->endSector);
        printf("Creation date    %02u-%02u-%02u\n\n",sir->dateYear,sir->dateMonth,sir->dateDay);
    }

    // Read DIR starting at track 0 sector 5
    int i,j,t,s,file_t,file_s,file_size;
    t = 0; s = 5; // Start of directory
    file_t = 0; file_s = 0;
    if(flag_list)
        printf("NAME           START     END        SIZE    DATE       FLAG\n");

    while(1) {
        readSector(&sector,t,s);
        if(flag_debug){
            printf(" -- Track %d Sector %d --\n",t,s);
            printSector(&sector);
        }
        for(j = 0; j <= SECTOR_SIZE-sizeof(DIR_struct); j = j + sizeof(DIR_struct)){
            dir = (void*)&sector[j+DIR_SECTOR_PADDING];
            if(dir->fileName[0] != 0xFF && strlen(dir->fileName)){ // Valid directory entry?
                if(flag_extract && matchFileName(dir->fileName, dir->fileExt, argv[3])){ // Filename match?
                    file_t = dir->startTrack;
                    file_s = dir->startSector;
                    file_size = dir->totalSectorsHi*SECTOR_SIZE+dir->totalSectorsLo;
                }
                if(flag_list || flag_onecol){
                    printFileName(dir->fileName, dir->fileExt);
                    if(flag_list)
                        printf("   t%02u s%02u - t%02u s%02u   %#5u   %#3u-%02u-%02u   %02X\n",
                               dir->startTrack, dir->startSector,
                               dir->endTrack, dir->endSector,
                               dir->totalSectorsHi*SECTOR_SIZE+dir->totalSectorsLo,
                               dir->dateYear,dir->dateMonth,dir->dateDay,
                               dir->randomFileFlag);
                    else
                        printf("\n");
                }
            }
        }
        if(t == sector[0] && s == sector[1]) // Faulty chain? (prevent infinite loop)
            break;
        t = sector[0];
        s = sector[1];
        if(t == 0 && s == 0) // End of directory?
            break;
    }
    if(flag_list)
        printf("\n");
  
    // Extract file from image
    int seq;
    if(flag_extract){
        if(file_t == 0){
            printf("File %s not found\n", argv[3]);
            program_exit(-3);
        }
        if(flag_verbose)
            printf("Extracting file %s\n", argv[3]);
        // Open and create desination file
        // File name "-" outputs to standard output instead of file
        if(strcmp(argv[4],"-") == 0){
            outFile = stdout;
        }else{
            if( (outFile = fopen( argv[4], "wb")) == NULL ){
                printf("Unable to create file\n");
                program_exit(-1);
            }
        }
        if(flag_text)
            seq = exportTextFile(outFile,file_t,file_s,true);
        else
            seq = exportFile(outFile,file_t,file_s,true);
        if(outFile != 0 && outFile != stdout)
            fclose(outFile);
        if(flag_verbose)
            printf("%u sectors read\n", seq);
        if(seq != file_size){
            printf("File length mis-match\n");
            program_exit(-4);
        }
    }

    program_exit(0);
}
