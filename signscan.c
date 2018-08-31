#include "nbt.h"
#include "list.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#define  say(...)  printf("[SignScan] ");printf(__VA_ARGS__);fflush(stdout);
#define  err(...)  fprintf(stderr,"[SignScan] <ERROR> ");fprintf(stderr,__VA_ARGS__);exit(1);
#define  VERSION "0.1"

// private structures
struct MCR {
    int fd;
    int readonly;
    uint32_t last_timestamp;
    struct MCRChunk {
        uint32_t timestamp;
        uint32_t len;
        unsigned char *data; // compression type + data
    } chunk[32][32];
};

struct Coord {
    int x;
    int y;
};

void parse_args_world(char**,char**);
MCR *region_open(char*,char*,int);

// usage
void usage(void) {
    fprintf(stderr, "\nUsage: signscan [world folder] [sign text]\n");
    exit(1);
}

void lowercase(char* text) {
    for(;*text;text++) *text = tolower(*text);
}

// main function, processes args before work
int main(int argc, char** argv) {
    char* world; // String path to world to scan
    char* forbidden = malloc(strlen(argv[2])+1);
    DIR *dp;
    struct dirent *ep;

    say("Version %s\n",VERSION);

    // Check number of arguments
    if(argc != 3) { usage(); }

    // Check the world folder is readable
    parse_args_world(&world,&argv[1]);
    dp = opendir(world);

    // Copy test string to local storage, make lower case
    strcpy(forbidden,argv[2]);
    lowercase(forbidden);
    
    // Get the list of region files
    while((ep = readdir (dp))) {
        if(strstr(ep->d_name,".mca")) {
            // Open each region file
            MCR *src = region_open(ep->d_name,world,O_RDONLY);
         
            // Get each chunk in the region
            for(int x=0; x<32; x++) for(int z=0; z<32; z++) {
                nbt_node *root = mcr_chunk_get(src,x,z);
                if (root == NULL) continue;

                // Check every block in the chunk
                nbt_node *s = nbt_find_by_path(root, ".Level.TileEntities");
                const struct list_head* pos;
                if (s && s->type == TAG_LIST) {
                    list_for_each(pos, &s->payload.tag_list.list->entry) {
                        nbt_node *tile_entity = list_entry(pos, struct tag_list, entry)->data;
                        if (tile_entity->type != TAG_COMPOUND) continue;
                        nbt_node* eid_node = nbt_find_by_path(tile_entity,".id");
                        if (!eid_node) continue;

                        char *eid = eid_node->payload.tag_string;
                        //say("Scanning TileEntity: %s\n",eid);
                        if (!strstr(eid,"Sign")) continue;

                        int bx = nbt_find_by_path(tile_entity, ".x")->payload.tag_int;
                        int by = nbt_find_by_path(tile_entity, ".y")->payload.tag_int;
                        int bz = nbt_find_by_path(tile_entity, ".z")->payload.tag_int;

                        char *text1 = nbt_find_by_path(tile_entity, ".Text1")->payload.tag_string;
                        lowercase(text1);
                        char *text2 = nbt_find_by_path(tile_entity, ".Text2")->payload.tag_string;
                        lowercase(text2);
                        char *text3 = nbt_find_by_path(tile_entity, ".Text3")->payload.tag_string;
                        lowercase(text3);
                        char *text4 = nbt_find_by_path(tile_entity, ".Text4")->payload.tag_string;
                        lowercase(text4);
                        
                        if (strstr(text1,argv[2]) ||
                            strstr(text2,argv[2]) ||
                            strstr(text3,argv[2]) ||
                            strstr(text4,argv[2])                       
                        ) {
                            say(" + Found offending sign at (X Y Z) : %d %d %d\n", bx,by,bz);
                        } 
                    }
                }
                nbt_free(root);
            } 

            // Close region file
            mcr_close(src);
       }
    }
   exit(0); 
}

// Verify world folder exists
void parse_args_world(char **world_name,char **orig) {
    struct stat st;

    *world_name = malloc(strlen(*orig)+1);
    sprintf(*world_name,"%s/region",*orig);

    if(stat(*world_name,&st) != 0) {
        err("  !!! Cannot open world folder: %s\n",*world_name);
    } else {
        say("  World OK: %s\n",*world_name);
    }
}

MCR *region_open(char *region_filename,char *world,int mode) {
    char *path;
    path = malloc(strlen(region_filename) + strlen(world) + 2);
    sprintf(path,"%s/%s",world,region_filename);
    MCR *ret_region = mcr_open(path,mode);
    if (mode == O_RDONLY) {
        //say("  Opened for reading: %s\n",path);
    } else {
        //say("  Opened for writing: %s\n",path);
    }
    if(ret_region == NULL) {
        err("  !!! Unknown error opening region file: %s\n",path);
    }
    free(path);
    return ret_region;
}

