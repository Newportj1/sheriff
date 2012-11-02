// -*- C++ -*-

#ifndef SHERIFF_XPERSIST_H
#define SHERIFF_XPERSIST_H

#include <set>
#include <list>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "mm.h"
#include "wordchangeinfo.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

#include "atomic.h"
#include "ansiwrapper.h"
#include "freelistheap.h"

#include "stlallocator.h"
#include "privateheap.h"
#include "xplock.h"
#include "xdefines.h"
#include "xpageentry.h"
#include "xpagestore.h"

#ifdef GET_CHARACTERISTICS
#include "xpageprof.h"
#endif


#include "xtracker.h"
#include "xheapcleanup.h"
#include "stats.h"

#if defined(sun)
extern "C" int madvise(caddr_t addr, size_t len, int advice);
#endif

/**
 * @class xpersist
 * @brief Makes a range of memory persistent and consistent.
 *
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 */
template <class Type,
   unsigned long NElts = 1>
class xpersist {
public:
  typedef std::pair<int, void *> objType;
  typedef HL::STLAllocator<objType, privateheap> dirtyListTypeAllocator;
  typedef std::less<int> localComparator;
  /// A map of pointers to objects and their allocated sizes.
  typedef std::map<int, void *, localComparator, dirtyListTypeAllocator> dirtyListType;

  /// @arg startaddr  the optional starting address of local memory.
  /// @arg startsize  the optional size of local memory.
  xpersist (void * startaddr = 0, 
	    size_t startsize = 0)
    : _startaddr (startaddr),
      _startsize (startsize)
  {
    if (_startsize > 0) {
      if (_startsize > NElts * sizeof(Type)) {
        fprintf (stderr, "This persistent region (%d) is too small (%d).\n",
		 NElts * sizeof(Type), _startsize);
        ::abort();
      }
    }
    
    // Get a temporary file name (which had better not be NFS-mounted...).
    char _backingFname[L_tmpnam];
    sprintf (_backingFname, "/tmp/sheriff-backing-XXXXXX");
    _backingFd = mkstemp (_backingFname);
    if (_backingFd == -1) {
      fprintf (stderr, "Failed to make persistent file.\n");
      ::abort();
    }

    // Set the files to the sizes of the desired object.
    if (ftruncate (_backingFd,  NElts * sizeof(Type))) { 
      fprintf (stderr, "Mysterious error with ftruncate.\n");
      ::abort();
    }

    // Get rid of the files when we exit.
    unlink (_backingFname);

    //
    // Establish two maps to the backing file.
    //
    // The persistent map is shared.
    _persistentMemory
      = (Type *) MM::allocateShared (NElts * sizeof(Type), _backingFd);

    if (_persistentMemory == MAP_FAILED) {
      fprintf (stderr, "Failed to allocate memory (%u).\n", NElts * sizeof(Type));
      ::abort();
    }

    // If we specified a start address (globals), copy the contents into the
    // persistent area now because the transient memory map is going
    // to squash it.
    if (_startaddr) {
      memcpy (_persistentMemory, _startaddr, _startsize);
      _startsize = (_startsize / xdefines::PageSize + 1) * xdefines::PageSize;
      _isHeap = false;
    }
    else {
      _isHeap = true;
    }
  
    // The transient map is optionally fixed at the desired start
    // address. If globals, then startaddr is not zero.
    _transientMemory
      = (Type *) MM::allocateShared (NElts * sizeof(Type), _backingFd, startaddr);

    _isProtected = false;
  
#ifndef NDEBUG
    //fprintf (stderr, "transient = %p, persistent = %p, size = %lx\n", _transientMemory, _persistentMemory, NElts * sizeof(Type));
#endif

    _cacheLastthread = (unsigned long *)
      MM::allocateShared (TotalCacheNums * sizeof(unsigned long));
  
    _cacheInvalidates = (unsigned long *)
      MM::allocateShared (TotalCacheNums * sizeof(unsigned long));

    // How many users can be in the same page. We only start to keep track of 
    // wordChanges when there are multiple user in the same page.
    _pageUsers = (unsigned long *)
      MM::allocateShared (TotalPageNums * sizeof(unsigned long));

    // This is used to save all wordchange information about one page. 
    _wordChanges = (wordchangeinfo *)
      MM::allocateShared (TotalWordNums * sizeof(wordchangeinfo));

    if ((_transientMemory == MAP_FAILED) ||
	      (_persistentMemory == MAP_FAILED) ) {
      fprintf(stderr, "mmap error with %s\n", strerror(errno));
      // If we couldn't map it, something has seriously gone wrong. Bail.
      ::abort();
    }
  
    if(_isHeap) {
      xheapcleanup::getInstance().storeProtectHeapInfo
	                ((void *)_transientMemory, size(),
	                (void *)_cacheInvalidates, (void *)_wordChanges);
    }

#ifdef SSE_SUPPORT
    // A string of one bits.
    allones = _mm_setzero_si128();
    allones = _mm_cmpeq_epi32(allones, allones);
#endif 
  }

  virtual ~xpersist (void) {
#if 0
    close (_backingFd);
    // Unmap everything.
    munmap (_transientMemory,  NElts * sizeof(Type));
    munmap (_persistentMemory, NElts * sizeof(Type));
#endif
  }

  void initialize(void) {
    _privatePagesList.clear();
    _savedPagesList.clear();
  }

  void finalize(void *end) {
    //closeProtection();
  #ifdef TRACK_ALL_WRITES
    // We will check those memory writes from the beginning, if one callsite are captured to 
    // have one bigger updates, then report that.
    _tracker.checkWrites((int *)base(), size(),  _wordChanges); 
  #endif

    if(!_isHeap) {
      _tracker.checkGlobalObjects(_cacheInvalidates, (int *)base(), size(), _wordChanges); 
    }
    else {
      _tracker.checkHeapObjects(_cacheInvalidates, (int *)base(), (int *)end, _wordChanges);  
    }

    // printf those object information.
    if(_isHeap) {
      _tracker.print_objects_info();
    }
  }


  void sharemem_write_word(void * addr, unsigned long val) {
    unsigned long offset = (intptr_t)addr - (intptr_t)base();
    *((unsigned long *)((intptr_t)_persistentMemory + offset)) = val;
    return;
  }

  unsigned long sharemem_read_word(void * addr) {
    unsigned long offset = (intptr_t)addr - (intptr_t)base();
    return *((unsigned long *)((intptr_t)_persistentMemory + offset));
  }


  /**
   * We need to change mapping for the transient mapping,
   * thus, all following functions are working on _backingFd and 
   * all are working on specified address .
   */
  void * changeMappingToShared(int protInfo, void * start, size_t sz) {
    int  offset = (intptr_t)start - (intptr_t)base();
    return changeMapping(true, protInfo, start, sz, offset);
  }

  void * changeMappingToPrivate(int protInfo, void * start, size_t sz) {
    int  offset = (intptr_t)start - (intptr_t)base();
    return changeMapping(false, protInfo, start, sz, offset);
  }
  
  void * changeMapping (bool isShared, int protInfo, void * start,
            size_t sz, int offset)
  {
    int sharedInfo = isShared ? MAP_SHARED : MAP_PRIVATE;
    sharedInfo     |= MAP_FIXED ;
   
    return mmap (start, sz, protInfo,
                 sharedInfo, _backingFd, offset);
  }
  
  
  void* mmapRdPrivate(void * start, unsigned long size) {
    void * ptr;
    // Map to readonly private area.
    ptr = changeMappingToPrivate(PROT_READ, start, size); 
    if(ptr == MAP_FAILED) {
      fprintf(stderr, "Weird, %d can't map to read and private area\n", getpid());
      exit(-1);
    }
    return ptr;
  }

  // Set a page to be read-only but shared
  void * mmapRdShared(int pageNo) {
    void * start = (void *)((intptr_t)base() + pageNo * xdefines::PageSize);
    void * ptr;

    // Map to writable share area. 
    ptr = changeMappingToShared(PROT_READ, start, size); 
    if(ptr == MAP_FAILED) {
      fprintf(stderr, "Weird, %d remove protect failed!!!\n", getpid());
      exit(-1);
    }
    return (ptr);
  }

  // Set a page to be read-only but shared
  void * mmapRdShared(void * start) {
    void * ptr;

    // Map to writable share area. 
    ptr = changeMappingToShared(PROT_READ, start, size); 
    if(ptr == MAP_FAILED) {
      fprintf(stderr, "Weird, %d remove protect failed!!!\n", getpid());
      exit(-1);
    }
    return (ptr);
  }

  /// Set a block of memory to Readable/Writable and shared. 
  void *mmapRwShared(void * start, unsigned long size) {
    void * ptr;

    // Map to writable share area. 
    ptr = changeMappingToShared(PROT_READ|PROT_WRITE, start, size); 
    if(ptr == MAP_FAILED) {
      fprintf(stderr, "Weird, %d remove protect failed!!!\n", getpid());
      exit(-1);
    }
    return (ptr);
  }

  // We set the attribute to Private and Readable
  void openProtection (void) {
    mmapRdPrivate(base(), size());
    _isProtected = true;
  }

  void closeProtection(void) {
    mmapRwShared(base(), size());
    _isProtected = false;
  }
  
  int getDirtyPages(void) {
    return _privatePagesList.size();
  }
 
  // Cleanup those counter information about one heap object when one object is re-used.
  bool cleanupHeapObject(void * ptr, size_t sz) {
    int offset;
    int cachelines;
    int index;
  
    assert(_isHeap == true);
  
    if(inRange(ptr) == false) {
      return false;
    }
    
    offset = (int)ptr - (int)base();
    index = offset/xdefines::CACHE_LINE_SIZE;
  
    // At least we will check one cache line.
    cachelines = sz/xdefines::CACHE_LINE_SIZE;  
    if(cachelines == 0)
      cachelines = 1;
    
    // Cleanup the cacheinvalidates that are involved in this object.
    for(int i = index; i < index+cachelines; i++) {
      if(_cacheInvalidates[i] >= xdefines::MIN_INVALIDATES_CARE) {
        return false;
      }
      // We don't need atomic operation here.
      _cacheInvalidates[i] = 0; 
    } 
  
    // Cleanup the wordChanges
    void * wordptr = (void *)&_wordChanges[offset/sizeof(unsigned long)];
    memset(wordptr, 0, sz);
  
    return true;
  }

  /// @return true iff the address is in this space.
  inline bool inRange (void * addr) {
    if (((size_t) addr >= (size_t) base())
      && ((size_t) addr < (size_t) base() + size())) {
      return true;
    } else {
      return false;
    }
  }


  /// @return the start of the memory region being managed.
  inline Type * base (void) const {
    return _transientMemory;
  }

  /// @return the size in bytes of the underlying object.
  inline unsigned long size (void) const {
    if(_isHeap) 
      return NElts * sizeof(Type);
    else
      return _startsize;
  }

  inline void addPageEntry(int pageNo, struct pageinfo * curPage, dirtyListType * pageList) {
    pair<dirtyListType::iterator, bool> it;
      
    it = pageList->insert(pair<int, void *>(pageNo, curPage));
    if(it.second == false) {
      // The element is existing in the list now.
      memcpy((void *)it.first->second, curPage, sizeof(struct pageinfo));
    }
    return;                                                                       
  }

  /// @brief Handle the write operation on a page.
  /// For detection, we will try to get a twin page.
  void handleWrite (void * addr) {
    // Compute the page that holds this address.
    unsigned long * pageStart = (unsigned long *) (((intptr_t) addr) & ~(xdefines::PAGE_SIZE_MASK));

    // Unprotect the page and record the write.
    mprotect ((char *) pageStart, xdefines::PageSize, PROT_READ | PROT_WRITE);
  
    // Compute the page number of this item
    int pageNo = computePage ((size_t) addr - (size_t) base());
 
    // Get an entry from page store.
    struct pageinfo * curPage = xpageentry::getInstance().alloc();
    curPage->pageNo = pageNo;
    curPage->pageStart = (void *)pageStart;
    curPage->alloced = false;

    // Force the copy-on-write of kernel by writing to this address directly
    // Using assemly language here to avoid the code to be optimized.
    // That is, pageStart[0] = pageStart[0] can be optimized to "nop"
 #if defined(X86_32BIT)
    asm volatile ("movl %0, %1 \n\t"
                  :   // Output, no output 
                  : "r"(pageStart[0]),  // Input 
                    "m"(pageStart[0])
                  : "memory");
 #else
    asm volatile ("mov %0, %1 \n\t"
                  :   // Output, no output 
                  : "r"(pageStart[0]),  // Input 
                    "m"(pageStart[0])
                  : "memory");
  #endif

    // Create the "origTwinPage" from the transient page.
    memcpy(curPage->origTwinPage, pageStart, xdefines::PageSize);

    // We will update the users of this page.
    int origUsers = atomic::increment_and_return(&_pageUsers[pageNo]);
    if(origUsers != 0) {
      // Set this page to be shared by current thread.
      // But we don't need to allocate temporary twin page for this page
      // if current transaction is too short, then there is no need to do that.
      curPage->shared = true;
    }
    else {
      curPage->shared = false;
    }

    // Add this entry to dirtiedPagesList.
    addPageEntry(pageNo, curPage, &_privatePagesList);
  }

  inline void allocResourcesForSharedPage(struct pageinfo * pageinfo) {
    // Alloc those resources for share page.
    pageinfo->wordChanges = (unsigned long *)xpagestore::getInstance().alloc();
    pageinfo->tempTwinPage = xpagestore::getInstance().alloc();
    
    // Cleanup all word change information about one page
    memset(pageinfo->wordChanges, 0, xdefines::PageSize);
    pageinfo->alloced = true;
  }

  // In the periodically checking, we are trying to check all dirty pages  
  inline void periodicCheck(void) {
    struct pageinfo * pageinfo;
    int pageNo;
    bool createTempPage = false;

    for (dirtyListType::iterator i = _privatePagesList.begin(); i != _privatePagesList.end(); i++) {
      pageinfo = (struct pageinfo *)i->second;
      pageNo = pageinfo->pageNo;

      // If the original page is not shared, now we can check again.
      if(pageinfo->shared != true) {
        // Check whether one un-shared page becomes shared now?
        int curUsers = atomic::atomic_read(&_pageUsers[pageNo]);
        if(curUsers == 1) {
          // We don't care those un-shared page.
          continue;
        }
        else {
          pageinfo->shared = true; 
        }
      }

      //printf("%d: period checking on pageNo %d\n", getpid(), pageNo);

      // now all pages should be shared.
      assert(pageinfo->shared == true);
      
      if(pageinfo->shared == true) {
        // Check whether we have allocated the temporary page or not.
        if(pageinfo->alloced == false) {
          allocResourcesForSharedPage(pageinfo);

          // Create the temporary page by copying from the working version.
          createTempPage = true;
        }

        // We will try to record changes for those shared pages
        recordChangesAndUpdate(pageinfo, createTempPage);
      }
    }
  }

  inline int recordCacheInvalidates(int pageNo, int cacheNo) {
    int myTid = getpid();
    int lastTid;
    int interleaving = 0;

    // Try to check the global array about cache last thread id.
    lastTid = atomic::exchange(&_cacheLastthread[cacheNo], myTid);

    //fprintf(stderr, "Record cache interleavings, lastTid %d and myTid %d\n", lastTid, myTid);
    if(lastTid != 0 && lastTid != myTid) {
      // If the last thread to invalidate cache is not current thread, then we will update global
      // counter about invalidate numbers.
      atomic::increment(&_cacheInvalidates[cacheNo]);
     // fprintf(stderr, "Record cache invalidates %p with interleavings %d\n", &_cacheInvalidates[cacheNo], _cacheInvalidates[cacheNo]);
      interleaving = 1;
    }
    return interleaving;
  }
  
  // Record changes for those shared pages and update those temporary pages.
  inline void recordChangesAndUpdate(struct pageinfo * pageinfo, bool createTempPage) {
    int myTid = getpid();
    unsigned long * local = (unsigned long *)pageinfo->pageStart;
    //printf("%d: before record on pageNo %d createTempPage %d\n", getpid(), pageinfo->pageNo, createTempPage);

    // Which twin page should we compared this time. 
    unsigned long * twin;
  
    if(createTempPage) {
      // Compare the original twin page with local copy
      twin = (unsigned long *)pageinfo->origTwinPage;
   
      // We copy the page from the working copy
      memcpy(twin, local, xdefines::PageSize);
    }
    else {
      // Compare the temporary twin page with local copy
      twin = (unsigned long *)pageinfo->tempTwinPage;
    }
  
    //printf("%d: record on pageNo %d createTempPage %d\n", getpid(), pageinfo->pageNo, createTempPage);
 
    unsigned long * wordChanges;
    unsigned long interWrites = 0;
    wordChanges = (unsigned long *)pageinfo->wordChanges;
  
    // We will check those modifications by comparing "local" and "twin".
    unsigned long cacheNo;
    int startCacheNo = pageinfo->pageNo*xdefines::CACHES_PER_PAGE;
    unsigned long recordedCacheNo = 0xFFFFFFFF;

    for(int i = 0; i < xdefines::PageSize/sizeof(unsigned long); i++) {
      if(local[i] != twin[i]) {
        int lastTid;
    
        // Calculate the cache number for current words.  
        cacheNo = i >> 4;
        
        // We will update corresponding cache invalidates.
        if(cacheNo != recordedCacheNo) {
          interWrites += recordCacheInvalidates(pageinfo->pageNo, 
                            pageinfo->pageNo*xdefines::CACHES_PER_PAGE + cacheNo);
          recordedCacheNo = cacheNo;
        }
        
        // Update words on twin page if we are comparing against temporary twin page.
        // We can't update the original twin page!!! That is a bug.
        if(createTempPage == false) {
          twin[i] = local[i];
        }
        
        // Record changes for words in this cache line.
        wordChanges[i]++; 
      }   
    }
  }

  /// @brief Start a transaction.
  inline void begin (void) {
    updateAll();
  }

  // Use vectorization to improve the performance if we can.
  inline void commitPageDiffs (const void * local, const void * twin, int pageNo) {
    void * dest = (void *)((intptr_t)_persistentMemory + xdefines::PageSize * pageNo);
  #ifdef SSE_SUPPORT
    __m128i * localbuf = (__m128i *) local;
    __m128i * twinbuf  = (__m128i *) twin;
    __m128i * destbuf  = (__m128i *) dest;
    for (int i = 0; i < xdefines::PageSize / sizeof(__m128i); i++) {
  
      __m128i localChunk, twinChunk, destChunk;
  
      localChunk = _mm_load_si128 (&localbuf[i]);
      twinChunk  = _mm_load_si128 (&twinbuf[i]);
  
      // Compare the local and twin byte-wise.
      __m128i eqChunk = _mm_cmpeq_epi8 (localChunk, twinChunk);
  
      // Invert the bits by XORing them with ones.
      __m128i neqChunk = _mm_xor_si128 (allones, eqChunk);
  
      // Write local pieces into destbuf everywhere diffs.
      _mm_maskmoveu_si128 (localChunk, neqChunk, (char *) &destbuf[i]);
    }
  #else
    /* If hardware can't support SSE3 instructions, use slow commits as following. */
    unsigned long * mylocal = (unsigned long *)local;
    unsigned long * mytwin = (unsigned long *)twin;
    unsigned long * mydest = (unsigned long *)dest;

    for(int i = 0; i < xdefines::PageSize/sizeof(unsigned long); i++) {
      if(mylocal[i] != mytwin[i]) {
          //if(mytwin[i] != mydest[i] && mylocal[i] != mydest[i])
          //fprintf(stderr, "%d: RACE at %p from %x to %x (dest %x). pageno %d\n", getpid(), &mylocal[i], mytwin[i], mylocal[i], mydest[i], pageno);
        mydest[i] = mylocal[i];
      }
    }

  #endif
  }

  inline void checkCommitWord(char * local, char * twin, char * share) {
    int i = 0;
    while(i < sizeof(unsigned long)) {
      if(local[i] != twin[i]) {
        share[i] = local[i];
      }
      i++;
    }
  }

  inline void recordWordChanges(void * addr, unsigned long changes) {
    wordchangeinfo * word = (wordchangeinfo *)addr;
    unsigned short tid = word->tid;
  
    int mine = getpid();
  
    // If this word is not shared, we should set to current thread.
    if(tid == 0) {
      word->tid = mine;
    }
    else if (tid != 0 && tid != mine && tid != 0xFFFF) {
      // This word is shared by different threads. Set to 0xFFFF.
      word->tid = 0xFFFF;
    }
  
    word->version += changes;
  }

  // Normal commit procedure. All local modifications should be commmitted to the shared mapping so
  // that other threads can see this change. 
  // Also, all wordChanges has be integrated to the global place too.
  inline void checkcommitpage(struct pageinfo * pageinfo) {
    unsigned long * twin = (unsigned long *) pageinfo->origTwinPage;
    unsigned long * local = (unsigned long *) pageinfo->pageStart; 
    unsigned long * share = (unsigned long *) ((intptr_t)_persistentMemory + xdefines::PageSize * pageinfo->pageNo);
    unsigned long * tempTwin = (unsigned long *) pageinfo->tempTwinPage;
    unsigned long * localChanges = (unsigned long *) pageinfo->wordChanges;
    // Here we assume sizeof(unsigned long) == 2 * sizeof(unsigned short);
    unsigned long * globalChanges = (unsigned long *)((unsigned long)_wordChanges + xdefines::PageSize * pageinfo->pageNo);
    unsigned long recordedCacheNo = 0xFFFFFFFF;
    unsigned long cacheNo;
    unsigned long interWrites = 0;

    // Now we have the temporary twin page and original twin page.
    // We always commit those changes against the original twin page.
    // But we need to capture the changes since last period by checking against 
    // the temporary twin page.  
    for (int i = 0; i < xdefines::PageSize/sizeof(unsigned long); i++) {
      if(local[i] == twin[i]) {
        if(localChanges[i] != 0) {
          //fprintf(stderr, "detect the ABA changes %d\n", localChanges[i]);
          recordWordChanges((void *)&globalChanges[i], localChanges[i]);
        }
        // It is very unlikely that we have ABA changes, so we don't check
        // against temporary twin page now.
        continue;
      }

      // Now there are some changes, at least we must commit the word.
      if(local[i] != tempTwin[i]) {
        // Calculate the cache number for current words.    
        cacheNo = i >> 4;

        // We will update corresponding cache invalidates.
        if(cacheNo != recordedCacheNo) {
          interWrites += recordCacheInvalidates(pageinfo->pageNo, 
                          pageinfo->pageNo*xdefines::CACHES_PER_PAGE + cacheNo);

          recordedCacheNo = cacheNo;
        }

        recordWordChanges((void *)&globalChanges[i], localChanges[i] + 1);
      }
      else {
        recordWordChanges((void *)&globalChanges[i], localChanges[i]);
      }

      // Now we are doing a byte-by-byte based commit
      checkCommitWord((char *)&local[i], (char *)&twin[i], (char *)&share[i]);
    }
  }

  // Update those continuous pages.
  inline void updateBatchedPages(int batchedPages, void * batchedStart) {
    updatePages(batchedStart, batchedPages * xdefines::PageSize);
  }

  // Update all pages in the beginning of each transaction.
  // By postpone those page updates, we can possibly improve the 
  // parallelism for those critical sections. Now updateAll
  // are done outside the critical section.
  void updateAll(void) {
    // Don't need to commit a page if no pages in the writeset.
    if(_privatePagesList.size() == 0) {
      return;
    }

    // We are trying to batch those system calls
    int    pageNo;
    void * batchedStart = NULL;
    int    batchedPages = 0;
    
    // We are setting lastpage to a impossible page no at first.
    unsigned int lastpage = 0xFFFFFFF0;
    struct pageinfo * pageinfo;

    // Check every pages in the private pages list.
    for (dirtyListType::iterator i = _privatePagesList.begin(); i != _privatePagesList.end(); ++i) {
      pageNo = i->first;
      pageinfo = (struct pageinfo *)i->second;
    
    //  fprintf(stderr, "Inside the loop with pageNo %d\n", pageNo); 
      // Check whether current page is continuous with previous page. 
      if(pageNo == lastpage + 1) {
        batchedPages++;
      }
      else {
        if(batchedPages > 0) {
          // We will update batched pages together.
          // By doing this, we may save the overhead of system calls (madvise and mprotect).  
          updateBatchedPages(batchedPages, batchedStart);
        }

        batchedPages = 1;
        batchedStart = pageinfo->pageStart;
     //   fprintf(stderr, "Now pageNo is %d and batchedStart is %p\n", pageNo, batchedStart);
        // Now we set last page to current pageNo.
        lastpage = pageNo;
      }
    }

    // Now we have to commit again since the last part won't be committed.      
    if(batchedPages > 0) {
      updateBatchedPages(batchedPages, batchedStart);
    }

    //fprintf(stderr, "COMMIT-BEGIN at process %d\n", getpid());
    // Now we already finish the commit, let's cleanup the list. 
    // For every transaction, we will restart to capture those writeset
    _privatePagesList.clear();

    // Clean up those page entries.
    xpageentry::getInstance().cleanup();
    xpagestore::getInstance().cleanup();
  }

  // Commit those pages in the end of each transaction. 
  inline void commit(bool doChecking) {
    // Don't need to commit a page if no pages in the writeset.
    if(_privatePagesList.size() == 0) {
      return;
    }

    // Commit those private pages. 
    struct pageinfo * pageinfo = NULL;
    int    pageNo;
    // Check every pages in the private pages list.
    for (dirtyListType::iterator i = _privatePagesList.begin(); i != _privatePagesList.end(); ++i) {
      pageinfo = (struct pageinfo *)i->second;
      pageNo = pageinfo->pageNo;
 
    //  fprintf(stderr, "COMMIT: %d on page %d on heap %d\n", getpid(), pageNo, _isHeap);
 
      // If a page is shared and there are some wordChanges information,
      // We should commit the changes and update wordChanges information too.
      if((pageinfo->shared == true) && (pageinfo->alloced == true)) { 
        checkcommitpage(pageinfo);
        //fprintf(stderr, "%d COMMIT: finish on page %d\n", getpid(), pageNo);
      }
      else {
        // Commit those changes by checking the original twin page.
        commitPageDiffs(pageinfo->pageStart, pageinfo->origTwinPage, pageNo);
      }
    }
  //  fprintf(stderr, "COMMIT: %d finish commits on heap %d\n", getpid(), _isHeap);
  }

  /// @brief Commit all writes.
  inline void memoryBarrier (void) {
    atomic::memoryBarrier();
  }

private:

  inline int computePage (int index) {
    return (index * sizeof(Type)) / xdefines::PageSize;
  }

  /// @brief Update the given page frame from the backing file.
  void updatePages (void * local, int size) {
    madvise (local, size, MADV_DONTNEED);

    //fprintf(stderr, "%d: current copy at %p is discarded with size %d\n", getpid(), local, size);
    // Set this page to PROT_READ again.
    mprotect (local, size, PROT_READ);
  }
 
  /// True if current xpersist.h is a heap.
  bool _isHeap;

  /// The starting address of the region.
  void * const _startaddr;

  /// The size of the region.
  size_t _startsize;

  /// A map of dirtied pages.
  dirtyListType _privatePagesList;

  dirtyListType _savedPagesList;
  /// The file descriptor for the backing store.
  int _backingFd;

  /// The transient (not yet backed) memory.
  Type * _transientMemory;

  /// The persistent (backed to disk) memory.
  Type * _persistentMemory;

  bool _isProtected;
  
  /// The file descriptor for the versions.
  int _versionsFd;

  //unsigned long * _globalSharedInfo;
  bool * _globalSharedInfo;
  bool * _localSharedInfo;

 
  /// The length of the version array.
  enum { TotalPageNums = NElts * sizeof(Type)/(xdefines::PageSize) };
  enum { TotalCacheNums = NElts * sizeof(Type)/(xdefines::CACHE_LINE_SIZE) };

  unsigned long * _cacheInvalidates;

  // Last thread to modify current cache
  unsigned long * _cacheLastthread;

  // A string of one bits.
  __m128i allones;
  
  enum { TotalWordNums = NElts * sizeof(Type)/sizeof(unsigned long) };
  
  // In order to save space, we will use the higher 16 bit to store the thread id
  // and use the lower 16 bit to store versions.
  wordchangeinfo * _wordChanges;

  // Keeping track of whether multiple users are on the same page.
  // If no multiple users simultaneously, then there is no need to check the word information, 
  // thus we don't need to pay additional physical pages on _wordChanges since
  // _wordChanges will double the physical pages's usage.
  unsigned long * _pageUsers;
 
  xtracker<NElts> _tracker;
};

#endif
