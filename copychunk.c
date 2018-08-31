#include "nbt.h"
#include "list.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#define  say(...)  printf("[CopyChunk] ");printf(__VA_ARGS__);fflush(stdout);
#define  err(...)  fprintf(stderr,"[CopyChunk] <ERROR> ");fprintf(stderr,__VA_ARGS__);exit(1);
#define  VERSION "0.4"

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

struct Rectangle {
    struct Coord lower;
    struct Coord upper;
};

// function declarations
struct Rectangle *world_to_region_rect(struct Coord region, struct Rectangle rect);
struct Rectangle region_to_world_rect(struct Coord region,struct Rectangle src);
void parse_args_coords(struct Rectangle *chunks, struct Rectangle *regions, char **argv);
void parse_args_world(char **world_name,char **orig);
int coord_from_char(char* input);
int coord_normalize(int r, int l,int def);
void region_check(struct Rectangle regions,char *world);
MCR* region_open(char *region_filename,char *world,int mode);
void find_region(struct Coord chunk, struct Coord *region);
void copy_chunk_rectangle(char   *src_world,         char *dest_world, 
                          struct Coord src_region,   struct Coord dest_region, 
                          struct Rectangle src_rect, struct Rectangle dest_rect,
                          struct Coord offset);
void iterate_regions(struct Rectangle src, struct Rectangle src_regions, char *src_world,
                     struct Rectangle dest, struct Rectangle dest_regions, char *dest_world);
struct Rectangle apply_offset(struct Rectangle original, struct Coord offset);
char *print_rectangle(struct Rectangle rect);
char *print_coord(struct Coord c);
void move_tile_entities(nbt_node *chunk_data, struct Coord offset);
void move_entities(nbt_node *chunk_data, struct Coord offset);
void check_entities(nbt_node *chunk_data);

// usage
void usage(void) {
    fprintf(stderr, "\nUsage: copychunk [src] [dest] [x1] [y1] [x2] [y2]");
    fprintf(stderr, "\n       copychunk [src] [dest] [x1] [y1] [x2] [y2] [dx1] [dy1] [dx2] [dy2]");
    fprintf(stderr, "\n\n   [src] and [dest] are both paths to minecraft world directories");
    fprintf(stderr, "\n   [x1] [y1] [x2] [y2] are chunk coords (not block coords) of a square region\n\n");
    fprintf(stderr, "\n   [dx1] [dy1] [dx2] [dy2] are OPTIONAL chunk coords for destination. If not");
    fprintf(stderr, "\n                           given, src and destination coords are the same.\n\n");
    exit(1);
}

// main function, processes args before work
int main(int argc, char** argv) {
    struct Rectangle src;
    struct Rectangle src_regions;
    struct Rectangle dest;
    struct Rectangle dest_regions;
    char* src_world;
    char* dest_world;

    // Check number of arguments
    say("Version %s\n",VERSION);
    if(argc != 7 && argc != 11) { usage(); }

    // Check coordinate arguments
    parse_args_coords(&src,&src_regions,argv);
    if(argc == 11) {
        parse_args_coords(&dest,&dest_regions,&argv[4]);
        if((src.upper.x - src.lower.x) != (dest.upper.x - dest.lower.x) || 
           (src.upper.y - src.lower.y) != (dest.upper.y - dest.lower.y)) {
            err("Source and dest areas are not the same size\n[!] Source dimensions: %d by %d chunks\n[!] Target dimensions: %d by %d chunks\n[!] Aborting.",
                 (src.upper.x - src.lower.x),(src.upper.y - src.lower.y),
                 (dest.upper.x - dest.lower.x),(dest.upper.y - dest.lower.y));
        }
    } else { // Reuse src coordinates as destination
        dest = src;
        dest_regions = src_regions;
    }

    // Check the world folders exist
    parse_args_world(&src_world,&argv[1]);
    parse_args_world(&dest_world,&argv[2]);

    // Make sure all region files exist before beginning
    region_check(src_regions,src_world);
    region_check(dest_regions,dest_world);

    // Begin work
    iterate_regions(src,src_regions,src_world,dest,dest_regions,dest_world);
    say("All Done!\n");
    return 0;
}

// Main looping body to do work
void iterate_regions(struct Rectangle src, struct Rectangle src_regions, char *src_world,
                     struct Rectangle dest, struct Rectangle dest_regions, char *dest_world) {
    struct Coord dr;  // Destination Region iterator
    struct Coord sr;  // Source Region iterator
    struct Rectangle *current_dest;
    struct Rectangle *current_src;
    struct Rectangle *temp_rect = (struct Rectangle *) malloc(sizeof(struct Rectangle));    
    struct Coord offset;

    // Find offsets
    offset.x = src.lower.x - dest.lower.x;
    offset.y = src.lower.y - dest.lower.y;
    
    // Iterate destinations
    for    (dr.x = dest_regions.lower.x; dr.x <= dest_regions.upper.x; dr.x++) {
        for(dr.y = dest_regions.lower.y; dr.y <= dest_regions.upper.y; dr.y++) {

            say("Filling destination (%d,%d)\n",dr.x,dr.y);
            current_dest = world_to_region_rect(dr,dest);

            for    (sr.x = src_regions.lower.x; sr.x <= src_regions.upper.x; sr.x++) {
                for(sr.y = src_regions.lower.y; sr.y <= src_regions.upper.y; sr.y++) {

                    *temp_rect = region_to_world_rect(dr,*current_dest);
                    *temp_rect = apply_offset(*temp_rect,offset);
                    current_src = world_to_region_rect(sr,*temp_rect);

                    if(current_src == NULL) {
                        say("Skipping source (%d,%d)\n",sr.x,sr.y);
                        continue;
                    }
                                        say("Copying from source (%d,%d)\n",sr.x,sr.y);
                    copy_chunk_rectangle(src_world,dest_world,sr,dr,*current_src,*current_dest,offset);
                }
            }
        }
    }
}

// Core working part, copies data from file to file
void copy_chunk_rectangle(char   *src_world,         char *dest_world, 
                          struct Coord src_region,   struct Coord dest_region, 
                          struct Rectangle src_rect, struct Rectangle dest_rect,
                          struct Coord offset) {
            struct Coord internal_offset;
            struct Coord src_offset;

            // Open region files
            char src_region_filename[256];
            char dest_region_filename[256];
            sprintf(src_region_filename,"r.%d.%d.mca",src_region.x,src_region.y);
            sprintf(dest_region_filename,"r.%d.%d.mca",dest_region.x,dest_region.y);
            MCR *src = region_open(src_region_filename,src_world,O_RDONLY);
            MCR *dest = region_open(dest_region_filename,dest_world,O_RDWR);
    
            internal_offset.x = offset.x % 32;
            internal_offset.y = offset.y % 32;
            
            src_offset.x = region_to_world_rect(src_region,src_rect).lower.x -
            (region_to_world_rect(dest_region,dest_rect).lower.x + offset.x);
            src_offset.y = region_to_world_rect(src_region,src_rect).lower.y -
            (region_to_world_rect(dest_region,dest_rect).lower.y + offset.y);

            if(src_offset.x == 0) {
                //say("This source is LOWER X\n");
            } else {
                //say("This source is UPPER X\n");
                dest_rect.lower.x += src_offset.x;
            }

            if(src_offset.y == 0) {
                //say("This source is LOWER Y\n");
            } else {
                //say("This source is UPPER Y\n");
                dest_rect.lower.y += src_offset.y;
            }

            say("Copying source chunks (%d,%d) through (%d,%d) ... \n",src_rect.lower.x,src_rect.lower.y,src_rect.upper.x,src_rect.upper.y);
            say("Filling dest. chunks  (%d,%d) through (%d,%d) ... \n",dest_rect.lower.x,dest_rect.lower.y,dest_rect.upper.x,dest_rect.upper.y);
            say("Global offset: (%d,%d)\n",offset.x,offset.y);
            say("Internal offset: (%d,%d)\n",internal_offset.x,internal_offset.y);
            // Walk chunks
            int count = 0;
            for(int src_x = src_rect.lower.x; src_x <= src_rect.upper.x; src_x++) {
                for(int src_y = src_rect.lower.y; src_y <= src_rect.upper.y; src_y++) {
                    count++;
                    int dest_x = src_x - internal_offset.x;
                    int dest_y = src_y - internal_offset.y;
                    dest_x = dest_x % 32;
                    dest_y = dest_y % 32;
                    if(dest_x < 0) dest_x += 32;
                    if(dest_y < 0) dest_y += 32;
                    say("Source (%d,%d) => Dest (%d,%d)\n",src_x,src_y,dest_x,dest_y);
                    uint32_t timestamp = dest->chunk[dest_x][dest_y].timestamp;
                    nbt_node *chunk_data = mcr_chunk_get(src,src_x,src_y);
                    say("Old absolute position: (%d,%d)\n",
                        nbt_find_by_path(chunk_data,".Level.xPos")->payload.tag_int,
                        nbt_find_by_path(chunk_data,".Level.zPos")->payload.tag_int
                    );
                    nbt_find_by_path(chunk_data,".Level.xPos")->payload.tag_int -= offset.x;
                    nbt_find_by_path(chunk_data,".Level.zPos")->payload.tag_int -= offset.y;
                    say("New absolute position: (%d,%d)\n",
                        nbt_find_by_path(chunk_data,".Level.xPos")->payload.tag_int,
                        nbt_find_by_path(chunk_data,".Level.zPos")->payload.tag_int
                    );
                    //check_entities(chunk_data);
                    move_entities(chunk_data,offset);
                    //check_entities(chunk_data);
                    move_tile_entities(chunk_data,offset);
                    mcr_chunk_set(dest,dest_x,dest_y,chunk_data);
                    dest->chunk[dest_x][dest_y].timestamp = timestamp;
                }
            }
            printf("%d chunks copied\n",count); 
            mcr_close(src);

            say("Writing dest region...\n");
            mcr_close(dest);

}

// ---------------------------------------------------------------
// ----------------- Coordinate manipulation ---------------------
// ---------------------------------------------------------------

// Calculate region coords from chunk coords
void find_region(struct Coord chunk, struct Coord *region) {
    region->x = chunk.x >> 5;
    region->y = chunk.y >> 5;
}

// Normalize a chunk coord to its location within a region
// If chunk lies outside the region, return "def" as a min/max bound
int coord_normalize(int r,int l,int def) {
    int tmp;
    if( ((r << 5) <= l) && (((r+1) << 5) > l)) {
        tmp = l % 32;
        if (tmp < 0) {
            return tmp+32;
        } else {
            return tmp;
        }
    } else {
        return def;
    }
}

struct Rectangle *world_to_region_rect(struct Coord region, struct Rectangle rect) {
    struct Rectangle *ret = NULL;
    ret = (struct Rectangle *) malloc(sizeof(struct Rectangle));    

    // If the rect is completely outside the region, return null
    if (region.x << 5 > rect.upper.x ||
        region.y << 5 > rect.upper.y ||
        (region.x+1) << 5 <= rect.lower.x ||
        (region.y+1) << 5 <= rect.lower.y) {
        return NULL;
    } 
    ret->lower.x = coord_normalize(region.x,rect.lower.x,0);
    ret->lower.y = coord_normalize(region.y,rect.lower.y,0);
    ret->upper.x = coord_normalize(region.x,rect.upper.x,31);
    ret->upper.y = coord_normalize(region.y,rect.upper.y,31);
/*    say("World Rect: %s\n",print_rectangle(rect));
    say("Region    : %s\n",print_coord(region));
    say("Result    : %s\n",print_rectangle(*ret));
    */
    return ret;
}

struct Rectangle region_to_world_rect(struct Coord region,struct Rectangle src) {
    struct Rectangle ret;
    ret.lower.x = (region.x << 5) + src.lower.x;
    ret.lower.y = (region.y << 5) + src.lower.y;
    ret.upper.x = (region.x << 5) + src.upper.x;
    ret.upper.y = (region.y << 5) + src.upper.y;
    return ret;
}

struct Rectangle apply_offset(struct Rectangle original, struct Coord offset) {
    struct Rectangle ret;
    ret.lower.x = original.lower.x + offset.x;
    ret.upper.x = original.upper.x + offset.x;
    ret.lower.y = original.lower.y + offset.y;
    ret.upper.y = original.upper.y + offset.y;
    return ret;
}


// ---------------------------------------------------------------
// ------------ Chunk Data manipulation --------------------------
// ---------------------------------------------------------------

void move_entities(nbt_node *chunk_data, struct Coord offset) {
    nbt_node *n = nbt_find_by_path(chunk_data, ".Level.Entities");
    if (n && n->type == TAG_LIST) {
        const struct list_head* pos;
        list_for_each(pos, &n->payload.tag_list.list->entry) {
            nbt_node *entry = list_entry(pos, struct tag_list, entry)->data;
            if (entry->type == TAG_COMPOUND) {
                //say("Moving Entity\n");
                nbt_list_item(nbt_find_by_path(entry, ".Pos"),0)->payload.tag_double -= offset.x*16; 
                nbt_list_item(nbt_find_by_path(entry, ".Pos"),2)->payload.tag_double -= offset.y*16; 
            }
        }
    }
}

void move_tile_entities(nbt_node *chunk_data, struct Coord offset) {
    nbt_node *n = nbt_find_by_path(chunk_data, ".Level.TileEntities");
    if (n && n->type == TAG_LIST) {
        const struct list_head* pos;
        list_for_each(pos, &n->payload.tag_list.list->entry) {
            //say("Moving TileEntity\n");
            nbt_node *entry = list_entry(pos, struct tag_list, entry)->data;
            if (entry->type == TAG_COMPOUND) {
                nbt_find_by_path(entry,".x")->payload.tag_int -= offset.x*16;
                nbt_find_by_path(entry,".z")->payload.tag_int -= offset.y*16;
            }
        }
    }
}

void check_entities(nbt_node *chunk_data) {
    nbt_node *n = nbt_find_by_path(chunk_data, ".Level.Entities");
    if (n && n->type == TAG_LIST) {
        const struct list_head* pos;
        list_for_each(pos, &n->payload.tag_list.list->entry) {
            nbt_node *entry = list_entry(pos, struct tag_list, entry)->data;
            if (entry->type == TAG_COMPOUND) {
                /* 
                say("Position: (%f,%f)\n",
                    nbt_list_item(nbt_find_by_path(entry, ".Pos"),0)->payload.tag_double,
                    nbt_list_item(nbt_find_by_path(entry, ".Pos"),2)->payload.tag_double
                );
                */
            }
        }
    }
}

// ---------------------------------------------------------------
// ------------ Parameter parsing and validation -----------------
// ---------------------------------------------------------------

// Verify world folder exists
void parse_args_world(char **world_name,char **orig) {
    struct stat st;

    *world_name = malloc(strlen(*orig)+1);
    strcpy(*world_name,*orig);

    if(stat(*world_name,&st) != 0) {
        err("Cannot open world folder: %s\n",*world_name);
    } else {
        say("World OK: %s\n",*world_name);
    }
}

// Convert a string coordinate arg to integer "safely"
int coord_from_char(char *input) {
    char *end;
    int ret_val = strtol(input,&end,10);
    if (*end) {
        err("Error converting (%s) to integer.\n",input);
    }
    return ret_val;
}

// Parse all coordinate arguments and check for validity
void parse_args_coords(struct Rectangle *chunks, struct Rectangle *regions, char **argv) {
    // Read chunk coordinates
    chunks->lower.x = coord_from_char(argv[3]);
    chunks->lower.y = coord_from_char(argv[4]);
    chunks->upper.x = coord_from_char(argv[5]);
    chunks->upper.y = coord_from_char(argv[6]);
    say("Chunk coordinates: %d,%d to %d,%d\n",chunks->lower.x,chunks->lower.y,chunks->upper.x,chunks->upper.y);

    if (chunks->upper.x < chunks->lower.x || chunks->upper.y < chunks->lower.y) {
        err("Coordinates not in order.  Please provide numerically lower corner first.\n");
    }

    int w = 1 + chunks->upper.x - chunks->lower.x;
    int h = 1 + chunks->upper.y - chunks->lower.y; 
    say("%d chunks (%d by %d)\n",w*h,w,h);

    // Determine region coords
    find_region(chunks->lower,&regions->lower);
    find_region(chunks->upper,&regions->upper);
    int rh = (regions->upper.x-regions->lower.x)+1;
    int rw = (regions->upper.y-regions->lower.y)+1;
    say("%d region file(s): %d,%d to %d,%d\n",rh*rw,regions->lower.x,regions->lower.y,regions->upper.x,regions->upper.y);
}

// Check regions exist
void region_check(struct Rectangle regions, char *world) {
    struct stat st;
    char *path;
    char region_filename[256];
            
    for(int rx = regions.lower.x; rx <= regions.upper.x; rx++) {
        for(int ry = regions.lower.y; ry <= regions.upper.y; ry++) {
            sprintf(region_filename,"r.%d.%d.mca",rx,ry);
            path = (char *) malloc(strlen(region_filename) + strlen(world) + 9);
            sprintf(path,"%s/region/r.%d.%d.mca",world,rx,ry);
            if(stat(path,&st) != 0) {
                err("A required region file does not exist: %s\n[!] All region files must exist in both worlds for successful copy.\n[!] Aborting.\n",path);
            } else {
                say("Region OK: %s\n",path);
            }
            free(path);
        }
   }
}

// ---------------------------------------------------------------
// --------------------- Other functions -------------------------
// ---------------------------------------------------------------

// Open a region file for work
MCR *region_open(char *region_filename,char *world,int mode) {
    char *path;
    path = malloc(strlen(region_filename) + strlen(world) + 9);
    sprintf(path,"%s/region/%s",world,region_filename);
    MCR *ret_region = mcr_open(path,mode);
    if (mode == O_RDONLY) {
        say("Opened for reading: %s\n",path);
    } else {
        say("Opened for writing: %s\n",path);
    }
    if(ret_region == NULL) {
        err("Unknown error opening region file: %s\n",path);
    }
    free(path);
    return ret_region;
}

// Pretty print rectangle
char *print_rectangle(struct Rectangle rect) {
    char *retstr = malloc(128);
    sprintf(retstr,"%s->%s",print_coord(rect.lower),print_coord(rect.upper));
    return retstr;
}

// Pretty print coords
char *print_coord(struct Coord c) {
    char* retstr = malloc(64);
    sprintf(retstr,"(%d,%d)",c.x,c.y);
    return retstr;
}

