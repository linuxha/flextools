# flextools

Linux tools for working with the TSC's Flex disk images and vintage computers such as a SWTPC or Gimix Ghost (based on the Motorola 6800 or 6809 processor).

## Impetus

I'm working on creating a set of libraries for the Fuzix 6800 OS and C compiler. So I can use the Fuzix C compiler to write code for TSC's Flex OS and Microware's RT68MX OS. I still need to write the basic libraries such as open, close, read, write, etc. but the Fuzix C compiler works well. And it's pretty much the only C compiler (other than Small C which I'm working on also) for the 6800. I am able to take the C code and compile it to a binary then convert that binary to a Flex cmd format. I test with the exorsim 6800 (another repos I have) and virtual disks. I haven't attempted to use the emu6800 that comes with Fuzix. At some point I may attempt to understand that as it seems to be a good way to run tests from a make file.

## AI (Gemini)

I initially decided to attack the problem of Flex tools for Linux using Google's Gemini. I gave Gemini a long list of requirements, like I would for any software engineer or programmer. It appeared that it did a really good job when I initially compiled it. But the more I checked the more confused I got. I found a lot of bad code practices (uppercase variables). And failures to actually set variables used in structures written to the Flex disk image file. And other weird errors (can AIs be dyslexic?). At first I attempted to let Gemini attempt to fix the problems by giving it enough information for a programmer to debug the issue. That didn't work well. I then gave it the answer to the problem and that was no better. So I gave up on Gemini and attacked the code. That's where I found a lot of issues with 'off by one' errors. A lot of them.

So I decided to go it on my own. I've attempted to put a lot of the pre-defined things (default values, structures, etc) in a flexfs.h file and I've been converting the source over to using that. I mostly have flexdsk.c, flexadd.c and flexedit.c is mostly working order. The file flexsort.c needs work on fixing the secotr links when it rewrites the directory sectors. Oh, flexadd.c need to check for the same name being added again. It doesn't at this moment.

At this moment the code is not pretty, I've hacked a few things to get the code working. A refactor really is in order but for now this is it.

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

# Notes

As usual, this is a work in progress. There is a ton of debug code still sitting on much of this. The AI's math wasn't 'mathing'. Also this code isn't pretty. Not up to my usual standards but I trying to grasp what was wrong and fix the code. So far it's quite a few hacks to work around the AI's mess. I may refactor sometime in the future.

# Tools

|---------------|---------------------------------------------------|
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
| flex_vfs      | Create and manipulate a flex disk (Perl)          |
| flex_vfs.help | text file with basic help                         |
|               |                                                   |
|---------------|---------------------------------------------------|

# Future

Well it looks like I need to build some Flex 1.0 (Mini Flex?) as my SWTPC & Gimix Ghost both have disk boards that only handle 128 bytes per sector.

# Reason

As a collector of older systems I often need tools to manipulate files and commands for that particular computer. This collection is for the Flex OS which I have a number of systems (6800 and 6809).

# Credits

Need to add a lot of folks here
