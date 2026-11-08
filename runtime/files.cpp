// File system: the game's paths (game:\, d:\, \Device\Cdrom0\...) map onto the extracted disc
// folder (CRACKDOWN_DISC), read-only; update:\ onto the title update's files (CRACKDOWN_UPDATE, default
// <data dir>/crackdown-recomp/update: the DLC string tables of devtools/stfs.py --update), read-only. Content packages (saves) mounted by XamContentCreateEx map a
// root name ("save:\") onto a writable host folder. Lookup is case-insensitive, as on the 360.
// Disc files copied into the program's own folder (<program dir>/disc, CRACKDOWN_DISC_COPY: build/disc
// holds the disc's Audio folder) are opened there instead: the disc folder can be on a spinning disk,
// and the sound banks are read as sounds play. Banks opened from there are kept in memory whole (a
// thread loads each the first time it is opened; reads come from memory once it has).
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
void sound_bank_read( const std::string &path, uint64_t off, uint32_t dest, uint32_t size );  // soundbanks.cpp
#include "objects.h"

#include <dirent.h>
#include <strings.h>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <map>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <fnmatch.h>
#include <sys/stat.h>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <chrono>
#include <vector>

std::string xui_options_archive( const std::string &host, const std::string &scenes );  // xui_options.cpp
std::string xui_menu_archive( const std::string &host );  // xui_options.cpp
void fontatlas_seen( const std::string &host );           // fontatlas.cpp

namespace
{

constexpr uint32_t STATUS_SUCCESS = 0, STATUS_PENDING = 0x103, STATUS_END_OF_FILE = 0xC0000011, STATUS_INVALID_HANDLE = 0xC0000008,
                   STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_ACCESS_DENIED = 0xC0000022,
                   STATUS_INVALID_PARAMETER = 0xC000000D;

// The disc's folder: CRACKDOWN_DISC, else the one the launcher's install chose (<data dir>/crackdown-recomp/
// install.ini "disc = <folder>", devtools/setup.sh).
std::string disc_root()
{
    static const std::string root = [] {
        if ( const char *d = getenv( "CRACKDOWN_DISC" ) ) return std::string( d );
        const char *xdg = getenv( "XDG_DATA_HOME" ), *home = getenv( "HOME" );
        const std::string ini = ( xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.local/share" ) + "/crackdown-recomp/install.ini";
        std::string disc;
        if ( FILE *f = fopen( ini.c_str(), "r" ) )
        {
            char line[4096];
            while ( fgets( line, sizeof( line ), f ) )
                if ( !strncmp( line, "disc = ", 7 ) )
                {
                    disc = line + 7;
                    while ( !disc.empty() && ( disc.back() == '\n' || disc.back() == '\r' ) ) disc.pop_back();
                }
            fclose( f );
        }
        if ( disc.empty() ) fprintf( stderr, "files: no disc folder: run the launcher's install (or set CRACKDOWN_DISC)\n" );
        return disc;
    }();
    return root;
}

// The folder of disc files copied next to the program (empty if none).
std::string disc_copy_root()
{
    static const std::string root = [] {
        if ( const char *d = getenv( "CRACKDOWN_DISC_COPY" ) ) return std::string( d );
        char exe[4096];
        ssize_t n = readlink( "/proc/self/exe", exe, sizeof( exe ) - 1 );
        if ( n <= 0 ) return std::string();
        std::string dir( exe, n );
        dir = dir.substr( 0, dir.find_last_of( '/' ) ) + "/disc";
        return access( dir.c_str(), R_OK ) == 0 ? dir : std::string();
    }();
    return root;
}

// Files kept in memory whole (sound banks from the disc copy), by host path.
struct Resident
{
    std::vector<uint8_t> data;
    std::atomic<bool> ready{ false };
};
std::mutex g_resident_lock;
std::map<std::string, std::shared_ptr<Resident>> g_resident;

bool keep_resident( const std::string &host )
{
    const std::string &copy = disc_copy_root();
    if ( copy.empty() || host.rfind( copy, 0 ) != 0 ) return false;
    std::string l = host;
    for ( char &ch : l ) ch = (char)tolower( (unsigned char)ch );
    return l.size() > 4 && ( l.compare( l.size() - 4, 4, ".xwb" ) == 0 || l.compare( l.size() - 4, 4, ".apk" ) == 0 );
}

// The file in memory, if it is kept there and has finished loading; starts loading it the first time.
std::shared_ptr<Resident> resident( const std::string &host )
{
    if ( !keep_resident( host ) ) return nullptr;
    std::lock_guard<std::mutex> g( g_resident_lock );
    auto &r = g_resident[host];
    if ( !r )
    {
        r = std::make_shared<Resident>();
        std::thread( [r, host] {
            auto start = std::chrono::steady_clock::now();
            if ( FILE *f = fopen( host.c_str(), "rb" ) )
            {
                fseeko( f, 0, SEEK_END );
                r->data.resize( (size_t)ftello( f ) );
                fseeko( f, 0, SEEK_SET );
                size_t got = fread( r->data.data(), 1, r->data.size(), f );
                fclose( f );
                if ( got == r->data.size() )
                {
                    r->ready = true;
                    rt_log( "file: %s kept in memory (%.1f MB, loaded in %.0f ms)\n", host.c_str(), got / 1048576.0,
                            std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count() );
                }
            }
        } ).detach();
    }
    return r->ready ? r : nullptr;
}

std::string update_root()
{
    if ( const char *d = getenv( "CRACKDOWN_UPDATE" ) ) return d;
    const char *xdg = getenv( "XDG_DATA_HOME" ), *home = getenv( "HOME" );
    return ( xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.local/share" ) + "/crackdown-recomp/update";
}

// ANSI_STRING { u16 Length; u16 Max; u32 Buffer }
std::string read_ansi( uint32_t p )
{
    uint16_t len = __builtin_bswap16( *(uint16_t *)( g_base + p ) );
    uint32_t buf = guest_read32( p + 4 );
    return std::string( (const char *)g_base + buf, len );
}

// OBJECT_ATTRIBUTES { u32 RootDirectory; u32 ObjectName (ANSI_STRING*); u32 Attributes }
std::string object_name( uint32_t pattr ) { return pattr ? read_ansi( guest_read32( pattr + 4 ) ) : std::string(); }

bool iequal( const std::string &a, const char *b )
{
    size_t n = strlen( b );
    if ( a.size() != n ) return false;
    for ( size_t i = 0; i < n; ++i ) if ( tolower( (unsigned char)a[i] ) != tolower( (unsigned char)b[i] ) ) return false;
    return true;
}

std::mutex g_mount_lock;
std::map<std::string, std::string> g_mounts;  // lower-case root name -> host folder

std::string lower( std::string s )
{
    for ( char &ch : s ) ch = (char)tolower( (unsigned char)ch );
    return s;
}

// Guest path -> host path, resolving each component case-insensitively. Under a writable (mounted)
// root the last component may be missing (`exists` false): the host path then names a new file.
bool resolve( std::string guest, std::string &host, bool *writable = nullptr, bool *exists = nullptr )
{
    for ( char &ch : guest ) if ( ch == '/' ) ch = '\\';
    if ( writable ) *writable = false;
    if ( exists ) *exists = true;
    std::string rest = guest;
    host = disc_root();
    bool mounted = false;
    std::string g = guest.rfind( "\\??\\", 0 ) == 0 ? guest.substr( 4 ) : guest;
    size_t colon = g.find( ':' );
    if ( colon != std::string::npos && colon > 1 )
    {
        std::lock_guard<std::mutex> lk( g_mount_lock );
        auto it = g_mounts.find( lower( g.substr( 0, colon ) ) );
        if ( iequal( g.substr( 0, colon ), "update" ) )
        {
            host = update_root();
            rest = g.substr( std::min( colon + 2, g.size() ) );
            static bool said;
            if ( !said ) { said = true; rt_log( "files: update:\\ is %s\n", host.c_str() ); }
        }
        else if ( it != g_mounts.end() )
        {
            host = it->second;
            rest = g.size() > colon + 1 ? g.substr( colon + 2 > g.size() ? g.size() : colon + 2 ) : "";
            mounted = true;
        }
    }
    if ( !mounted && host == disc_root() )
    {
        static const char *prefixes[] = { "\\Device\\Cdrom0\\", "game:\\", "d:\\", "\\??\\game:\\", "\\??\\d:\\" };
        for ( const char *p : prefixes )
        {
            size_t n = strlen( p );
            if ( guest.size() >= n && iequal( guest.substr( 0, n ), p ) ) { rest = guest.substr( n ); break; }
            if ( iequal( guest, std::string( p, n - 1 ).c_str() ) ) { rest.clear(); break; }
        }
    }
    if ( writable ) *writable = mounted;
    size_t pos = 0;
    while ( pos < rest.size() )
    {
        size_t end = rest.find( '\\', pos );
        if ( end == std::string::npos ) end = rest.size();
        std::string part = rest.substr( pos, end - pos );
        pos = end + 1;
        if ( part.empty() || part == "." ) continue;
        DIR *d = opendir( host.c_str() );
        if ( !d ) return false;
        bool found = false;
        while ( dirent *e = readdir( d ) )
            if ( iequal( part, e->d_name ) ) { host += "/"; host += e->d_name; found = true; break; }
        closedir( d );
        if ( !found )
        {
            if ( !mounted || pos < rest.size() || !exists ) return false;
            host += "/" + part;  // a new file or folder in a writable folder
            *exists = false;
            return true;
        }
    }
    // The game's menu scenes (the disc's: the game checks a DLC's files and refuses changed ones):
    // served with the Recomp Options page (xui_options.cpp).
    if ( !mounted && host.rfind( disc_root(), 0 ) == 0 )
    {
        static const std::string scenes = "/xuifrontend/scene.xzp";
        std::string l = lower( host );
        if ( l.size() > scenes.size() && l.compare( l.size() - scenes.size(), scenes.size(), scenes ) == 0 && l.find( "/local/" ) == std::string::npos )
            host = xui_options_archive( host, host );
    }
    // The game's font picture: fontatlas.cpp draws it again at the resolution's size.
    if ( host.size() > 18 && !strcasecmp( host.c_str() + host.size() - 18, "crackdown_font.dds" ) ) fontatlas_seen( host );
    // A DLC's menu: the game checks the DLC's files by MD5 against its digest, naming them as the digest
    // does (XUIFrontEnd\...), and loads the menu as xuifrontend\...: the check reads the DLC's own file.
    if ( mounted && colon != std::string::npos && g.compare( std::min( colon + 2, g.size() ), std::string::npos, "xuifrontend\\Scene_CommonDLC.xzp" ) == 0 )
        host = xui_menu_archive( host );
    if ( !mounted && !disc_copy_root().empty() && host.rfind( disc_root(), 0 ) == 0 )
    {
        std::string copy = disc_copy_root() + host.substr( disc_root().size() );
        struct stat st;
        if ( stat( copy.c_str(), &st ) == 0 && S_ISREG( st.st_mode ) ) host = copy;  // files only: folders stay the disc's
    }
    return true;
}

void write_iosb( uint32_t p, uint32_t status, uint32_t info )
{
    if ( !p ) return;
    guest_write32( p, status );
    guest_write32( p + 4, info );
}

void write64( uint32_t p, uint64_t v ) { guest_write32( p, (uint32_t)( v >> 32 ) ); guest_write32( p + 4, (uint32_t)v ); }
uint64_t read64( uint32_t p ) { return ( (uint64_t)guest_read32( p ) << 32 ) | guest_read32( p + 4 ); }

FileObject *file( uint32_t h )
{
    Object *o = obj_get( h );
    return o && o->type == ObjType::File ? static_cast<FileObject *>( o ) : nullptr;
}

// FILE_NETWORK_OPEN_INFORMATION (0x38): 4 times, AllocationSize, EndOfFile, FileAttributes
void write_network_open_info( uint32_t p, uint64_t size, bool dir )
{
    for ( int i = 0; i < 4; ++i ) write64( p + i * 8, 0x01C8000000000000ull );
    write64( p + 0x20, ( size + 0xFFF ) & ~0xFFFull );
    write64( p + 0x28, size );
    guest_write32( p + 0x30, dir ? 0x10 : 0x80 );  // DIRECTORY / NORMAL
    guest_write32( p + 0x34, 0 );
}

} // namespace

#define ARG(i) ( (i) == 0 ? c.r3.u32 : (i) == 1 ? c.r4.u32 : (i) == 2 ? c.r5.u32 : (i) == 3 ? c.r6.u32 : \
                 (i) == 4 ? c.r7.u32 : (i) == 5 ? c.r8.u32 : (i) == 6 ? c.r9.u32 : c.r10.u32 )

// NtCreateFile(PHANDLE, access, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, allocSize, attributes, share,
//              disposition, options [stack]). NtOpenFile(PHANDLE, access, attrs, iosb, share, options)
// opens existing files only. Creation and writes are allowed under mounted content roots.
void create_file( PPCContext &c, uint32_t disposition, uint32_t options )
{
    constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    enum { SUPERSEDE, OPEN, CREATE, OPEN_IF, OVERWRITE, OVERWRITE_IF };
    std::string name = object_name( ARG( 2 ) ), host;
    bool writable, exists;
    if ( !resolve( name, host, &writable, &exists ) || ( !exists && ( disposition == OPEN || disposition == OVERWRITE ) ) )
    {
        rt_log( "file: %s not found\n", name.c_str() );
        write_iosb( ARG( 3 ), STATUS_OBJECT_NAME_NOT_FOUND, 0 );
        c.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    if ( exists && disposition == CREATE )
    {
        write_iosb( ARG( 3 ), STATUS_OBJECT_NAME_COLLISION, 0 );
        c.r3.u64 = STATUS_OBJECT_NAME_COLLISION;
        return;
    }
    uint32_t info = 1;  // FILE_OPENED
    bool want_dir = options & 1;  // FILE_DIRECTORY_FILE
    if ( !exists )
    {
        if ( want_dir ? mkdir( host.c_str(), 0755 ) != 0 : !std::unique_ptr<FILE, int ( * )( FILE * )>( fopen( host.c_str(), "wb" ), fclose ) )
        {
            c.r3.u64 = STATUS_ACCESS_DENIED;
            return;
        }
        info = 2;  // FILE_CREATED
        rt_log( "file: created %s\n", host.c_str() );
    }
    else if ( writable && !want_dir && ( disposition == SUPERSEDE || disposition == OVERWRITE || disposition == OVERWRITE_IF ) )
    {
        if ( truncate( host.c_str(), 0 ) != 0 ) { c.r3.u64 = STATUS_ACCESS_DENIED; return; }
        info = disposition == SUPERSEDE ? 0 : 3;  // FILE_SUPERSEDED / FILE_OVERWRITTEN
    }
    struct stat st;
    stat( host.c_str(), &st );
    auto *f = new FileObject;
    f->type = ObjType::File;
    f->host_path = host;
    f->directory = S_ISDIR( st.st_mode );
    f->size = st.st_size;
    f->writable = writable;
    f->synchronous = ( options & 0x30 ) != 0;  // FILE_SYNCHRONOUS_IO_ALERT | _NONALERT
    if ( !f->directory && !( f->fp = fopen( host.c_str(), writable ? "r+b" : "rb" ) ) )
    {
        delete f;
        c.r3.u64 = STATUS_ACCESS_DENIED;
        return;
    }
    uint32_t h = obj_insert( f );
    guest_write32( ARG( 0 ), h );
    write_iosb( ARG( 3 ), STATUS_SUCCESS, info );
    rt_log( "file: open %s -> %04X\n", name.c_str(), h );
    if ( const char *who = getenv( "CRACKDOWN_OPEN_TRACE" ); who && name.find( who ) != std::string::npos )  // debug: who opens it
    {
        char bt[512];
        rt_guest_backtrace( bt, sizeof( bt ) );
        rt_log( "file:   opened from %s\n", bt );
    }
    c.r3.u64 = STATUS_SUCCESS;
}
void imp_NtCreateFile( PPCContext &c ) { create_file( c, ARG( 7 ), guest_read32( c.r1.u32 + 0x54 ) ); }
void imp_NtOpenFile( PPCContext &c ) { create_file( c, 1 /* FILE_OPEN */, ARG( 5 ) ); }

// NtWriteFile(HANDLE, event, apcRoutine, apcContext, PIO_STATUS_BLOCK, buffer, length, PLARGE_INTEGER offset)
RT_IMPORT( NtWriteFile )
{
    FileObject *f = file( RT_ARG( 0 ) );
    if ( !f || !f->fp || !f->writable ) RT_RETURN( f ? STATUS_ACCESS_DENIED : STATUS_INVALID_HANDLE );
    if ( RT_ARG( 7 ) ) fseeko( f->fp, (off_t)read64( RT_ARG( 7 ) ), SEEK_SET );
    size_t put = fwrite( g_base + RT_ARG( 5 ), 1, RT_ARG( 6 ), f->fp );
    fflush( f->fp );
    f->size = std::max<uint64_t>( f->size, (uint64_t)ftello( f->fp ) );
    write_iosb( RT_ARG( 4 ), STATUS_SUCCESS, (uint32_t)put );
    if ( RT_ARG( 1 ) ) sync_set_event( RT_ARG( 1 ) );
    c.r3.u64 = f->synchronous ? STATUS_SUCCESS : STATUS_PENDING;
    rt_log( "file: wrote %zu bytes to %s at %lld (lr %08X, iosb %08X, event %X)%s\n", put, f->host_path.c_str(), RT_ARG( 7 ) ? (long long)read64( RT_ARG( 7 ) ) : -1LL, (uint32_t)c.lr, RT_ARG( 4 ), RT_ARG( 1 ),
            RT_ARG( 2 ) ? " (APC not delivered!)" : "" );
    return;
}

void files_mount( const std::string &root, const std::string &host )
{
    std::lock_guard<std::mutex> lk( g_mount_lock );
    g_mounts[lower( root )] = host;
}
bool files_unmount( const std::string &root )
{
    std::lock_guard<std::mutex> lk( g_mount_lock );
    return g_mounts.erase( lower( root ) ) != 0;
}

// NtReadFile(HANDLE, event, apcRoutine, apcContext, PIO_STATUS_BLOCK, buffer, length, PLARGE_INTEGER offset)
void imp_NtReadFile( PPCContext &c )
{
    FileObject *f = file( ARG( 0 ) );
    if ( !f || !f->fp ) { c.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if ( ARG( 7 ) ) fseeko( f->fp, (off_t)read64( ARG( 7 ) ), SEEK_SET );
    // Physical memory (0xA0000000 up) may hold pages the renderer watches for writes (memwatch.h): read
    // through a buffer of our own and copied in, so a write to a watched page is caught as any other.
    // (The kernel can't write a protected page: the read would fail and the game get stale memory --
    // the renderer can protect a page again any time, so opening them first isn't enough.)
    size_t got;
    const off_t at = ftello( f->fp );
    const auto started = std::chrono::steady_clock::now();
    if ( auto r = resident( f->host_path ) )  // kept in memory: no disk at all
    {
        got = at >= 0 && (size_t)at < r->data.size() ? std::min<size_t>( ARG( 6 ), r->data.size() - (size_t)at ) : 0;
        memcpy( g_base + ARG( 5 ), r->data.data() + at, got );
        fseeko( f->fp, at + (off_t)got, SEEK_SET );
    }
    else if ( ARG( 5 ) >= 0xA0000000u )
    {
        thread_local std::vector<uint8_t> buffer;
        if ( buffer.size() < ARG( 6 ) ) buffer.resize( ARG( 6 ) );
        got = fread( buffer.data(), 1, ARG( 6 ), f->fp );
        memcpy( g_base + ARG( 5 ), buffer.data(), got );
    }
    else got = fread( g_base + ARG( 5 ), 1, ARG( 6 ), f->fp );
    if ( got < ARG( 6 ) && ferror( f->fp ) )
    {
        rt_log( "file: read of %u bytes from %s to %08X failed after %zu (%s)\n", ARG( 6 ), f->host_path.c_str(), ARG( 5 ), got, strerror( errno ) );
        clearerr( f->fp );
    }
    // Reads that kept the game waiting (the disc's files can be on a spinning disk): 25 ms or more.
    if ( double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count(); ms >= 25 )
        rt_log( "file: read of %u bytes at %llX of %s took %.0f ms\n", ARG( 6 ), (unsigned long long)at, f->host_path.c_str(), ms );
    sound_bank_read( f->host_path, (uint64_t)at, ARG( 5 ), (uint32_t)got );
    ++g_stats.reads;
    static const bool read_log = getenv( "CRACKDOWN_READ_LOG" );  // which files stream (music, video)
    if ( read_log ) rt_log( "file: read %s +%llX %zu\n", f->host_path.c_str(), ARG( 7 ) ? (unsigned long long)read64( ARG( 7 ) ) : 0ull, got );
    if ( ARG( 2 ) )
    {
        static int n;
        if ( n++ < 5 ) rt_log( "NtReadFile with APC routine %08X ctx %08X (not delivered!)\n", ARG( 2 ), ARG( 3 ) );
    }
    g_stats.read_bytes += got;
    uint32_t status = got || !ARG( 6 ) ? STATUS_SUCCESS : STATUS_END_OF_FILE;
    write_iosb( ARG( 4 ), status, (uint32_t)got );
    if ( ARG( 1 ) ) sync_set_event( ARG( 1 ) );  // reads complete at once: signal the completion event
    // On an asynchronous handle the request has "started": the caller collects the result from the
    // I/O status block (XAPI ReadFile then reports ERROR_IO_PENDING, which overlapped callers expect).
    c.r3.u64 = f->synchronous || status != STATUS_SUCCESS ? status : STATUS_PENDING;
}

// NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, info, length, FILE_INFORMATION_CLASS)
void imp_NtQueryInformationFile( PPCContext &c )
{
    FileObject *f = file( ARG( 0 ) );
    if ( !f ) { c.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t p = ARG( 2 );
    if ( f->writable ) rt_log( "file: query class %u (length %u) of %s: size %llu\n", ARG( 4 ), ARG( 3 ), f->host_path.c_str(), (unsigned long long)f->size );
    switch ( ARG( 4 ) )
    {
    case 5:  // FileStandardInformation { AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory }
        write64( p, ( f->size + 0xFFF ) & ~0xFFFull );
        write64( p + 8, f->size );
        guest_write32( p + 16, 1 );
        g_base[p + 20] = 0;
        g_base[p + 21] = f->directory;
        break;
    case 14:  // FilePositionInformation
        write64( p, f->fp ? (uint64_t)ftello( f->fp ) : 0 );
        break;
    case 20:  // FileEndOfFileInformation
        write64( p, f->size );
        break;
    case 34:  // FileNetworkOpenInformation
        write_network_open_info( p, f->size, f->directory );
        break;
    default:
        rt_log( "file: NtQueryInformationFile class %u not implemented\n", ARG( 4 ) );
        c.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }
    write_iosb( ARG( 1 ), STATUS_SUCCESS, ARG( 3 ) );
    c.r3.u64 = STATUS_SUCCESS;
}

// NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, info, length, class)
void imp_NtSetInformationFile( PPCContext &c )
{
    FileObject *f = file( ARG( 0 ) );
    if ( !f ) { c.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if ( ARG( 4 ) == 14 && f->fp ) fseeko( f->fp, (off_t)read64( ARG( 2 ) ), SEEK_SET );
    else if ( ARG( 4 ) == 20 && f->fp && f->writable )  // FileEndOfFileInformation
    {
        fflush( f->fp );
        f->size = read64( ARG( 2 ) );
        if ( ftruncate( fileno( f->fp ), (off_t)f->size ) != 0 ) rt_log( "file: truncate %s failed\n", f->host_path.c_str() );
    }
    else if ( ARG( 4 ) == 19 ) {}  // FileAllocationInformation: nothing to reserve
    else rt_log( "file: NtSetInformationFile class %u ignored\n", ARG( 4 ) );
    write_iosb( ARG( 1 ), STATUS_SUCCESS, 0 );
    c.r3.u64 = STATUS_SUCCESS;
}

// NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION)
void imp_NtQueryFullAttributesFile( PPCContext &c )
{
    std::string name = object_name( ARG( 0 ) ), host;
    struct stat st;
    if ( !resolve( name, host ) || stat( host.c_str(), &st ) )
    {
        rt_log( "file: attributes of %s: not found\n", name.c_str() );
        c.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    write_network_open_info( ARG( 1 ), st.st_size, S_ISDIR( st.st_mode ) );
    c.r3.u64 = STATUS_SUCCESS;
}

// NtQueryDirectoryFile(handle, event, apc, apcContext, PIO_STATUS_BLOCK, FILE_DIRECTORY_INFORMATION *,
//                      length, PANSI_STRING mask, BOOLEAN restart [stack]): one entry per call.
RT_IMPORT( NtQueryDirectoryFile )
{
    constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006, STATUS_BUFFER_OVERFLOW = 0x80000005;
    FileObject *f = file( RT_ARG( 0 ) );
    if ( !f || !f->directory ) RT_RETURN( STATUS_INVALID_HANDLE );
    bool restart = guest_read32( c.r1.u32 + 84 ) & 0xFF;
    std::string mask = RT_ARG( 7 ) ? read_ansi( RT_ARG( 7 ) ) : "*";
    if ( mask == "*.*" ) mask = "*";
    if ( !f->listed || restart )
    {
        f->listing.clear();
        if ( DIR *d = opendir( f->host_path.c_str() ) )
        {
            while ( dirent *e = readdir( d ) )
                if ( strcmp( e->d_name, "." ) && strcmp( e->d_name, ".." ) && !fnmatch( mask.c_str(), e->d_name, FNM_CASEFOLD ) )
                    f->listing.push_back( e->d_name );
            closedir( d );
        }
        f->cursor = 0;
        f->listed = true;
    }
    uint32_t iosb = RT_ARG( 4 ), p = RT_ARG( 5 ), len = RT_ARG( 6 );
    if ( f->cursor >= f->listing.size() ) { write_iosb( iosb, STATUS_NO_MORE_FILES, 0 ); RT_RETURN( STATUS_NO_MORE_FILES ); }
    const std::string &name = f->listing[f->cursor];
    uint32_t need = 64 + (uint32_t)name.size();
    if ( len < need ) { write_iosb( iosb, STATUS_BUFFER_OVERFLOW, 0 ); RT_RETURN( STATUS_BUFFER_OVERFLOW ); }
    struct stat st{};
    stat( ( f->host_path + "/" + name ).c_str(), &st );
    memset( g_base + p, 0, 64 );
    guest_write32( p + 4, (uint32_t)f->cursor );
    for ( int i = 0; i < 4; ++i ) write64( p + 8 + i * 8, 0x01C8000000000000ull );
    write64( p + 40, st.st_size );
    write64( p + 48, ( st.st_size + 0xFFF ) & ~0xFFFull );
    guest_write32( p + 56, S_ISDIR( st.st_mode ) ? 0x10 : 0x80 );
    guest_write32( p + 60, (uint32_t)name.size() );
    memcpy( g_base + p + 64, name.data(), name.size() );
    ++f->cursor;
    write_iosb( iosb, STATUS_SUCCESS, need );
    RT_RETURN( STATUS_SUCCESS );
}

// A disc file's host path ("XUIFrontEnd/Loading.bik"): the copy next to the program if it has it, else the disc.
std::string disc_file_path( const char *rel )
{
    std::string copy = disc_copy_root();
    if ( !copy.empty() )
    {
        std::string p = copy + "/" + rel;
        if ( access( p.c_str(), R_OK ) == 0 ) return p;
    }
    return disc_root() + "/" + rel;
}
