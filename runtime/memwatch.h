// Which pages of guest physical memory were written, for the renderer's textures: the pages a texture
// was made from are made read-only (in all three views of physical memory); the first write to one
// faults, gives the page a new write number and opens it again. A texture is current while none of its
// pages has a write number newer than the one it was made at.
#pragma once
#include <cstdint>

void memwatch_init();
// Protects the pages of [phys, phys + size) (physical addresses); returns the write number to make the
// texture at (any later write to those pages has a newer one).
uint32_t memwatch_protect( uint32_t phys, uint32_t size );
// Whether a page of [phys, phys + size) was written after write number `since`.
bool memwatch_written( uint32_t phys, uint32_t size, uint32_t since );
// Writes the CPU's page protection can't see -- the GPU's (resolves): [phys, phys + size) is opened and
// counts as written now. (The kernel's would fail on a protected page: system calls that write guest
// memory go through a buffer of their own, files.cpp NtReadFile.)
void memwatch_write( uint32_t phys, uint32_t size );
// Goes up with every write noticed (a texture checked since the last one needs no new check).
uint32_t memwatch_serial();
// Writes caught by page protection since the last call (statistics).
uint32_t memwatch_faults();
