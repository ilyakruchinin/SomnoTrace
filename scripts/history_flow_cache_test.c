/* Differential tests against an independent one-second source oracle. */
#include "history_flow_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef HISTORY_FLOW_IO_TEST
static size_t io_seeks, io_reads, io_bytes;
int history_test_fseek(FILE *file, long offset, int origin_value)
{ ++io_seeks; return fseek(file, offset, origin_value); }
size_t history_test_fread(void *out, size_t size, size_t count, FILE *file)
{ ++io_reads; io_bytes += size * count; return fread(out, size, count, file); }
#endif

#define SECONDS 28813U
static int16_t samples[SECONDS][2];
static uint16_t source_flags;
static const int64_t origin = 1788213723456LL;
static void put16(unsigned char *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void put32(unsigned char *p, uint32_t v) { for(int i=0;i<4;++i)p[i]=v>>(8*i); }
static void put64(unsigned char *p, uint64_t v) { for(int i=0;i<8;++i)p[i]=v>>(8*i); }
static void write_source(const char *path)
{
    unsigned char h[28]={0};
    put32(h,0x534e5442U); h[4]=2; h[5]=1; h[6]=2; h[7]=2;
    put16(h+8,10); put16(h+10,source_flags); put64(h+12,origin); put32(h+20,SECONDS);
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(h,1,sizeof(h),f)==sizeof(h));
    assert(fwrite(samples,sizeof(samples),1,f)==1); assert(!fclose(f));
}
static uint32_t compare_verified(const char *path, int64_t begin, int64_t end,
                                 size_t n, bool verified)
{
    history_flow_bin_t got[480], expected[480];
    for(size_t b=0;b<n;++b) expected[b]=(history_flow_bin_t){.low=INT16_MAX,.high=INT16_MIN};
    uint32_t count=0;
#ifdef HISTORY_FLOW_IO_TEST
    io_seeks = io_reads = io_bytes = 0;
#endif
    assert(history_flow_cache_read(path,begin,end,n,got,verified,NULL,NULL,&count)==0);
#ifdef HISTORY_FLOW_IO_TEST
    if (begin == origin && end == origin + SECONDS*1000LL && n == 480) {
        fprintf(stderr,"8-hour Flow %s: seeks=%zu reads=%zu requested_bytes=%zu logical_records=%u\n",
                verified ? "warm" : "cold", io_seeks, io_reads, io_bytes, count);
        /* Baseline: 4873 warm seeks / 4875 reads. Bound both call churn and
         * prefetch amplification, not just the logical records counter. */
        assert(io_seeks < 1000 && io_reads < 1250);
        assert(io_bytes < 4 * sizeof(samples));
    }
#endif
    for(size_t i=0;i<SECONDS;++i) {
        int64_t t=origin+(int64_t)i*1000;
        if(t<begin || t>=end)continue;
        size_t b=(uint64_t)(t-begin)*n/(uint64_t)(end-begin);
        int16_t low=samples[i][0],high=samples[i][1];
        if(low==INT16_MIN)low=high;
        if(high==INT16_MIN)high=low;
        if(low==INT16_MIN)continue;
        if(low<expected[b].low)expected[b].low=low;
        if(high>expected[b].high)expected[b].high=high;
        ++expected[b].count;
    }
    for(size_t b=0;b<n;++b) {
        if(got[b].count!=expected[b].count || got[b].low!=expected[b].low || got[b].high!=expected[b].high) {
            fprintf(stderr,"bin %zu: count %u/%u low %d/%d high %d/%d\n",b,
                got[b].count,expected[b].count,got[b].low,expected[b].low,got[b].high,expected[b].high);
            abort();
        }
    }
    return count;
}
static uint32_t compare(const char *path, int64_t begin, int64_t end, size_t n)
{ return compare_verified(path, begin, end, n, false); }
static bool cancel(void *arg) { int *remaining=arg; return --*remaining<=0; }
int main(void)
{
    char dir[]="/tmp/somno-pyramid-XXXXXX";assert(mkdtemp(dir));
    char path[160],cached[180],tmp[190];
    snprintf(path,sizeof(path),"%s/flow_mm.snt",dir);
    snprintf(cached,sizeof(cached),"%s.fpy",path);
    snprintf(tmp,sizeof(tmp),"%s.tmp",cached);
    for(size_t i=0;i<SECONDS;++i) {
        samples[i][0]=-100-(i%37);samples[i][1]=100+(i%53);
        if(i%113==0)samples[i][1]=31000; /* narrow peaks */
        if(i%227==0)samples[i][0]=-32000;
        if((i>610 && i<891) || i%331==0) samples[i][0]=samples[i][1]=INT16_MIN;
        if(i%997==0)samples[i][0]=INT16_MIN; /* one valid extremum */
    }
    write_source(path);
    history_flow_bin_t out[480];
    assert(history_flow_cache_read(path,origin,origin+10000,480,out,false,NULL,NULL,NULL)==1);
    int remaining=6;
    assert(history_flow_cache_build(path,cancel,&remaining)==-1);
    assert(access(cached,F_OK)!=0 && access(tmp,F_OK)!=0);
    assert(history_flow_cache_build(path,NULL,NULL)==0);
    uint32_t reads=compare(path,origin,origin+SECONDS*1000LL,480);
    printf("8-hour Flow: %u logical records vs %u one-second records\n",reads,SECONDS);
    assert(reads<SECONDS/2);
    assert(compare_verified(path,origin,origin+SECONDS*1000LL,480,true)==reads);
    compare(path,origin,origin+SECONDS*1000LL,90); /* 64s level */
    for(size_t i=0;i<35;++i) {
        int64_t left=origin-1333+(int64_t)i*71027;
        compare(path,left,left+993000+i*337,480);
    }
    compare(path,origin+590500,origin+900100,480); /* missing run and subsecond edges */
    compare(path,origin+SECONDS*1000LL-9123,origin+SECONDS*1000LL+15432,480);
    /* Deterministic varied bin widths and edge alignments exercise buffer
     * crossings at all pyramid levels, including warm verified reads. */
    uint32_t random=17;
    for (size_t i=0;i<80;++i) {
        random=random*1664525U+1013904223U;
        int64_t left=origin+(random%(SECONDS*1000U));
        random=random*1664525U+1013904223U;
        int64_t span=1+random%(SECONDS*1000U);
        compare_verified(path,left,left+span,1+random%480,true);
    }
    remaining=4;
    assert(history_flow_cache_read(path,origin,origin+SECONDS*1000LL,480,out,true,cancel,&remaining,NULL)==-1);
    /* Same size/header/FAT timestamp rewrite is caught by the per-generation
     * full source fingerprint, rather than trusting stat metadata alone. */
    struct stat old;assert(!stat(path,&old));
    samples[2000][1]=32700;source_flags=1;write_source(path);
    struct timespec times[2]={{old.st_atime,0},{old.st_mtime,0}};
    assert(!utimensat(AT_FDCWD,path,times,0));
    assert(history_flow_cache_read(path,origin,origin+SECONDS*1000LL,480,out,false,NULL,NULL,NULL)==1);
    assert(history_flow_cache_build(path,NULL,NULL)==0);
    compare(path,origin,origin+SECONDS*1000LL,480);
    /* Corrupt a used record then truncate the cache. Neither is publishable. */
    FILE *f=fopen(cached,"r+b");assert(f);assert(!fseek(f,100,SEEK_SET));
    int c=fgetc(f);assert(!fseek(f,100,SEEK_SET));fputc(c^0x7f,f);fclose(f);
    assert(history_flow_cache_read(path,origin,origin+64000,1,out,true,NULL,NULL,NULL)==1);
    assert(history_flow_cache_build(path,NULL,NULL)==0);
    assert(!truncate(cached,111));
    assert(history_flow_cache_read(path,origin,origin+64000,1,out,true,NULL,NULL,NULL)==1);
    assert(history_flow_cache_build(path,NULL,NULL)==0);
    compare(path,origin,origin+SECONDS*1000LL,480);
    unlink(cached);unlink(path);rmdir(dir);
    puts("Flow pyramid: peaks, gaps, edges, cancellation, stale source and corrupt cache passed");
}
