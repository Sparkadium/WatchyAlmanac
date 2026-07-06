#ifndef DICT_ENTRY_H
#define DICT_ENTRY_H

#include <Arduino.h>

typedef struct {
    String word;
    String definition;
    long   offset;
    bool   found;
} DictEntry;

#endif
