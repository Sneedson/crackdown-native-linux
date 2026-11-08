// XUI scenes (.xur, version 5 "XUIB"), read and edited: the Recomp Options page and its menu entry are
// made from the game's own scenes (runtime/xui_options.cpp).
//
// The format, as the game's XUI reads it (8278xxxx): a header ("XUIB", version 5, flags, u16, u32 file
// size, u16 section count; flag 1 adds 10 u32 counts, hints for the loader), then the sections
// { name, offset, size }: STRN (strings: u16 length, UTF-16BE), VECT (12-byte vectors), QUAT (16-byte
// quaternions), CUST (custom property data), DATA (the objects).
// An object: u16 class (a string, 1-based), u8 flags (1 properties, 2 children, 4 timelines);
// properties: u16 count, then for each class from XuiElement down to the object's own, a byte
// ((count - 1) << 3 | mask bytes: 0, 1, 2 or 4), the mask (bit i: definition i is set), and the values
// in definition order: bool 1 byte, string u16 (1-based, 0 none), vector / quaternion u32 (0-based),
// custom u32 (CUST offset), other 4 bytes; an indexed definition first has its count (a byte; with bit
// 7 set, that many bytes of count follow); an object (type 9) has u16 count and the sub-definitions'
// byte, mask and values. Children: u32 count, objects. Timelines (only with children): u32 named
// frames x { u16 name, u32 time, u8 command, u16 target }, u32 timelines x { u16 element (an Id),
// u32 properties x { u8 depth | 0x80 indexed; with depth: u8 base classes up from the element's,
// u8 definition, then a byte per level deeper; with 0x80: u32 index }, u32 keyframes x { u32 time,
// 4 bytes, then a value per property (none for types 9 and 10) } }.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct XurDef
{
    const char *name;
    uint8_t type, indexed;  // type: 1 bool, 2 int, 3 unsigned, 4 float, 5 string, 6 color, 7 vector, 8 quaternion, 9 object, 10 custom
    const XurDef *sub;      // type 9: its definitions
    uint8_t nsub;
};
struct XurClass
{
    const char *name, *base;
    const XurDef *defs;
    uint8_t n;
};

struct XurRef  // a value in DATA that names something in a table (or a property to edit)
{
    uint32_t at;        // offset in the file
    uint8_t type;       // 5 string (u16), 7 vector (u32), 8 quaternion (u32), 10 custom (u32), 0 class (u16), 4 float, 1 bool...
    const char *prop;   // its definition's name (nullptr for a class or a timeline's names)
};

struct XurObject
{
    uint32_t start = 0, end = 0;
    std::string cls;
    uint32_t props_at = 0;          // the u16 property count (0: no properties)
    std::vector<XurRef> refs;       // this object's values, and its timelines' names and values (not its children's)
    uint32_t children_at = 0;       // the u32 child count (0: no children)
    std::vector<XurObject> children;
    uint32_t timelines_at = 0;      // the u32 timeline count (0: none)
    struct Timeline { uint32_t start, end; uint16_t element; };
    std::vector<Timeline> timelines;
    uint16_t id = 0;                // its Id (a string), 0 if none

    XurObject *child( const std::u16string &id, const struct XurFile &f );
    const XurRef *prop( const char *name ) const;
};

struct XurFile
{
    std::vector<uint8_t> bytes;
    struct Section { std::array<char, 4> name; uint32_t offset, size; };
    std::vector<Section> sections;
    std::vector<std::u16string> strings;
    XurObject root;
    std::string error;

    bool parse( const uint8_t *data, size_t size );
    const Section *section( const char *name ) const;
    uint16_t string_index( const std::u16string &s ) const;  // 1-based, 0 if absent
};

// Edits to one file, applied together by build().
struct XurEdit
{
    explicit XurEdit( const XurFile &f ) : file( f ) {}
    const XurFile &file;

    uint16_t string( const std::u16string &s );                   // its index (added if new)
    void rename_string( const std::u16string &from, const std::u16string &to );  // every use of a string becomes another
    uint32_t vector( float x, float y, float z );                 // a new vector's index
    void set_u16( uint32_t at, uint16_t v );
    void set_u32( uint32_t at, uint32_t v );
    void set_float( uint32_t at, float v );
    void set_string( const XurObject &o, const char *prop, const std::u16string &s );  // the object has the property
    void set_position( const XurObject &o, float x, float y );                         // the object has a Position
    void remove_child( const XurObject &parent, const XurObject &child );               // and its timelines in the parent
    // A copy of src (from this file or another) added as parent's last child, with id; src's timelines in
    // src_parent come along for it. Strings, vectors and quaternions it names are carried over. fix(edit,
    // offset of the copy in the new file's terms is unknown, so) edits to the copy go through patch:
    // patch(copy_bytes, copy's refs relative to its start).
    struct Copy
    {
        std::vector<uint8_t> bytes;
        std::vector<XurRef> refs;  // offsets relative to bytes
        std::vector<std::vector<uint8_t>> timelines;
        void set_string( XurEdit &e, const char *prop, const std::u16string &s, int nth = 0 );  // the nth object in it that has prop
        void set_position( XurEdit &e, float x, float y );
        void set_float( const char *prop, float v );  // the first float property of that name
    };
    Copy copy( const XurFile &src, const XurObject &obj, const XurObject *src_parent, const std::u16string &id );
    void add_child( const XurObject &parent, const Copy &c );

    std::vector<uint8_t> build() const;

private:
    struct Splice { uint32_t at, remove; std::vector<uint8_t> insert; };
    std::vector<Splice> splices;
    std::vector<std::u16string> new_strings;
    std::vector<std::pair<uint16_t, std::u16string>> renamed;
    std::vector<std::array<uint8_t, 12>> new_vectors;
    std::vector<std::array<uint8_t, 16>> new_quats;
    std::vector<std::pair<uint32_t, int32_t>> count_changes;  // (offset of a u32 count, change)
    uint32_t quaternion( const uint8_t *q );
};

// XZP archives ("XUIZ"): the files in one, and one rebuilt with some replaced or added.
struct XzpEntry { std::u16string name; uint32_t offset, size; };
bool xzp_list( const std::vector<uint8_t> &a, std::vector<XzpEntry> &out, uint32_t &data_start );
std::vector<uint8_t> xzp_build( const std::vector<uint8_t> &a, const std::vector<std::pair<std::u16string, std::vector<uint8_t>>> &replace );
