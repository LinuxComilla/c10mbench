#include "c10mbench.h"
#include "pixie-timer.h"
#include "pixie-threads.h"
#include "pixie-mem.h"
#include "rte-ring.h"
#include "pixie-strerror.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>

#ifdef WIN32
#include <process.h>
#include <Windows.h>
#include <WinError.h>
#define getpid _getpid
#define read _read 
#elif defined(__APPLE__)
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#else
#include <unistd.h>
#endif

#define BENCH_ITERATIONS2 ((BENCH_ITERATIONS)*10)

struct MainmemParms {
    uint64_t memsize;
    unsigned char *pointer;
    unsigned result;
    unsigned id;
    unsigned cpu_count;
};

/******************************************************************************
 * I step through memory using large primes in order to create an unpreditable
 * pattern of memory accesses.
 ******************************************************************************/
unsigned primes[] = {
    1003001,
    1008001,
    1114111,
    1221221,
    1333331,
    1411141,
    1422241,
    1444441,
    1508051,
    1551551,
    1600061,
    1611161,
    1628261,
    1633361,
    1646461,
    1660661,
    1707071,
    1777771,
    1881881,
    3007003,
    3331333,
    7118117,
    7722277,
    9002009,
    9110119,
    9200029,
    9222229,
    9332339,
    9338339,
    9400049,
    9440449,
    9700079,
};

/******************************************************************************
 * This thread maximizes the number of random memory accesses, maximizing
 * the number of simultaneous accesses and predictable accesses.
 ******************************************************************************/
static void
rate_thread(void *v_parms)
{
    size_t i;
    struct MainmemParms *parms = (struct MainmemParms *)v_parms;
    unsigned result = 0;
    unsigned char *pointer = parms->pointer;
    uint64_t offset = 0;
    uint64_t memsize = parms->memsize;
    unsigned jump = primes[parms->id];
    
    //pixie_cpu_set_affinity(parms->id);
    for (i=0; i<BENCH_ITERATIONS2; i += 1) {
        size_t offset2;
        size_t j;
        result += pointer[(size_t)offset];
        offset += jump;
        if (offset > memsize)
            offset -= memsize;
        
        offset2 = offset;
        for (j=0; j<4; j++) {
            offset2 += jump;
            if (offset2 > memsize)
                offset2 -= memsize;
            __builtin_prefetch(pointer + offset2);
        }
    }
    
    pixie_locked_add_u32(&parms->result, result);
    free(parms);
}

char * getPWHash () {
    long i ; char pwd [64];
    char * sha1 = ( char *) malloc (41);
    // read password
     fgets ( pwd , sizeof ( pwd ) , stdin );
     // calculate sha1 of password
    //..
     // overwrite pwd in memory
     // Alternative (1) : use memset
    memset ( pwd , 0 , sizeof ( pwd )); // (1)
     // Alternative (2) : reset pwd in a loop
     for ( i =0; i < sizeof ( pwd ); ++ i )
     pwd [ i ]=0; // (2)
     // return only hash of pwd
    return sha1;
}
    
/*
  3 3 3 3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 
  4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |C|D|R|bank | row                           | column            |x x x|
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
/******************************************************************************
 * This thread is designed for "pointer-chasing", so that out-of-order
 * processors cannot prefetch/predict future bits of data
 ******************************************************************************/
static void
chase_thread(void *v_parms)
{
    size_t i;
    struct MainmemParms *parms = (struct MainmemParms *)v_parms;
    unsigned result = 0;
    unsigned char *pointer = parms->pointer;
    uint64_t offset = 0;
    uint64_t memsize = parms->memsize;
    unsigned jump = primes[parms->id];

    //pixie_cpu_set_affinity(parms->id);
    for (i=0; i<BENCH_ITERATIONS2; i++) {
        result += pointer[(size_t)offset];
        offset += jump + pointer[(size_t)offset];
        while (offset > memsize)
            offset -= memsize;
    }
    
    pixie_locked_add_u32(&parms->result, result);
    free(parms);
}

static void
chase_thread2(void *v_parms)
{
    size_t i;
    struct MainmemParms *parms = (struct MainmemParms *)v_parms;
    unsigned result = 0;
    unsigned char *pointer = parms->pointer;
    uint64_t offset = 0;
    uint64_t memsize = parms->memsize;
    unsigned jump = primes[parms->id];
    
    //pixie_cpu_set_affinity(parms->id);
    for (i=0; i<BENCH_ITERATIONS2; i++) {
        size_t x = 0;
        result += i; //pointer[(size_t)offset];
        
        
        asm ("cmovne (%1,%2,1), %0"
             : "=c" (x)
             : "r" (pointer), "r" (offset) 
             );
         
        
        
        /*if (i >= 0)
            offset += pointer[(size_t)offset];*/
        offset += jump;
        offset += x;
        while (offset > memsize)
            offset -= memsize;
    }
    
    pixie_locked_add_u32(&parms->result, result);
    free(parms);
}


/******************************************************************************
 ******************************************************************************/
void
bench_mainmem(unsigned cpu_count, unsigned which_test)
{
    size_t i;
    struct MainmemParms parms[1];
    const char *test_name = "unknown";
    static const uint64_t minsize = 32*1024*1024;
    int is_huge;
    void (*worker)(void*);
   

    /*
     * We support 4 types of memory tests
     */
    switch (which_test) {
        case MemBench_PointerChase:
            test_name = "memchase";
            worker = chase_thread;
            is_huge = 0;
            break;
        case MemBench_PointerChaseHuge:
            test_name = "hugechase";
            worker = chase_thread;
            is_huge = 1;
            break;
        case MemBench_CmovChase:
            test_name = "cmovchase";
            worker = chase_thread2;
            is_huge = 0;
            break;
        case MemBench_CmovChaseHuge:
            test_name = "hugechase";
            worker = chase_thread2;
            is_huge = 1;
            break;
        case MemBench_MaxRate:
            test_name = "memrate";
            worker = rate_thread;
            is_huge = 0;
            break;
        case MemBench_MaxRateHuge:
            test_name = "hugerate";
            worker = rate_thread;
            is_huge = 1;
            break;
        default:
            fprintf(stderr, "%s:%u: unknown test\n", __FILE__, __LINE__);
            return;
    }
    

    memset(parms, 0, sizeof(parms[0]));
    parms->cpu_count = cpu_count;
    
    /* We choose 1/4 of the RAM by default */
    parms->memsize = pixie_get_memsize()/8;
    
    if (is_huge) {
        /* We are doing "huge" pages in this test, which is likely to fail
         * unless the user has recently rebooted */
        int err = 0;
        
        parms->memsize = pixie_align_huge(parms->memsize);
        
        parms->pointer = pixie_alloc_huge(parms->memsize, &err);

        switch (err) {
            case HugeErr_Success:
                break;
            case HugeErr_MemoryFragmented:
                fprintf(stderr, "%s: test not run: memory too fragmented (reboot)\n", test_name);
                return;
            case HugeErr_NoPermissions:
                fprintf(stderr, "%s: test not run: need SeLockMemoryPrivilege permission\n", test_name);
                return;
            default:
                fprintf(stderr, "%s: unknown error allocating huge pages\n", test_name);
                return;
        }
    } else {
        /* We aren't doing huge pages, so free the memory as normal */
        parms->pointer = (unsigned char*)malloc((size_t)parms->memsize);
        if (parms->memsize < minsize) {
            fprintf(stderr, "%s: test not run: buffer too small\n", test_name);
            return;
        }
    }

    /* Zero out the memory. This also has the effect of committing all the 
     * pages if they weren't already, as well as rev up the CPU to full
     * speed if it's in some sort of sleep state */
    fprintf(stderr, "memsizae = %llu\n", (uint64_t)parms->memsize);
    memset(parms->pointer, 0, (size_t)parms->memsize);

    
    
    for (i=0; i<cpu_count; i++) {
        unsigned j;
        double ellapsed;
        double speed;
        size_t thread_handles[256];
        size_t thread_count = 0;
        uint64_t start, stop;
        
        start = pixie_gettime();
        
        /* start the threads */
        for (j=0; j<=i; j++) {
            struct MainmemParms *parms2 = (struct MainmemParms*)malloc(sizeof(*parms2));
            parms->id = j;
            memcpy(parms2, parms, sizeof(parms2[0]));
            
            /* launch the worker thread to performa the benchmark */
            thread_handles[thread_count++] = pixie_begin_thread(worker, 0, parms2);
        }
        
        /* wait for all threads to complete their work */
        for (j=0; j<thread_count; j++)
            pixie_join(thread_handles[j], 0);
        stop = pixie_gettime();
        
        
        ellapsed = (stop-start)/1000000.0;
        speed = BENCH_ITERATIONS2*1.0/ellapsed;
        
        printf("%-12s, %2u,  %7.3f,  %7.3f,  %7.1f\n",
               test_name,
               (unsigned)thread_count,
               speed/1000000.0,
               1.0*thread_count*speed/1000000.0,               
               1000000000.0/speed);
        
    }

    /* 
     * Free the huge buffer we allocated 
     */
    if (is_huge)
        pixie_free_huge(parms->pointer, parms->memsize);
    else {
        free(parms->pointer);
    }
}

