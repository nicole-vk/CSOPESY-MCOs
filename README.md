# Group Members
ANCHETA, LIAM MICHAEL ALAIN

FAUSTINO, REUCHLIN CHARLES SURATOS

NAVARRO, RAFAEL LUIS LIM

SHI, NICOLE JIA YING SHI

# Marquee Console
### Location of the Source Code
Source Code, main.cpp, is located inside the Marquee Console folder.

### Install MinGW-w64 g++ in MSYS2
Open msys2.exe and run:

     > pacman -Syu                          # full update

     > pacman -S mingw-w64-x86_64-gcc       # package contains g++, gcc, and other build tools for 64-bit

### Use the Right Shell
After installing, close msys2.exe and open mingw64.exe. Now check if g++ is there:

     > g++ --version

### Compile and Run
#### Marquee Console

     > g++ main.cpp -o main.exe

     > ./main.exe

#### Process Scheduler

     >  g++ main.cpp MC01.cpp -o project.exe -std=c++17 -pthread

     > ./project.exe
