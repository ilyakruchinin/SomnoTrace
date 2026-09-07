#include "maintenance_release.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void parse(const char *input, bool asset) {
    maintenance_release_t out;
    assert(maintenance_release_parse(input,strlen(input),&out)==ESP_OK);
    assert(out.valid && out.compatible_asset==asset);
    assert(!strcmp(out.version,"v1"));
}
int main(void) {
    parse("[{\"tag_name\":\"v1\",\"body\":\"Notes\",\"assets\":[{\"name\":\"somnotrace-7b-ota.bin\",\"browser_download_url\":\"https://example.invalid/firmware\"}]}]",true);
    parse("[{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"somnotrace-generic-ota.bin\",\"browser_download_url\":\"https://example.invalid/firmware\"}]}]",false);
    parse("[{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"somnotrace-7b.bin\",\"browser_download_url\":\"http://example.invalid/firmware\"}]}]",false);
    parse("[{\"tag_name\":\"v1\",\"assets\":[]}]",false);
    maintenance_release_t out;memset(&out,0xff,sizeof(out));
    assert(maintenance_release_parse("{bad",4,&out)!=ESP_OK);assert(!out.valid&&!out.compatible_asset);
    assert(maintenance_release_parse("[]",2,&out)!=ESP_OK);
    assert(maintenance_release_parse("[{\"tag_name\":\"v1\"}]BAD",strlen("[{\"tag_name\":\"v1\"}]BAD"),&out)!=ESP_OK);
    assert(maintenance_release_parse("x",MAINTENANCE_RELEASE_RESPONSE_MAX+1,&out)==ESP_ERR_INVALID_SIZE);
    const char *deep="[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]";
    assert(maintenance_release_parse(deep,strlen(deep),&out)==ESP_ERR_INVALID_SIZE);
    char notes[2100];memset(notes,'x',sizeof(notes));notes[sizeof(notes)-1]=0;
    char response[2400];snprintf(response,sizeof(response),"[{\"tag_name\":\"v1\",\"body\":\"%s\"}]",notes);
    assert(maintenance_release_parse(response,strlen(response),&out)==ESP_OK);assert(out.notes_truncated);assert(strstr(out.notes,"[Notes truncated]"));
    puts("release parser fixtures passed: compatible/missing target, unsafe URL, malformed, oversized, deep, truncated notes");
}
