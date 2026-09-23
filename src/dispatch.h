#ifndef DISPATCH_H
#define DISPATCH_H
#include <stdint.h>
typedef void (*Fn)(void);
typedef struct { uint16_t seg, off; Fn fn; } FnEntry;
typedef struct { const char *name; int ovl_id; uint16_t seg; int size; int fileoff; const FnEntry *tab; } SpaceTab;
extern const SpaceTab space_tabs[];
#endif
