// SPDX-License-Identifier: BSD-3-Clause

#include "utils.h"

void *heap_start;
size_t header_size = 32;
size_t pagesize;

size_t aligned_size(size_t bytes)
{
	if (bytes % 8 == 0)
		return bytes;
	return bytes + 8 - (bytes % 8);
}

static struct block_meta *alloc_block(void *prev_block, size_t bytes, size_t threshold)
{
	struct block_meta *block;

	if (bytes > threshold) {
		block = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		block->status = STATUS_MAPPED;
	} else {
		block = sbrk(bytes);
		block->status = STATUS_ALLOC;
	}

	DIE(block == NULL, "Not able to initialize the heap management system.");

	block->size = bytes - header_size;
	block->prev = prev_block;
	if (prev_block != NULL)
		((struct block_meta *)prev_block)->next = block;
	block->next = NULL;

	return block;
}

void fill_with_zeros(struct block_meta *block)
{
	for (size_t i = 0; i < block->size; ++i)
		((unsigned char *)block + header_size)[i] = 0;
}

static struct block_meta *alloc_block_with_zeros(void *prev_block, size_t bytes)
{
	struct block_meta *block = alloc_block(prev_block, bytes, pagesize);

	fill_with_zeros(block);
	return block;
}

static void split(struct block_meta *block, size_t bytes)
{
	if (header_size + block->size - bytes <= header_size)
		return;

	struct block_meta *new_block = (void *)block + bytes;

	new_block->size = block->size - bytes;
	block->size = bytes - header_size;
	new_block->status = STATUS_FREE;
	new_block->prev = block;
	new_block->next = block->next;

	block->next = new_block;
	if (new_block->next != NULL)
		new_block->next->prev = new_block;
}

static void merge(struct block_meta *first, struct block_meta *second)
{
	first->size += header_size + second->size;
	if (first->next->next != NULL)
		first->next->next->prev = first;
	first->next = first->next->next;
}

static void coalesce_blocks(struct block_meta *first_block)
{
	if (first_block == NULL)
		return;
	while (first_block->next != NULL) {
		if (first_block->status == STATUS_FREE && first_block->next->status == STATUS_FREE)
			merge(first_block, first_block->next);
		else
			first_block = first_block->next;
	}
}

static struct block_meta *find_best_block(struct block_meta *first_block, size_t bytes)
{
	struct block_meta *best_fit = NULL;

	while (1) {
		if (first_block->status == STATUS_FREE && first_block->size + header_size >= bytes)
			if (best_fit == NULL || first_block->size < best_fit->size)
				best_fit = first_block;

		if (first_block->next == NULL)
			break;
		first_block = first_block->next;
	}

	if (best_fit != NULL) {
		split(best_fit, bytes);
		best_fit->status = STATUS_ALLOC;
		return best_fit;
	}

	return NULL;
}

static struct block_meta *prealloc(size_t threshold, size_t bytes)
{
	if (threshold == pagesize) {
		if (bytes < threshold) {
			struct block_meta *block = alloc_block(NULL, INIT_MEM_ALLOC, INT_MAX);

			for (size_t i = 0; i < block->size; ++i)
				((unsigned char *)block + header_size)[i] = 0;
			return block;
		} else {
			return alloc_block_with_zeros(NULL, bytes);
		}
	} else {
		if (bytes < threshold)
			return alloc_block(NULL, INIT_MEM_ALLOC, threshold);
		else
			return alloc_block(NULL, bytes, threshold);
	}
}

void *os_malloc(size_t size)
{
	size = aligned_size(size);
	if (size == 0)
		return NULL;

	if (size + header_size > MMAP_THRESHOLD) {
		void *block = alloc_block(NULL, size + header_size, MMAP_THRESHOLD);

		return block + header_size;
	}

	if (heap_start == NULL) {
		heap_start = prealloc(MMAP_THRESHOLD, size + header_size);
		return heap_start + header_size;
	}
	coalesce_blocks(heap_start);

	struct block_meta *block = find_best_block(heap_start, size + header_size);

	if (block == NULL) {
		block = heap_start;
		while (block->next)
			block = block->next;

		if (block->status == STATUS_FREE) {
			merge(block, alloc_block(block, size - block->size, MMAP_THRESHOLD));
			block->status = STATUS_ALLOC;
			return (void *)block + header_size;
		}
		return (void *)alloc_block(block, size + header_size, MMAP_THRESHOLD) + header_size;
	}

	return (void *)block + header_size;
}

void os_free(void *ptr)
{
	if (ptr == 0)
		return;

	struct block_meta *block = ptr - header_size;

	if (block == NULL || block->status == STATUS_FREE)
		return;

	if (block->status == STATUS_MAPPED)
		munmap(ptr - header_size, block->size + header_size);
	else
		block->status = STATUS_FREE;
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	size_t new_size = aligned_size(nmemb * size);

	if (pagesize == 0)
		pagesize = getpagesize();

	if (size + header_size > pagesize) {
		void *block = alloc_block(NULL, new_size + header_size, pagesize);

		return block + header_size;
	}

	if (heap_start == NULL) {
		header_size = aligned_size(sizeof(struct block_meta));

		heap_start = prealloc(pagesize, new_size + header_size);
		return heap_start + header_size;
	}

	coalesce_blocks(heap_start);

	struct block_meta *block = find_best_block(heap_start, new_size + header_size);

	if (block == NULL) {
		block = heap_start;
		while (block->next)
			block = block->next;

		if (block->status == STATUS_FREE) {
			merge(block, alloc_block(block, new_size - block->size, pagesize));
			fill_with_zeros(block);
			return (void *)block + header_size;
		}

		return (void *)alloc_block_with_zeros(block, new_size + header_size) + header_size;
	}

	fill_with_zeros(block);
	return (void *)block + header_size;
}

void *relocate_mem(struct block_meta *block, size_t size)
{
	struct block_meta *new_block = os_malloc(size) - header_size;
	size_t min_size = new_block->size < block->size ? new_block->size : block->size;

	for (size_t i = 0; i < min_size; ++i)
		((char *)new_block + header_size)[i] = ((char *)block + header_size)[i];

	os_free((void *)block + header_size);
	return (void *)new_block + header_size;
}

void *os_realloc(void *ptr, size_t size)
{
	size = aligned_size(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	if (ptr == NULL)
		return os_malloc(size);

	struct block_meta *block = ptr - header_size;

	if (block->status == STATUS_FREE)
		return NULL;
	else if (block->status == STATUS_MAPPED)
		return relocate_mem(block, size);

	if (size == block->size)
		return ptr;

	if (size < block->size) {
		split(block, size + header_size);
		return ptr;
	}

	struct block_meta *next_block = block->next;

	if (next_block == NULL) {
		brk(ptr + size);
		block->size = size;
		return ptr;
	}

	while (next_block != NULL && next_block->status == STATUS_FREE) {
		merge(block, next_block);
		next_block = block->next;

		if (block->size >= size) {
			split(block, header_size + size);
			return ptr;
		}
	}

	return relocate_mem(block, size);
}
