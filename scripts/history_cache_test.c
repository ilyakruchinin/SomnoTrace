#include "history_cache.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static history_cache_key_t key(int window, int generation)
{
    history_cache_key_t k={.kind=3,.start_ms=window,.generation=generation};
    memcpy(k.day,"20260901",9);return k;
}
static void *hammer(void *arg)
{
    int value=(int)(intptr_t)arg;
    for(int i=0;i<500;++i) {
        history_cache_key_t k=key(i%5+value*10,3);
        history_cache_put(&k,&value,sizeof(value));
        int copy=0;
        if(history_cache_get(&k,&copy,sizeof(copy)))assert(copy==value);
    }
    return NULL;
}
int main(void)
{
    char *payload=malloc(HISTORY_CACHE_ITEM_BYTES+1), *out=malloc(HISTORY_CACHE_ITEM_BYTES);
    assert(payload && out);memset(payload,0x23,HISTORY_CACHE_ITEM_BYTES+1);
    history_cache_key_t a=key(1,1), b=key(2,1), c=key(3,1), d=key(4,1);
    history_cache_put(&a,payload,HISTORY_CACHE_ITEM_BYTES);
    history_cache_put(&b,payload,HISTORY_CACHE_ITEM_BYTES);
    history_cache_put(&c,payload,HISTORY_CACHE_ITEM_BYTES);
    assert(history_cache_get(&a,out,HISTORY_CACHE_ITEM_BYTES)); /* touch A so B is oldest */
    history_cache_put(&d,payload,HISTORY_CACHE_ITEM_BYTES);
    assert(!history_cache_get(&b,NULL,0));
    assert(history_cache_get(&a,NULL,0));
    assert(!history_cache_get(&a,out,12));
    history_cache_key_t too_big=key(5,1);
    history_cache_put(&too_big,payload,HISTORY_CACHE_ITEM_BYTES+1);
    assert(!history_cache_get(&too_big,NULL,0));
    history_cache_key_t changed=key(1,2);
    assert(!history_cache_get(&changed,NULL,0));
    history_cache_put(&changed,payload,12);
    assert(!history_cache_get(&a,NULL,0));
    changed.therapy_only=true;assert(!history_cache_get(&changed,NULL,0));
    changed.therapy_only=false;changed.signal=1;assert(!history_cache_get(&changed,NULL,0));
    history_cache_clear();
    history_cache_select_day("20260901");
    history_cache_key_t selected=key(1,3), calendar=key(2,3);
    memcpy(calendar.day,"20260831",9);
    history_cache_put(&selected,payload,12);
    for(int i=0;i<100;++i){calendar.start_ms=i;history_cache_put(&calendar,payload,HISTORY_CACHE_ITEM_BYTES);}
    assert(history_cache_get(&selected,NULL,0) && !history_cache_get(&calendar,NULL,0));
    history_cache_select_day("20260831");assert(!history_cache_get(&selected,NULL,0));
    history_cache_clear();
    pthread_t threads[4];
    for(int i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,hammer,(void *)(intptr_t)(i+1)));
    for(int i=0;i<4;++i)pthread_join(threads[i],NULL);
    history_cache_clear();free(out);free(payload);
    puts("History cache: byte budget, LRU, copying, source/filter keys and concurrent access passed");
}
