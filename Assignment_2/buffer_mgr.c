#include <stdio.h>
#include <strlib.h>

#include "buffer_mgr.h"
#include "storage_mgr.h"

#define RC_BM_PINNED 5
#define RC_PIN_NOUNUSED	6
#define RC_RANGE_ERROR 7

#define LARGENUM 1e9

static int TIMER = 0;	//global timer, to track the load time and used time for FIFO and LRU

/********************************************************
 Define the struct BM_FrameHandle
 Xiaolin hu
 ********************************************************/
typedef struct BM_FrameHandle{
	//These two are exactly the struct BM_PageHandle declared in header file
	//Modified by Linlin
    // PageNumber pageNum;
    // char *data;
    BM_PageHandle pgdata;	//page handle, stored the page num and its data in memory
    
    int fix_count; //starting from 0
    bool is_dirty; //true is dirty, false is clean

    //added by Linlin, used to keep loadtime and usedtime, for FIFO and LRU
    int load_time;	//the time this page is loaded to the frame, used in FIFO 
    int used_time;	//the last time this frame is used, used in LRU
    
} BM_FrameHandle

/********************************************************
Define the struct mgmtData
Xiaolin hu
********************************************************/
typedef struct BM_MgmtData { //The bookkeeping info
    
    //using a BM_FrameHandle and a SM_FileHandle to store the pages and current file info
    BM_FrameHandle *frames; //Actually it's an array of frames
    SM_FileHandle *fileHandle;
    
    int read_times;  //getNumReadIO()
    int write_times;  //getNumWriteIO()

    //added by Linlin Chen, to maintain a mapping between page number and page frames for fast look-ups
    int *page2frame;	//an array to store the mapping from page number to frames
    int *frame2page;	//an array to store the mapping from frames to page number
	

} BM_mgmtData;


// Buffer Manager Interface Pool Handling
/********************************************************
initBufferPool()
Xiaolin Hu
*********************************************************/
RC initBufferPool(BM_BufferPool *const bm, const char *const pageFileName, 
		  const int numPages, ReplacementStrategy strategy, 
		  void *stratData){


   //check if the file is OK to open
   //Using the storge manager
	SM_FileHandle *fHandle = (SM_FileHandle *)malloc(SM_FileHandle);

	RC ret = openPageFile(pageFileName, fHandle);
	if (ret != RC_OK) {
		return ret;
	}

	TIMER = 0;	//global timer set as 0

    //Initial an array to store the BM frames
    BM_FrameHandle *BM_frames = (BM_FrameHandle *)malloc(sizeof(BM_FrameHandle) * numPages);
    for (int i=0; i<numPages; i++){
        
        //repeatedly create new memory space to filepage and assign it to the frames
        SM_PageHandle fpage = (SM_PageHandle)malloc(PAGE_SIZE * sizeof(char));
        memset(fpage, '\0', PAGE_SIZE);		/* intialize the page with '\0' */
        
        //BM_frames[i].pageNum = NO_PAGE;   //at beginning, set all page number to be -1.
        //BM_frames[i].data = fpage;
        BM_frames[i]->pgdata.pageNum = NO_PAGE;
        BM_frames[i]->pgdata.data = fpage;
        BM_frames[i]->fix_count = 0;
        BM_frames[i]->is_dirty = 0;

        load_time = 0;
        used_time = 0;

    
    }
    
    //Using mgmtData to store the bookkeeping info
    BM_MgmtData *mgmtData = (BM_MgmtData *)malloc(sizeof(BM_MgmtData));
    mgmtData -> fileHandle = fHandle;
    mgmtData -> frames = BM_frames;
    mgmtData -> read_times = 0;
    mgmtData -> write_times =0;

    //initialize mapping array, added by Linlin
    mgmtData->page2frame = (int *) malloc (sizeof(int) * fHandle->totalNumPages);
    for (int i = 0; i < fHandle->totalNumPages; i++) {
    	mgmtData->page2frame = NO_PAGE;		//initialize with -1 for every page
    }
    mgmtData->frame2page = (int *) malloc (sizeof(int) * numPages);
    for (int i = 0; i < numPages; i++) {
    	mgmtData->frame2page = NO_PAGE;		//initialize with -1 for every frame
    }
    
    bm -> mgmtData = mgmtData;

	//Initial the others
	bm -> pagefile = pageFileName;
	bm -> numPages = numPages;
	bm -> strategy = strategy;

	return RC_OK;

}

/********************************************************
 shutdownBufferPool()
 Xiaolin Hu
 *********************************************************/
RC shutdownBufferPool(BM_BufferPool *const bm){
    
    //bugs here, should be pointer, fixed by Linlin
    BM_MgmtData *mgmtData = (BM_MgmtData*)bm -> mgmtData; //need casting
    BM_FrameHandle *frames = mgmtData -> frames;
    
    SM_FileHandle *file = mgmtData -> fileHandle;
    int numPages = bm -> numPages;
    
    //Check if there's any pinned pages
    for(int i=0 ; i<numPages ; i++){
        if(frames[i].fix_count != 0)
            return RC_BM_PINNED；
    }
    //Force flush pool before destroy
    forceFlushPool(bm);
    
    //Free the malloc spaces in the Pool
    //
    //add some unfree spaces, by Linlin
    free(mgmtData->page2frame);
    free(mgmtData->frame2page);
    for (int i = 0; i < bm->numPages; i ++) {
    	free(frames[i]->pgdata.data);
    }

    free(frames);
    free(fHandle);
    free(mgmtData);
    
    return RC_OK;
    
}
/********************************************************
 forceFlushPool()
 Xiaolin Hu
 *********************************************************/
RC forceFlushPool(BM_BufferPool *const bm){
    //fix bugs here by Linlin
    BM_MgmtData *mgmtData = (BM_MgmtData*)bm -> mgmtData; //need casting
    BM_FrameHandle *frames = mgmtData -> frames;
    
    SM_FileHandle *file = mgmtData -> fileHandle;
    int numPages = bm -> numPages;
    
    for(int i=0 ; i<numPages ; i++){
        if(frames[i].fix_count == 0 && frames[i].is_dirty == true){
            int pageN = frames[i].pgdata.pageNum;
            SM_PageHandle page = frames[i].pgdata.data;
            writeBlock (pageN, file, page);  //write back the page to file
            frames[i].is_dirty=false;  //reset the dirty flag
            
            mgmtData -> write_times++; // accumulate the num of write times
        }
    }
    
    return RC_OK;
    
}




//Start Buffer Manager Interface Access Pages
/**
 * Linlin Chen
 * A20348195
 * lchen96@hawk.iit.edu
 */


/**
 * replace frame with FIFO
 * @param  bm      buffer manager
 * @param  page    page handle
 * @param  pageNum page num to be loaded
 * @return         frameNum to be replaced
 */
int replacewithFIFO (BM_BufferPool *const bm, BM_PageHandle *const page, const PageNumber pageNum) {
	int frameNum = -1;
	int loadtime = LARGENUM;
	BM_MgmtData *mgmtData = (BM_MgmtData *)bm->mgmtData;

	//firstly search whether if there exist one frame with no page in
	for (int i = 0; i < bm->numPages; i++) 
		if (mgmtData->frame2page[i] == NO_PAGE) {
			return i;	//find an ununsed frame, return the frame number
		}
	//fail to find any unused frame
	for (int i = 0; i < bm->numPages; i++) {
		//iterate the frames, search for the frame with 0 fix_count and smallest load_time value
		if (mgmtData->frames[i]->fix_count == 0) {	//no other client using this frame
			if (loadtime > bm->mgmtData->frames[i]->load_time) {	//smaller load_time
				loadtime = bm->mgmtData->frames[i]->load_time;
				frameNum = i;
			}
		}
	}
	if (frameNum == -1) 
		return -1;		//cannot find a frame with no client using 
	
	if (mgmtData->frames[frameNum]->is_dirty)	//dirty, write the contents back
		forcePage(bm, mgmtData->frames[frameNum]->pgdata);
		
	return frameNum;	//return the frameNum to be replaced


}

/**
 * replace frame with LRU 
 * @param  bm      buffer manager
 * @param  page    [description]
 * @param  pageNum [description]
 * @return         [description]
 */
RC replacewithLRU (BM_BufferPool *const bm, BM_PageHandle *const page, const PageNumber pageNum) {
	int frameNum = -1;
	int usedtime = LARGENUM;
	BM_MgmtData *mgmtData = (BM_MgmtData *) bm->mgmtData;

	//firstly search whether if there exist one frame with no page in
	for (int i = 0; i < bm->numPages; i++) 
		if (mgmtData->frame2page[i] == NO_PAGE) {
			return i;	//find an ununsed frame, return the frame number
		}
	//fail to find any unused frame
	for (int i = 0; i < bm->numPages; i++) {
		//iterate the frames, search for the frame with 0 fix_count and smallest used_time value
		if (mgmtData->frames[i]->fix_count == 0) {	//no other client using this frame
			if (usedtime > mgmtData->frames[i]->used_time) {	//smaller used_time
				usedtime = mgmtData->frames[i]->used_time;
				frameNum = i;
			}
		}
	}
	if (frameNum == -1) 
		return -1;		//cannot find a frame with no client using 
	
	if (mgmtData->frames[frameNum]->is_dirty) {	//dirty, write the contents back
		forcePage(bm, mgmtData->frames[frameNum]->pgdata);
		mgmtData->frames[frameNum]->is_dirty = false;
	}
		
	return frameNum;	//return the frameNum to be replaced
}

/**
 * pins the page with page number pageNum
 * @param  bm: the buffer manager pool
 * @param  page: the page stored the page file content
 * @return      any error code
 */
RC pinPage (BM_BufferPool *const bm, BM_PageHandle *const page, const PageNumber pageNum) {
	BM_MgmtData *mgmtData = (BM_MgmtData *) bm->mgmtData;	//casting the mgmtData

	int framenum = -1;	//store the frameNum to be replaced

	TIMER++;	//increment the global timer
	//look up the page2frame array, if it's not NO_PAGE, it means the page has already in the buffer
	if (mgmtData->page2frame[pageNum] != NO_PAGE) {
		frameNum = mgmtData->page2frame[pageNum];	//get the frame num storing this page
		mgmtData->frames[frameNum]->fix_count ++;
		mgmtData->frames[framenum]->used_time = TIMER;
	} else {	//page is not in frame, need to be loaded
		switch (bm->strategy) {
			case RS_FIFO:		//FIFO strategy to replace
				frameNum = replacewithFIFO(bm, page, pageNum);
				break;
			case RS_LRU:		//LRU strategy to replace
				frameNum = replacewithLRU(bm, page, pageNum);
				break;
			default:
				printf("unrecognizable replace strategy!")
				exit(1);
		}


		if (frameNum != -1) {	//sucessfully find a frame to be replaced 
			mgmtData->page2frame[pageNum] = frameNum;	//set the mappings for fast lookup
			mgmtData->frame2page[frameNum] = pageNum;

			//read the content from disk to the buffer
			readBlock(pageNum, mgmtData->fileHandle, mgmtData->frames[frameNum]->pgdata.data);
			mgmtData->frames[frameNum]->pgdata.pageNum = pageNum;	//set pageNum
			mgmtData->frames[frameNum]->fix_count ++;	//increase the number of using clients
			mgmtData->frames[frameNum]->is_dirty = false;	//loading page will not make the frame dirty
			mgmtData->frames[frameNum]->load_time = TIMER;	//set the load time of the frame
			mgmtData->frames[frameNum]->used_time = TIMER;	//set the used time of the frame

		} else {	//cannot find any frames to be replaced
			printf("All frames are being used by clients now!");
			return RC_PIN_NOUNUSED;
		}

	}
	//increment the number of read times
	mgmtData->read_times ++;

	//assign the value for pages, with its pageNum and content
	page->pageNum = pageNum;
	page->data = malloc(sizeof(char) * PAGE_SIZE);
	strcpy(page->data, mgmtData->frames[frameNum]->pgdata.data);

	return RC_OK;

}


/**
 * unpins the page page
 * @param  bm: the buffer manager pool
 * @param  page: the page handle, the pagenum will be used to figure out which page will be unpined
 * @return      error code
 */
RC unpinPage (BM_BufferPool *const bm, BM_PageHandle *const page) {
	BM_MgmtData *mgmtData = (BM_MgmtData *) bm->mgmtData;		//casting

	if (page->pageNum < 0 || page->pageNum > bm->numPages) {
		printf("ERROR: exceeding the maximum number of pages!\n");
		return RC_RANGE_ERROR;	//the pageNum is not correct, exceeding the numPages, or negative
	}

	if (mgmtData->frames[page->pageNum]->is_dirty) {	//the frame is dirty, write the ocntents back
		forcePage(bm, mgmtData->frames[page->pageNum]->pgdata);	//write back the contents
		mgmtData->frames[page->pageNum]->is_dirty = false;
	}
	mgmtData->frames[page->pageNum]->fix_count --;	//Decrement the fix_count by one
	return RC_OK;
}


/**
 * marks a page as dirty.
 * @param  bm: buffer manager
 * @param  page: page handle
 * @return    error code
 */
RC markDirty (BM_BufferPool *const bm, BM_PageHandle *const page) {
	BM_MgmtData *mgmtData = (BM_MgmtData *) bm->mgmtData;	//casting

	TIMER++;		//increment globle timer

	if (page->pageNum < 0 || page->pageNum > bm->numPages) {
		printf("ERROR: exceeding the maximum number of pages!\n");
		return RC_RANGE_ERROR;		//the pageNum is not correct, exceeding the numPages, or negative
	}

	if (mgmtData->frames[page->pageNum]->is_dirty){	//the frame to be marked as dirty is already diry, needs to write back the contents first
		forcePage(bm, mgmtData->frames[page->pageNum]->pgdata);		//write back the content
	}
	mgmtData->frames[page->pageNum]->is_dirty = true;	//mark the frame as dirty
	mgmtData->frames[page->pageNum]->used_time = TIMER;	//set used timer as current timer

	return RC_OK;
}


/**
 * write the current content of the page back to the page file on disk
 * @param  bm: the buffer manager
 * @param  page: the page handle
 * @return      error code
 */
RC forcePage (BM_BufferPool *const bm, BM_PageHandle *const page) {
	BM_MgmtData *mgmtData = (BM_MgmtData *) bm->mgmtData;	//casting

	mgmtData->write_times ++;	//increment the write times by one
	//write the data in pageNum's frame into the fileHandle, since all info are stored in bm, so directly call writeBlock function
	return writeBlock(page->pageNum, mgmtData->fileHandle, mgmtData->frames[page->pageNum]->pgdata.data);

}
//End Buffer Manager Interface Access Pages


