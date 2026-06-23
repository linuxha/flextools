# flextools

Linux tools for working with the TSC's Flex disk images and vintage computers such as a SWTPC or Gimix Ghost (based on the Motorola 6800 or 6809 processor).

## Quick usage

```bash
# Add a Linux file into a FLEX image
./flexadd <disk_image_file> <host_file_path> <FLEX_FILENAME.EXT> [-t] [-z 128|256]

# List files in a FLEX image
./flexfs -l [-z 128|256] <disk.dsk>

# Get a FLEX file into Linux
./flexfs -g [-t] [-z 128|256] <disk.dsk> <FILE.EXT> <linuxfile>

# Put a Linux file into FLEX
./flexfs -p [-t] [-z 128|256] <disk.dsk> <FILE.EXT> <linuxfile>

# Delete a FLEX file
./flexfs -d [-z 128|256] <disk.dsk> <FILE.EXT>

# Convert Motorola S-record to FLEX CMD
./s192cmd <input.s19> <output.cmd>
```

Currently the 128 byte sectors are not ready for use. That's a project in progress. TSC's MiniFlex uses 128 byte sectors. I do plan to properly support those files images also.

## Impetus

I'm working on creating a set of libraries for the Fuzix 6800 OS and C compiler. So I can use the Fuzix C compiler to write code for TSC's Flex OS and Microware's RT68MX OS. I still need to write the basic libraries such as open, close, read, write, etc. but the Fuzix C compiler works well. And it's pretty much the only C compiler (other than Small C which I'm working on also) for the 6800. I am able to take the C code and compile it to a binary then convert that binary to a Flex cmd format. I test with the exorsim 6800 (another repos I have) and virtual disks. I haven't attempted to use the emu6800 that comes with Fuzix. At some point I may attempt to understand that as it seems to be a good way to run tests from a make file.

### AI (Gemini)

I initially decided to attack the problem of Flex tools for Linux using Google's Gemini. I gave Gemini a long list of requirements, like I would for any software engineer or programmer. It appeared that it did a really good job when I initially compiled it. But the more I checked the more confused I got. I found a lot of bad code practices (uppercase variables). And failures to actually set variables used in structures written to the Flex disk image file. And other weird errors (can AIs be dyslexic?). At first I attempted to let Gemini attempt to fix the problems by giving it enough information for a programmer to debug the issue. That didn't work well. I then gave it the answer to the problem and that was no better. So I gave up on Gemini and attacked the code. That's where I found a lot of issues with 'off by one' errors. A lot of them.

So I decided to go it on my own. I've attempted to put a lot of the pre-defined things (default values, structures, etc) in a flexfs.h file and I've been converting the source over to using that. I mostly have flexdsk.c, flexadd.c and flexedit.c is mostly working order. The file flexsort.c needs work on fixing the sector links when it rewrites the directory sectors.

At this moment the code is not pretty, I've hacked a few things to get the code working. A refactor really is in order but for now this is it.

I've been also using Github's Copilot and I've made some further progress in adding features (still need to work on the TAB translation). Copilot gives me fits at it's ablility to ignore what I just told it. I often have to tell it twice some technical detail.

My attempts at using a local LLM (Ollama/Hermes/Gemma4) are worse. But I suspect I still need to learn how to setup the AGENTS.md, skills and tools. It's a learning process. I've built a python script to wrap exorsim so the AI can use that as a skill/tool to test the 6800 asm code. I also have flexadd (below) as a skill/tool for file transfers. I'll share these tools at a later date.

I've since gotten a Claude account and will attempt to use claude to replicate the work done with the local LLMs. All this experimenting is so I can teach a class at my (local makespace, CDL)[https://compdecon.github.io/) in September on building an Agentic Flow (loops).

# Other tools I found

I actually had these tools and didn't realize it. Only after I decided to add everything to the repos did I find these tools on my system.

## flex_vfs

A perl script that really does a good job of dealing with Flex files and disk images. I use this to double check my tools.

https://github.com/nealcrook/multicomp6809
Neal Crook, July 2015.

## flextract.c

http://www.waveguide.se/?article=reading-flex-disk-images
Daniel Tufvesson

## flexfs.c

Fuzix-Bintools
Fuzix-Compiler-Kit

Used in testing with the Fuzix OS and C Compiler.

(Need to send a PR to EtchedPixels)

# Notes

As usual, this is a work in progress. There is a ton of debug code still sitting on much of this. The AI's math wasn't 'mathing'. Also this code isn't pretty. Not up to my usual standards but I trying to grasp what was wrong and fix the code. So far it's quite a few hacks to work around the AI's mess. I may refactor sometime in the future.

## Recent updates (v1.1.1)

- flexadd and flexfs are both at version 1.1.1.
- Directory entry alignment fixes are in place (entries start at byte 16 in each directory sector).
- Existing-file handling in flexadd now prompts before delete/re-add.
- Data sector logical record numbering is written correctly for file extraction compatibility.
- Directory random flag is now explicitly cleared (0) when writing new entries.

## Text translation option (-t)

Both flexadd and flexfs now support optional text translation.

- Linux to FLEX: LF (0x0A) to CR (0x0D), and space runs (2-127) to 0x09 + count
- FLEX to Linux: CR (0x0D) to LF (0x0A), and 0x09 + count expanded back to spaces

Note: the 0x09 byte is the FLEX text compression marker for space runs. It is
not treated as a literal ASCII HT/TAB character in text mode.

Examples:

```bash
# Add Linux file to FLEX image with text conversion (LF -> CR)
./flexadd disk.dsk host.txt HOSTFILE.TXT -t

# Put Linux file into FLEX image with text conversion (LF -> CR)
./flexfs -p -t disk.dsk HOSTFILE.TXT host.txt

# Get FLEX file into Linux file with text conversion (CR -> LF)
./flexfs -g -t disk.dsk HOSTFILE.TXT out.txt
```

Sector size override is available on both tools with -z 128 or -z 256.

# Tools

| file          | description                                       |
|---------------|---------------------------------------------------|
| cmd2bin.c     | converts a cmd file to binary                     |
| fddump.c      | a hex dump like program for Flex disks - old name |
| flexdump.c    | a hex dump like program for Flex disks - new name |
| fdedit.c      | a hex editor program for flex disks - old name    |
| flexedit.c    | a hex editor program for flex disks - new name    |
| flexadd.c     | a program to add a file to a virtual flex disk    |
| flex-binify.c | convert a flex bin file to a command file         |
|               | used with Fuzix's 6800 C Compiler.                |
| flexdsk.c     | Create a virtual flex disk (up to 16M)            |
| flexfs.c      | manipulate virtual flex disks                     |
| flexsort.c    | Clean up a flex disk directory                    |
| flextract.c   | manipulate a flex disk                            |
| s192cmd.c     | convert Motorola S-record files to FLEX CMD       |
| flex_vfs      | Create and manipulate a flex disk (Perl)          |
| flex_vfs.help | text file with basic help                         |


# Future

Well it looks like I need to build some Flex 1.0 (Mini Flex?) as my SWTPC & Gimix Ghost both have disk boards that only handle 128 bytes per sector.

# Caveats

The folks on the FLEX UniFLEX User Group have noted that when using the Gotek and Western Digital (WD) Floppy Disk Controller chips (FDC) that cylinder sizes greater than 243 don't work. According to the datasheet, the values $F5 and $FE may not be used for the track and sector!

The larger 16M Flex images work fine with serial drives (Flexnet, Drivewire, etc.) but will give you nothing but problems with WD chips.

# Reason for this code/madness

As a collector of older systems I often need tools to manipulate files and commands for that particular computer. This collection is for the Flex OS which I have a number of systems (6800 and 6809).

# Credits

Need to add a lot of folks here
