// Kernel objects behind guest handles: events, mutants, semaphores, threads, files.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct PPCContext;

enum class ObjType { Event, Mutant, Semaphore, Thread, File };

struct Object
{
    ObjType type;
    virtual ~Object() = default;
};

struct FileObject : Object
{
    FILE *fp = nullptr;
    std::string host_path;
    bool directory = false, writable = false;
    bool synchronous = true;  // opened with FILE_SYNCHRONOUS_IO_*: I/O returns its final status, else STATUS_PENDING
    uint64_t size = 0;
    std::vector<std::string> listing;  // directory enumeration (NtQueryDirectoryFile)
    size_t cursor = 0;
    bool listed = false;
};

// Handle table (handles are multiples of 4 from 0x1000; 0xFFFFFFFE = current thread).
uint32_t obj_insert( Object *o );
Object *obj_get( uint32_t handle );
bool obj_close( uint32_t handle );

// Signalling: every object state change goes through these so waiters re-check.
void sync_signal_all();
// Signals an event object by handle (I/O completion).
void sync_set_event( uint32_t handle );
void sync_reset_event( uint32_t handle );

// A new event object (for kernel-made handles such as notification listeners).
uint32_t sync_create_event( bool manual, bool signaled );

// Called by a thread's guest code path (ExCreateThread) and by main.
uint32_t sync_current_thread_handle();
