// addrspace.cc 
//	Routines to manage address spaces (executing user programs).
//
//	In order to run a user program, you must:
//
//	1. link with the -N -T 0 option 
//	2. run coff2noff to convert the object file to Nachos format
//		(Nachos object code format is essentially just a simpler
//		version of the UNIX executable object code format)
//	3. load the NOFF file into the Nachos file system
//		(if you haven't implemented the file system yet, you
//		don't need to do this last step)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "system.h"
#include "addrspace.h"
#include "noff.h"
#include "machine.h" // definition of PageSize
#include "virtualmemorymanager.h"

#ifdef HOST_SPARC
#include <strings.h>
#endif

//----------------------------------------------------------------------
// SwapHeader
// 	Do little endian to big endian conversion on the bytes in the 
//	object file header, in case the file was generated on a little
//	endian machine, and we're now running on a big endian machine.
//----------------------------------------------------------------------

static void 
SwapHeader (NoffHeader *noffH)
{
	noffH->noffMagic = WordToHost(noffH->noffMagic);
	noffH->code.size = WordToHost(noffH->code.size);
	noffH->code.virtualAddr = WordToHost(noffH->code.virtualAddr);
	noffH->code.inFileAddr = WordToHost(noffH->code.inFileAddr);
	noffH->initData.size = WordToHost(noffH->initData.size);
	noffH->initData.virtualAddr = WordToHost(noffH->initData.virtualAddr);
	noffH->initData.inFileAddr = WordToHost(noffH->initData.inFileAddr);
	noffH->uninitData.size = WordToHost(noffH->uninitData.size);
	noffH->uninitData.virtualAddr = WordToHost(noffH->uninitData.virtualAddr);
	noffH->uninitData.inFileAddr = WordToHost(noffH->uninitData.inFileAddr);
}

//----------------------------------------------------------------------
// AddrSpace::AddrSpace
// 	Create an address space to run a user program.
//	Load the program from a file "executable", and set everything
//	up so that we can start executing user instructions.
//
//	Assumes that the object code file is in NOFF format.
//
//	First, set up the translation from program memory to physical 
//	memory.  For now, this is really simple (1:1), since we are
//	only uniprogramming, and we have a single unsegmented page table
//
//	"executable" is the file containing the object code to load into memory
//----------------------------------------------------------------------

AddrSpace::AddrSpace(OpenFile *executable, PCB* newPCB)
{
    NoffHeader noffH;
    unsigned int i, size;

    executable->ReadAt((char *)&noffH, sizeof(noffH), 0);
    if ((noffH.noffMagic != NOFFMAGIC) && 
		(WordToHost(noffH.noffMagic) == NOFFMAGIC))
        SwapHeader(&noffH);
    ASSERT(noffH.noffMagic == NOFFMAGIC);

    // how big is address space?
    size = noffH.code.size + noffH.initData.size + noffH.uninitData.size 
			+ UserStackSize;	// we need to increase the size
						// to leave room for the stack
    numPages = divRoundUp(size, PageSize);
    size = numPages * PageSize;

    DEBUG('a', "Initializing address space, num pages %d, size %d\n", numPages, size);

    this->pcb = newPCB;

    pageTable = new TranslationEntry[numPages];
    for (i = 0; i < numPages; i++) {

        // Set the usual bits for a new process
        pageTable[i].virtualPage = i;
        pageTable[i].physicalPage = -1; // demand paging
        pageTable[i].valid = FALSE;
        pageTable[i].use = FALSE;
        pageTable[i].dirty = FALSE;
        pageTable[i].readOnly = FALSE;

        // Allocate space for entire addr space on backing store at creation
        //pageTable[i].space = this;
        pageTable[i].locationOnDisk = virtualMemoryManager->allocSwapSector();
        char placeHolder[PageSize];
        bzero(placeHolder, PageSize);
        virtualMemoryManager->writeToSwap(placeHolder, PageSize, pageTable[i].locationOnDisk);

        // Debuggin output
        int currVirtPage = pageTable[i].locationOnDisk / PageSize;
        DEBUG('v',"Z %d: %d\n", pcb->getPID(), currVirtPage);

        // Maintain swap space page information
        //SwapSectorInfo * swapInfo =
        //        virtualMemoryManager->getSwapSectorInfo(pageTable[i].locationOnDisk / PageSize);
        //swapInfo->pageTableEntry = &pageTable[i];
    }

    //printf("Loaded Program: %d code | %d data | %d bss\n",
    DEBUG('v',"Loaded Program: %d code | %d data | %d bss\n",
        noffH.code.size, noffH.initData.size, noffH.uninitData.size);

    // then, copy in the code and data segments into memory using new
    // ReadFile functionality
    if (noffH.code.size > 0) {
        DEBUG('a', "Initializing code segment, at 0x%x, size %d\n", 
            noffH.code.virtualAddr, noffH.code.size);

        ReadFile(noffH.code.virtualAddr,executable,noffH.code.size,
            noffH.code.inFileAddr);
    }
    if (noffH.initData.size > 0) {
        DEBUG('a', "Initializing data segment, at 0x%x, size %d\n", 
            noffH.initData.virtualAddr, noffH.initData.size);
        ReadFile(noffH.initData.virtualAddr,executable,
            noffH.initData.size,noffH.initData.inFileAddr);
    }
}


//----------------------------------------------------------------------
// AddrSpace::~AddrSpace
// 	Deallocate an address space, releasing the physical memory and
// 	associated metadata.
//----------------------------------------------------------------------

AddrSpace::~AddrSpace()
{
    if (isValid()) 
    {
        virtualMemoryManager->releasePages(this);
        delete [] pageTable;
        delete pcb;
    }
}



//----------------------------------------------------------------------
// AddrSpace::ReadFile
//     
//     Loads the code and data segments into the translated memory.
//----------------------------------------------------------------------

int AddrSpace::ReadFile(int virtAddr, OpenFile* file, int size, int fileAddr)
{
    char buffer1[size];
    int numBytesTotal = file->ReadAt(buffer1, size, fileAddr);
    size = numBytesTotal;
    int bytesCopiedSoFar = 0;

    while (size > 0) { // we have remaining bytes to read

        int pageTableIndex = virtAddr / PageSize;
        int offset = virtAddr % PageSize;
        int numBytesThisLoop = size < PageSize ? size : PageSize; // read 1 page at a time
        virtualMemoryManager->writeToSwap(buffer1 + bytesCopiedSoFar, numBytesThisLoop,
                                        pageTable[pageTableIndex].locationOnDisk + offset);
        size -= numBytesThisLoop;
        bytesCopiedSoFar += numBytesThisLoop;
        virtAddr += numBytesThisLoop;
    }

    return numBytesTotal;
}
