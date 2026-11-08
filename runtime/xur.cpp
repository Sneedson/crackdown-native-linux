// XUI scenes (.xur) and their archives (.xzp), read and edited (xur.h).
#include "xur.h"
#include "xui_schema.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace
{
uint16_t rd16( const uint8_t *p ) { return (uint16_t)( p[0] << 8 | p[1] ); }
uint32_t rd32( const uint8_t *p ) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void wr16( uint8_t *p, uint16_t v ) { p[0] = v >> 8, p[1] = (uint8_t)v; }
void wr32( uint8_t *p, uint32_t v ) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = (uint8_t)v; }
void put16( std::vector<uint8_t> &o, uint16_t v ) { o.push_back( v >> 8 ), o.push_back( (uint8_t)v ); }
void put32( std::vector<uint8_t> &o, uint32_t v ) { put16( o, v >> 16 ), put16( o, (uint16_t)v ); }

const XurClass *find_class( const std::string &name )
{
    for ( auto &c : XUR_CLASSES )
        if ( name == c.name ) return &c;
    return nullptr;
}
// Base first.
std::vector<const XurClass *> chain( const std::string &name )
{
    std::vector<const XurClass *> out;
    for ( const XurClass *c = find_class( name ); c; c = *c->base ? find_class( c->base ) : nullptr ) out.push_back( c );
    std::reverse( out.begin(), out.end() );
    return out;
}

struct Reader
{
    const XurFile &f;
    uint32_t p, end;
    bool bad = false;
    bool need( uint32_t n ) { if ( p + n > end ) bad = true; return !bad; }
    uint8_t u8() { return need( 1 ) ? f.bytes[p++] : 0; }
    uint16_t u16() { if ( !need( 2 ) ) return 0; uint16_t v = rd16( &f.bytes[p] ); p += 2; return v; }
    uint32_t u32() { if ( !need( 4 ) ) return 0; uint32_t v = rd32( &f.bytes[p] ); p += 4; return v; }
};

void scalar( Reader &r, uint8_t type, const char *prop, std::vector<XurRef> &refs )
{
    refs.push_back( { r.p, type, prop } );
    if ( type == 1 ) r.u8();
    else if ( type == 5 ) r.u16();
    else r.u32();
}

void property_block( Reader &r, const XurDef *defs, uint8_t n, std::vector<XurRef> &refs );

void value( Reader &r, const XurDef &d, std::vector<XurRef> &refs )
{
    uint32_t count = 1;
    if ( d.indexed )
    {
        uint8_t b = r.u8();
        if ( b & 0x80 )
        {
            count = 0;
            for ( int k = 0; k < ( b & 0x7F ); ++k ) count = count << 8 | r.u8();
        }
        else count = b;
    }
    for ( uint32_t i = 0; i < count && !r.bad; ++i )
        if ( d.type == 9 ) r.u16(), property_block( r, d.sub, d.nsub, refs );
        else scalar( r, d.type, d.name, refs );
}

void property_block( Reader &r, const XurDef *defs, uint8_t n, std::vector<XurRef> &refs )
{
    uint8_t b = r.u8(), size = b & 7;
    uint32_t mask = 0;
    for ( int k = 0; k < size; ++k ) mask = mask << 8 | r.u8();
    for ( uint8_t i = 0; i < n && !r.bad; ++i )
        if ( mask >> i & 1 ) value( r, defs[i], refs );
    if ( mask >> n ) r.bad = true;
}

bool object( Reader &r, XurObject &o )
{
    o.start = r.p;
    uint32_t class_at = r.p;
    uint16_t ci = r.u16();
    uint8_t flags = r.u8();
    if ( r.bad || !ci || ci > r.f.strings.size() ) return false;
    o.cls.clear();
    for ( char16_t ch : r.f.strings[ci - 1] ) o.cls += (char)ch;
    o.refs.push_back( { class_at, 0, nullptr } );
    if ( flags & 1 )
    {
        o.props_at = r.p;
        r.u16();
        auto ch = chain( o.cls );
        if ( ch.empty() ) return false;
        for ( auto *c : ch ) property_block( r, c->defs, c->n, o.refs );
        for ( auto &ref : o.refs )
            if ( ref.prop && !strcmp( ref.prop, "Id" ) ) { o.id = rd16( &r.f.bytes[ref.at] ); break; }
    }
    if ( flags & 2 )
    {
        o.children_at = r.p;
        uint32_t n = r.u32();
        if ( n > 100000 ) return false;
        o.children.resize( n );
        for ( auto &c : o.children )
            if ( !object( r, c ) ) return false;
    }
    if ( flags & 4 )
    {
        uint32_t frames = r.u32();
        for ( uint32_t i = 0; i < frames && !r.bad; ++i )
        {
            o.refs.push_back( { r.p, 5, nullptr } );
            r.u16(), r.u32(), r.u8();
            o.refs.push_back( { r.p, 5, nullptr } );
            r.u16();
        }
        if ( !o.children.empty() )
        {
            o.timelines_at = r.p;
            uint32_t n = r.u32();
            for ( uint32_t t = 0; t < n && !r.bad; ++t )
            {
                XurObject::Timeline tl{ r.p, 0, 0 };
                o.refs.push_back( { r.p, 5, nullptr } );
                tl.element = r.u16();
                const XurObject *target = nullptr;
                for ( auto &c : o.children )
                    if ( c.id == tl.element ) target = &c;
                if ( !target ) return false;
                auto ch = chain( target->cls );
                std::reverse( ch.begin(), ch.end() );  // the element's own class first
                uint32_t props = r.u32();
                std::vector<const XurDef *> types;
                for ( uint32_t i = 0; i < props && !r.bad; ++i )
                {
                    uint8_t b = r.u8(), depth = b & 0x7F;
                    const XurDef *d = nullptr;
                    if ( depth )
                    {
                        uint8_t level = r.u8(), idx = r.u8();
                        if ( level < ch.size() && idx < ch[level]->n ) d = &ch[level]->defs[idx];
                        for ( int k = 1; k < depth; ++k )
                        {
                            uint8_t s = r.u8();
                            d = d && s < d->nsub ? &d->sub[s] : nullptr;
                        }
                    }
                    if ( b & 0x80 ) r.u32();
                    if ( !d ) return false;
                    types.push_back( d );
                }
                uint32_t keys = r.u32();
                for ( uint32_t k = 0; k < keys && !r.bad; ++k )
                {
                    r.u32(), r.u32();
                    for ( auto *d : types )
                        if ( d->type != 9 && d->type != 10 ) scalar( r, d->type, d->name, o.refs );
                }
                tl.end = r.p;
                o.timelines.push_back( tl );
            }
        }
    }
    o.end = r.p;
    return !r.bad;
}

void subtree_refs( const XurObject &o, std::vector<XurRef> &out )
{
    out.insert( out.end(), o.refs.begin(), o.refs.end() );
    for ( auto &c : o.children ) subtree_refs( c, out );
}
} // namespace

XurObject *XurObject::child( const std::u16string &name, const XurFile &f )
{
    uint16_t i = f.string_index( name );
    for ( auto &c : children )
        if ( i && c.id == i ) return &c;
    return nullptr;
}

const XurRef *XurObject::prop( const char *name ) const
{
    for ( auto &r : refs )
        if ( r.prop && !strcmp( r.prop, name ) ) return &r;
    return nullptr;
}

const XurFile::Section *XurFile::section( const char *name ) const
{
    for ( auto &s : sections )
        if ( !memcmp( s.name.data(), name, 4 ) ) return &s;
    return nullptr;
}

uint16_t XurFile::string_index( const std::u16string &s ) const
{
    for ( size_t i = 0; i < strings.size(); ++i )
        if ( strings[i] == s ) return (uint16_t)( i + 1 );
    return 0;
}

bool XurFile::parse( const uint8_t *data, size_t size )
{
    bytes.assign( data, data + size );
    if ( size < 0x14 || memcmp( data, "XUIB", 4 ) || rd32( data + 4 ) != 5 ) { error = "not a version 5 XUIB"; return false; }
    uint32_t flags = rd32( data + 8 ), table = 0x14 + ( flags & 1 ? 40 : 0 );
    uint16_t n = rd16( data + 0x12 );
    if ( table + n * 12 > size ) { error = "sections"; return false; }
    for ( uint16_t i = 0; i < n; ++i )
    {
        Section s;
        memcpy( s.name.data(), data + table + i * 12, 4 );
        s.offset = rd32( data + table + i * 12 + 4 ), s.size = rd32( data + table + i * 12 + 8 );
        if ( s.offset + s.size > size ) { error = "section past the end"; return false; }
        sections.push_back( s );
    }
    if ( const Section *st = section( "STRN" ) )
        for ( uint32_t p = st->offset; p + 2 <= st->offset + st->size; )
        {
            uint16_t len = rd16( data + p );
            std::u16string s;
            for ( uint16_t k = 0; k < len; ++k ) s += (char16_t)rd16( data + p + 2 + k * 2 );
            strings.push_back( s );
            p += 2 + len * 2;
        }
    const Section *d = section( "DATA" );
    if ( !d ) { error = "no DATA"; return false; }
    Reader r{ *this, d->offset, d->offset + d->size };
    if ( !object( r, root ) || r.p != r.end ) { error = "objects don't read (" + root.cls + ")"; return false; }
    return true;
}

uint16_t XurEdit::string( const std::u16string &s )
{
    if ( uint16_t i = file.string_index( s ) ) return i;
    for ( size_t i = 0; i < new_strings.size(); ++i )
        if ( new_strings[i] == s ) return (uint16_t)( file.strings.size() + i + 1 );
    new_strings.push_back( s );
    return (uint16_t)( file.strings.size() + new_strings.size() );
}

void XurEdit::rename_string( const std::u16string &from, const std::u16string &to )
{
    if ( uint16_t i = file.string_index( from ) ) renamed.push_back( { i, to } );
}

uint32_t XurEdit::vector( float x, float y, float z )
{
    std::array<uint8_t, 12> v;
    const float xyz[3] = { x, y, z };
    for ( int i = 0; i < 3; ++i )
    {
        uint32_t b;
        memcpy( &b, &xyz[i], 4 );
        wr32( &v[i * 4], b );
    }
    const XurFile::Section *s = file.section( "VECT" );
    uint32_t have = s ? s->size / 12 : 0;
    new_vectors.push_back( v );
    return have + (uint32_t)new_vectors.size() - 1;
}

uint32_t XurEdit::quaternion( const uint8_t *q )
{
    std::array<uint8_t, 16> v;
    memcpy( v.data(), q, 16 );
    const XurFile::Section *s = file.section( "QUAT" );
    new_quats.push_back( v );
    return ( s ? s->size / 16 : 0 ) + (uint32_t)new_quats.size() - 1;
}

void XurEdit::set_u16( uint32_t at, uint16_t v ) { Splice s{ at, 2, {} }; put16( s.insert, v ); splices.push_back( s ); }
void XurEdit::set_u32( uint32_t at, uint32_t v ) { Splice s{ at, 4, {} }; put32( s.insert, v ); splices.push_back( s ); }
void XurEdit::set_float( uint32_t at, float v ) { uint32_t b; memcpy( &b, &v, 4 ); set_u32( at, b ); }

void XurEdit::set_string( const XurObject &o, const char *prop, const std::u16string &s )
{
    if ( const XurRef *r = o.prop( prop ) ) set_u16( r->at, string( s ) );
}

void XurEdit::set_position( const XurObject &o, float x, float y )
{
    if ( const XurRef *r = o.prop( "Position" ) ) set_u32( r->at, vector( x, y, 0 ) );
}

void XurEdit::remove_child( const XurObject &parent, const XurObject &child )
{
    splices.push_back( { child.start, child.end - child.start, {} } );
    count_changes.push_back( { parent.children_at, -1 } );
    for ( auto &t : parent.timelines )
        if ( t.element == child.id )
        {
            splices.push_back( { t.start, t.end - t.start, {} } );
            count_changes.push_back( { parent.timelines_at, -1 } );
        }
}

XurEdit::Copy XurEdit::copy( const XurFile &src, const XurObject &obj, const XurObject *src_parent, const std::u16string &id )
{
    Copy c;
    c.bytes.assign( src.bytes.begin() + obj.start, src.bytes.begin() + obj.end );
    std::vector<XurRef> refs;
    subtree_refs( obj, refs );
    auto remap = [&]( std::vector<uint8_t> &bytes, uint32_t base, const XurRef &r ) {
        uint8_t *p = &bytes[r.at - base];
        if ( r.type == 0 || r.type == 5 )
        {
            uint16_t i = rd16( p );
            if ( i && i <= src.strings.size() ) wr16( p, string( src.strings[i - 1] ) );
        }
        else if ( r.type == 7 || r.type == 8 )
        {
            const XurFile::Section *s = src.section( r.type == 7 ? "VECT" : "QUAT" );
            uint32_t i = rd32( p ), size = r.type == 7 ? 12 : 16;
            if ( s && ( i + 1 ) * size <= s->size )
            {
                const uint8_t *v = &src.bytes[s->offset + i * size];
                wr32( p, r.type == 7 ? vector( 0, 0, 0 ) : quaternion( v ) );
                if ( r.type == 7 ) memcpy( new_vectors.back().data(), v, 12 );
            }
        }
    };
    for ( auto &r : refs )
    {
        remap( c.bytes, obj.start, r );
        c.refs.push_back( { r.at - obj.start, r.type, r.prop } );
    }
    uint16_t new_id = string( id );
    for ( auto &r : c.refs )
        if ( r.prop && !strcmp( r.prop, "Id" ) ) { wr16( &c.bytes[r.at], new_id ); break; }
    if ( src_parent )
        for ( auto &t : src_parent->timelines )
            if ( t.element == obj.id )
            {
                std::vector<uint8_t> tb( src.bytes.begin() + t.start, src.bytes.begin() + t.end );
                for ( auto &r : src_parent->refs )
                    if ( r.at >= t.start && r.at < t.end ) remap( tb, t.start, r );
                wr16( tb.data(), new_id );
                c.timelines.push_back( tb );
            }
    return c;
}

void XurEdit::Copy::set_string( XurEdit &e, const char *prop, const std::u16string &s, int nth )
{
    for ( auto &r : refs )
        if ( r.prop && !strcmp( r.prop, prop ) && nth-- == 0 ) { wr16( &bytes[r.at], e.string( s ) ); return; }
}

void XurEdit::Copy::set_position( XurEdit &e, float x, float y )
{
    for ( auto &r : refs )
        if ( r.prop && !strcmp( r.prop, "Position" ) ) { wr32( &bytes[r.at], e.vector( x, y, 0 ) ); return; }
}

void XurEdit::Copy::set_float( const char *prop, float v )
{
    for ( auto &r : refs )
        if ( r.prop && !strcmp( r.prop, prop ) && r.type == 4 )
        {
            uint32_t b;
            memcpy( &b, &v, 4 );
            wr32( &bytes[r.at], b );
            return;
        }
}

void XurEdit::add_child( const XurObject &parent, const Copy &c )
{
    splices.push_back( { parent.children.back().end, 0, c.bytes } );
    count_changes.push_back( { parent.children_at, 1 } );
    if ( !parent.timelines_at ) return;
    uint32_t at = parent.timelines.empty() ? parent.timelines_at + 4 : parent.timelines.back().end;
    for ( auto &t : c.timelines )
    {
        splices.push_back( { at, 0, t } );
        count_changes.push_back( { parent.timelines_at, 1 } );
    }
}

std::vector<uint8_t> XurEdit::build() const
{
    // DATA with the counts changed and the splices made (last first; at one place in the order made).
    std::vector<uint8_t> bytes = file.bytes;
    std::map<uint32_t, int32_t> counts;
    for ( auto &c : count_changes ) counts[c.first] += c.second;
    for ( auto &c : counts ) wr32( &bytes[c.first], rd32( &bytes[c.first] ) + c.second );
    std::vector<size_t> order( splices.size() );
    for ( size_t i = 0; i < order.size(); ++i ) order[i] = i;
    std::stable_sort( order.begin(), order.end(), [&]( size_t a, size_t b ) { return splices[a].at != splices[b].at ? splices[a].at > splices[b].at : a > b; } );
    const XurFile::Section *d = file.section( "DATA" );
    std::vector<uint8_t> data( bytes.begin() + d->offset, bytes.begin() + d->offset + d->size );
    for ( size_t i : order )
    {
        const Splice &s = splices[i];
        uint32_t at = s.at - d->offset;
        data.erase( data.begin() + at, data.begin() + at + s.remove );
        data.insert( data.begin() + at, s.insert.begin(), s.insert.end() );
    }
    // The sections, in the file's order, with STRN, VECT and QUAT grown.
    std::vector<std::pair<XurFile::Section, std::vector<uint8_t>>> out;
    bool have_quat = false;
    for ( auto &s : file.sections )
    {
        std::vector<uint8_t> b( file.bytes.begin() + s.offset, file.bytes.begin() + s.offset + s.size );
        if ( !memcmp( s.name.data(), "STRN", 4 ) )
        {
            b.clear();
            std::vector<std::u16string> all = file.strings;
            for ( auto &r : renamed ) all[r.first - 1] = r.second;
            all.insert( all.end(), new_strings.begin(), new_strings.end() );
            for ( auto &str : all )
            {
                put16( b, (uint16_t)str.size() );
                for ( char16_t ch : str ) put16( b, ch );
            }
        }
        if ( !memcmp( s.name.data(), "VECT", 4 ) )
            for ( auto &v : new_vectors ) b.insert( b.end(), v.begin(), v.end() );
        if ( !memcmp( s.name.data(), "QUAT", 4 ) )
        {
            have_quat = true;
            for ( auto &q : new_quats ) b.insert( b.end(), q.begin(), q.end() );
        }
        if ( !memcmp( s.name.data(), "DATA", 4 ) ) b = data;
        out.push_back( { s, b } );
    }
    if ( !new_quats.empty() && !have_quat )  // (a file without quaternions getting one: before DATA)
    {
        std::vector<uint8_t> b;
        for ( auto &q : new_quats ) b.insert( b.end(), q.begin(), q.end() );
        XurFile::Section s{ { 'Q', 'U', 'A', 'T' }, 0, 0 };
        out.insert( out.end() - 1, { s, b } );
    }
    // The header without the loader's hints (flag 1), then the sections.
    std::vector<uint8_t> f( file.bytes.begin(), file.bytes.begin() + 0x14 );
    wr32( &f[8], rd32( &f[8] ) & ~1u );
    wr16( &f[0x12], (uint16_t)out.size() );
    uint32_t at = 0x14 + (uint32_t)out.size() * 12;
    for ( auto &s : out )
    {
        f.insert( f.end(), s.first.name.begin(), s.first.name.end() );
        put32( f, at ), put32( f, (uint32_t)s.second.size() );
        at += (uint32_t)s.second.size();
    }
    for ( auto &s : out ) f.insert( f.end(), s.second.begin(), s.second.end() );
    wr32( &f[0x0E], (uint32_t)f.size() );
    return f;
}

bool xzp_list( const std::vector<uint8_t> &a, std::vector<XzpEntry> &out, uint32_t &data_start )
{
    if ( a.size() < 0x16 || memcmp( a.data(), "XUIZ", 4 ) ) return false;
    uint32_t table = rd32( &a[0x10] ), p = 0x16;
    uint16_t n = rd16( &a[0x14] );
    data_start = 0x16 + table;
    for ( uint16_t i = 0; i < n; ++i )
    {
        if ( p + 9 > a.size() ) return false;
        XzpEntry e;
        e.size = rd32( &a[p] ), e.offset = rd32( &a[p + 4] );
        uint8_t len = a[p + 8];
        p += 9;
        for ( uint8_t k = 0; k < len; ++k ) e.name += (char16_t)rd16( &a[p + k * 2] );
        p += len * 2;
        if ( data_start + e.offset + e.size > a.size() ) return false;
        out.push_back( e );
    }
    return p == data_start;
}

std::vector<uint8_t> xzp_build( const std::vector<uint8_t> &a, const std::vector<std::pair<std::u16string, std::vector<uint8_t>>> &replace )
{
    std::vector<XzpEntry> entries;
    uint32_t data_start;
    if ( !xzp_list( a, entries, data_start ) ) return {};
    std::vector<std::pair<std::u16string, std::vector<uint8_t>>> files;
    for ( auto &e : entries ) files.push_back( { e.name, std::vector<uint8_t>( a.begin() + data_start + e.offset, a.begin() + data_start + e.offset + e.size ) } );
    for ( auto &r : replace )
    {
        auto it = std::find_if( files.begin(), files.end(), [&]( auto &f ) { return f.first == r.first; } );
        if ( it != files.end() ) it->second = r.second;
        else files.push_back( r );
    }
    std::vector<uint8_t> table, body;
    for ( auto &f : files )
    {
        put32( table, (uint32_t)f.second.size() ), put32( table, (uint32_t)body.size() );
        table.push_back( (uint8_t)f.first.size() );
        for ( char16_t ch : f.first ) put16( table, ch );
        body.insert( body.end(), f.second.begin(), f.second.end() );
    }
    std::vector<uint8_t> out( a.begin(), a.begin() + 0x10 );
    put32( out, (uint32_t)table.size() );
    put16( out, (uint16_t)files.size() );
    out.insert( out.end(), table.begin(), table.end() );
    out.insert( out.end(), body.begin(), body.end() );
    wr32( &out[8], (uint32_t)out.size() );
    return out;
}
