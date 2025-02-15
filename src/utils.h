#pragma once

#include <unistd.h>
#include <sys/mman.h>
#include "osmem.h"
#include "block_meta.h"

#define MMAP_THRESHOLD (128 * 1024)
#define INIT_MEM_ALLOC (128 * 1024)
#define INT_MAX 2147483647
