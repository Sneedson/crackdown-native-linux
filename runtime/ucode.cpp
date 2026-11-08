// Xenos shader microcode decoding and disassembly (see ucode.h).
#include "ucode.h"

#include <algorithm>
#include <cstdio>

namespace ucode
{

void Program::parse()
{
    cf.clear();
    // Control flow is a sequence of 48-bit instructions, two per three dwords, from dword 0. It runs to
    // an end, but code after an end can still be reached by a jump or call (a static branch that skips
    // to a second export: the HUD sprites' pixel shader 611AF6B6), so parsing goes on past an end until
    // no jump or call target lies beyond it.
    uint32_t reach = 0;  // highest jump / call target seen
    auto note = [&]( const Cf &c ) { if ( c.op() == CfOp::CondJmp || c.op() == CfOp::CondCall ) reach = std::max( reach, c.target() ); };
    for ( size_t i = 0; i + 2 < words.size() || ( i + 1 < words.size() && cf.empty() ); i += 3 )
    {
        uint32_t w0 = words[i], w1 = i + 1 < words.size() ? words[i + 1] : 0, w2 = i + 2 < words.size() ? words[i + 2] : 0;
        Cf a{ (uint64_t)w0 | ( (uint64_t)( w1 & 0xFFFF ) << 32 ) };
        Cf b{ (uint64_t)( w1 >> 16 ) | ( (uint64_t)w2 << 16 ) };
        cf.push_back( a );
        note( a );
        if ( a.is_end() && cf.size() > reach ) break;
        cf.push_back( b );
        note( b );
        if ( b.is_end() && cf.size() > reach ) break;
        if ( cf.size() > 512 ) break;
    }
}

const char *vector_op_name( uint32_t op )
{
    static const char *names[32] = { "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad",
                                     "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push",
                                     "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne",
                                     "dst", "maxa", "v30?", "v31?" };
    return names[op & 31];
}

const char *scalar_op_name( uint32_t op )
{
    static const char *names[64] = {
        "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes", "frcs",
        "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas", "maxasf", "subs",
        "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr", "setp_rstr",
        "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "s41?", "mulsc", "mulsc", "addsc", "addsc",
        "subsc", "subsc", "sin", "cos", "retain_prev" };
    return op < 51 ? names[op] : "s?";
}

namespace
{

std::string swizzle_text( uint32_t swz, int comps = 4 )
{
    // Each 2-bit field is an offset from the identity component.
    static const char xyzw[] = "xyzw";
    std::string s;
    for ( int i = 0; i < comps; ++i ) s += xyzw[( ( swz >> ( i * 2 ) ) + i ) & 3];
    return s == std::string( "xyzw" ).substr( 0, comps ) ? "" : "." + s;
}

std::string src_text( const Alu &a, int src, int comps = 4 )
{
    char buf[48];
    uint32_t r = a.reg( src );
    if ( a.is_temp( src ) )
        snprintf( buf, sizeof(buf), "%s%sr%u%s%s", a.negate( src ) ? "-" : "", r & 0x80 ? "|" : "", r & 0x3F, r & 0x40 ? "[aL]" : "", r & 0x80 ? "|" : "" );
    else
        snprintf( buf, sizeof(buf), "%s%sc%u%s", a.negate( src ) ? "-" : "", a.abs_constants() ? "|" : "", r, a.abs_constants() ? "|" : "" );
    return buf + swizzle_text( a.swizzle( src ), comps );
}

std::string dest_text( bool export_, uint32_t reg, uint32_t mask, bool vertex )
{
    char buf[32];
    if ( !export_ ) snprintf( buf, sizeof(buf), "r%u", reg );
    else if ( vertex ) snprintf( buf, sizeof(buf), reg == 62 ? "oPos" : reg == 63 ? "oPts" : "o%u", reg );
    else snprintf( buf, sizeof(buf), reg == 61 ? "oDepth" : "oC%u", reg );
    std::string s = buf;
    if ( mask != 0xF ) { s += "."; for ( int i = 0; i < 4; ++i ) if ( mask & ( 1 << i ) ) s += "xyzw"[i]; }
    return s;
}

int vector_sources( uint32_t op )
{
    switch ( op )
    {
    case 8: case 9: case 10: case 29: return 1;           // frc trunc floor maxa(1 src... uses 1)
    case 11: case 12: case 13: case 14: case 17: return 3;   // mad, cnd*, dp2add
    default: return 2;
    }
}

std::string alu_text( const Alu &a, bool vertex )
{
    std::string out;
    if ( a.predicated() ) out += a.pred_condition() ? "(p) " : "(!p) ";
    bool vector_used = a.vector_mask() || a.vector_op() >= 20;
    if ( vector_used )
    {
        out += std::string( vector_op_name( a.vector_op() ) ) + ( a.vector_clamp() ? "_sat " : " " );
        out += dest_text( a.export_data(), a.vector_dest(), a.vector_mask(), vertex );
        int n = vector_sources( a.vector_op() );
        for ( int s = 1; s <= n; ++s ) out += ", " + src_text( a, s );
    }
    if ( a.scalar_mask() || a.scalar_op() >= 27 )
    {
        if ( vector_used ) out += "  +  ";
        out += std::string( scalar_op_name( a.scalar_op() ) ) + ( a.scalar_clamp() ? "_sat " : " " );
        out += dest_text( a.export_data(), a.export_data() ? a.vector_dest() : a.scalar_dest(), a.scalar_mask(), vertex );
        out += ", " + src_text( a, 3, 2 );
    }
    if ( out.empty() ) out = "nop";
    return out;
}

std::string fetch_text( const Fetch &f )
{
    char buf[160];
    static const char comp[] = "xyzw01?_";
    std::string dsw;
    for ( int i = 0; i < 4; ++i ) dsw += comp[( f.dst_swizzle() >> ( i * 3 ) ) & 7];
    if ( f.op() == 0 )
        snprintf( buf, sizeof(buf), "vfetch%s r%u.%s, r%u.%c, vf%u (fmt %u%s%s, stride %u, offset %d)", f.vtx_mini_fetch() ? "_mini" : "",
                  f.dst_reg(), dsw.c_str(), f.src_reg(), "xyzw"[f.vtx_src_swizzle()], f.const_index() * 3 + f.vtx_const_sel(),
                  f.vtx_data_format(), f.vtx_format_signed() ? " signed" : "", f.vtx_num_format_int() ? " int" : "", f.vtx_stride(), f.vtx_offset() );
    else if ( f.op() == 1 )
        snprintf( buf, sizeof(buf), "tfetch r%u.%s, r%u%s, tf%u%s", f.dst_reg(), dsw.c_str(), f.src_reg(),
                  swizzle_text( f.tex_src_swizzle(), 3 ).c_str(), f.const_index(), f.tex_unnormalized() ? " (unnormalised)" : "" );
    else
        snprintf( buf, sizeof(buf), "fetch_op%u r%u, r%u, tf%u", f.op(), f.dst_reg(), f.src_reg(), f.const_index() );
    return buf;
}

} // namespace

std::string disassemble( const Program &p )
{
    std::string out;
    char buf[96];
    bool vertex = p.type == 0;
    for ( size_t i = 0; i < p.cf.size(); ++i )
    {
        const Cf &c = p.cf[i];
        static const char *cf_names[16] = { "nop", "exec", "exece", "cexec", "cexece", "cpexec", "cpexece", "loop", "endloop",
                                            "ccall", "ret", "cjmp", "alloc", "cexec_pc", "cexece_pc", "vfetch_done" };
        snprintf( buf, sizeof(buf), "%3zu  %s", i, cf_names[(int)c.op()] );
        out += buf;
        if ( c.op() == CfOp::Alloc )
        {
            static const char *types[4] = { "none", "position", "interpolators", "memory" };
            snprintf( buf, sizeof(buf), " %s %u\n", types[c.alloc_type()], c.alloc_size() );
            out += buf;
            continue;
        }
        if ( c.op() == CfOp::CondJmp || c.op() == CfOp::CondCall || c.op() == CfOp::LoopStart || c.op() == CfOp::LoopEnd )
        {
            snprintf( buf, sizeof(buf), " -> %u%s b%u\n", c.target(), c.unconditional() ? " (always)" : "", c.bool_address() );
            out += buf;
            continue;
        }
        if ( !c.is_exec() ) { out += "\n"; continue; }
        if ( c.op() == CfOp::CondExec || c.op() == CfOp::CondExecEnd ) { snprintf( buf, sizeof(buf), " if %sb%u", c.condition() ? "" : "!", c.bool_address() ); out += buf; }
        if ( c.op() == CfOp::CondPredExec || c.op() == CfOp::CondPredExecEnd ) out += c.condition() ? " if p" : " if !p";
        snprintf( buf, sizeof(buf), " @%u x%u\n", c.address(), c.count() );
        out += buf;
        for ( uint32_t k = 0; k < c.count(); ++k )
        {
            uint32_t slot = c.address() + k;
            bool fetch = ( c.sequence() >> ( k * 2 ) ) & 1;
            uint32_t w[3] = { p.slot_word( slot, 0 ), p.slot_word( slot, 1 ), p.slot_word( slot, 2 ) };
            out += "        ";
            if ( fetch ) out += fetch_text( Fetch{ { w[0], w[1], w[2] } } );
            else out += alu_text( Alu{ { w[0], w[1], w[2] } }, vertex );
            out += "\n";
        }
    }
    return out;
}

} // namespace ucode

namespace ucode
{
std::vector<uint32_t> binding_mask( const Program &p )
{
    std::vector<uint32_t> mask( p.words.size() );
    if ( p.type != 0 ) return mask;
    for ( const Cf &c : p.cf )
    {
        if ( !c.is_exec() ) continue;
        for ( uint32_t k = 0; k < c.count(); ++k )
        {
            uint32_t slot = c.address() + k;
            if ( !( ( c.sequence() >> ( k * 2 ) ) & 1 ) || slot * 3 + 2 >= p.words.size() ) continue;
            if ( ( p.words[slot * 3] & 0x1F ) != 0 ) continue;  // not a vertex fetch
            mask[slot * 3 + 0] = 0x3FF7F000;  // destination register, fetch constant, its dword pair, prefetch count
            mask[slot * 3 + 1] = 0x5FBF3FFF;  // destination swizzle, signed / integer, format, exponent adjust, mini fetch
            mask[slot * 3 + 2] = 0x7FFFFFFF;  // stride, offset
        }
    }
    return mask;
}

bool valid_for_stage( const Program &p )
{
    bool exports = false;
    for ( const Cf &c : p.cf )
    {
        if ( !c.is_exec() ) continue;
        for ( uint32_t k = 0; k < c.count(); ++k )
        {
            const uint32_t slot = c.address() + k, w[3] = { p.slot_word( slot, 0 ), p.slot_word( slot, 1 ), p.slot_word( slot, 2 ) };
            if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
            {
                const uint32_t op = Fetch{ { w[0], w[1], w[2] } }.op();
                if ( !( op <= 1 || ( op >= 16 && op <= 19 ) || ( op >= 24 && op <= 26 ) ) ) return false;
                continue;
            }
            const Alu a{ { w[0], w[1], w[2] } };
            if ( a.vector_op() >= 30 || a.scalar_op() == 41 || a.scalar_op() > 50 ) return false;
            if ( !a.export_data() ) continue;
            exports = true;
            const uint32_t r = a.vector_dest();
            if ( p.type ? !( r < 4 || r == 61 ) : !( r < 16 || r == 62 || r == 63 || ( r >= 32 && r <= 37 ) ) ) return false;
        }
    }
    return exports;
}

bool uses_memory_export( const Program &p )
{
    for ( const Cf &c : p.cf )
        if ( c.op() == CfOp::Alloc && c.alloc_type() == 3 ) return true;
    return false;
}

bool plausible_program( const uint32_t *w, size_t avail, uint32_t &dwords )
{
    if ( avail < 3 ) return false;
    Program p;
    p.words.assign( w, w + std::min<size_t>( avail, 0x4000 ) );
    p.parse();
    if ( p.cf.empty() || !p.cf.back().is_end() ) return false;
    uint32_t cf_slots = ( (uint32_t)p.cf.size() + 1 ) / 2, end = cf_slots, instructions = 0, first = ~0u;
    for ( const Cf &c : p.cf )
    {
        uint32_t op = (uint32_t)c.op();
        if ( c.is_exec() )
        {
            if ( c.address() < cf_slots ) return false;
            end = std::max( end, c.address() + c.count() );
            if ( c.count() ) first = std::min( first, c.address() );
            for ( uint32_t k = 0; k < c.count(); ++k )
            {
                uint32_t slot = c.address() + k;
                if ( slot * 3 + 2 >= p.words.size() ) return false;
                const uint32_t *d = &p.words[slot * 3];
                if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
                {
                    uint32_t fop = d[0] & 0x1F;
                    if ( fop != 0 && fop != 1 && fop != 16 && fop != 17 && fop != 18 && fop != 19 && fop != 24 && fop != 25 && fop != 26 ) return false;
                }
                else if ( ( ( d[0] >> 26 ) & 0x3F ) > 50 ) return false;  // scalar opcode
                ++instructions;
            }
        }
        else if ( op == (uint32_t)CfOp::Alloc )
        {
            if ( c.alloc_type() == 0 ) return false;
        }
        else if ( op == (uint32_t)CfOp::CondJmp || op == (uint32_t)CfOp::CondCall )
        {
            if ( c.target() >= p.cf.size() + 64 ) return false;
        }
    }
    if ( !instructions || first != cf_slots ) return false;  // the compiler puts the instructions right after the control flow
    dwords = end * 3;
    return dwords <= avail;
}


namespace
{
// The bits of a vertex fetch binding sets: all but the opcode and the source (register, swizzle), which
// the translated template reads.
constexpr uint32_t kFetchBound[3] = { 0x3FFFF000, 0xFFFFFFFF, 0xFFFFFFFF };
// The bits of an interpolator export linking sets: register and write masks.
constexpr uint32_t kExportBound0 = 0x00FF003F;
// What D3D writes over an export the pixel shader doesn't read.
constexpr uint32_t kDroppedExport[3] = { 0xC8000000, 0x00000000, 0x02000000 };

template <class F> void each_slot( const Program &p, F f )  // f(slot, is fetch), each slot once
{
    std::vector<bool> done;
    for ( const Cf &c : p.cf )
    {
        if ( !c.is_exec() ) continue;
        for ( uint32_t k = 0; k < c.count(); ++k )
        {
            uint32_t slot = c.address() + k;
            if ( slot >= done.size() ) done.resize( slot + 1 );
            if ( done[slot] ) continue;
            done[slot] = true;
            f( slot, ( ( c.sequence() >> ( k * 2 ) ) & 1 ) != 0 );
        }
    }
}

bool dropped( const Program &p, uint32_t slot )
{
    for ( int i = 0; i < 3; ++i )
        if ( p.slot_word( slot, i ) != kDroppedExport[i] ) return false;
    return true;
}
} // namespace

bool linked_export( const Alu &a ) { return a.export_data() && !a.vector_dest_rel() && a.vector_dest() < 16; }

bool binds_to( const Program &tmpl, const Program &bound )
{
    if ( tmpl.type != 0 || bound.type != 0 || template_key( tmpl ) != template_key( bound ) ) return false;
    bool ok = true;
    each_slot( tmpl, [&]( uint32_t slot, bool fetch ) {
        uint32_t t[3] = { tmpl.slot_word( slot, 0 ), tmpl.slot_word( slot, 1 ), tmpl.slot_word( slot, 2 ) };
        uint32_t b[3] = { bound.slot_word( slot, 0 ), bound.slot_word( slot, 1 ), bound.slot_word( slot, 2 ) };
        if ( fetch && ( t[0] & 0x1F ) == 0 )
        {
            for ( int i = 0; i < 3; ++i ) ok &= ( ( t[i] ^ b[i] ) & ~kFetchBound[i] ) == 0;
            ok &= ( b[0] & 0x1F ) == 0;
        }
        else if ( !fetch && linked_export( Alu{ { t[0], t[1], t[2] } } ) )
        {
            bool relinked = ( ( t[0] ^ b[0] ) & ~kExportBound0 ) == 0 && t[1] == b[1] && t[2] == b[2] && Alu{ { b[0], b[1], b[2] } }.vector_dest() < 16;
            ok &= relinked || dropped( bound, slot );
        }
        else ok &= t[0] == b[0] && t[1] == b[1] && t[2] == b[2];
    } );
    return ok;
}

uint64_t template_key( const Program &p )
{
    uint64_t h = 1469598103934665603ull ^ p.cf.size();
    size_t cf_dwords = std::min( p.words.size(), ( ( p.cf.size() + 1 ) / 2 ) * 3 );
    for ( size_t i = 0; i < cf_dwords; ++i ) h = ( h ^ p.words[i] ) * 1099511628211ull;
    return h;
}

std::vector<std::pair<uint32_t, uint32_t>> vs_specialization( const Program &tmpl, const Program &bound )
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if ( tmpl.type != 0 ) return out;
    each_slot( tmpl, [&]( uint32_t slot, bool fetch ) {
        if ( fetch )
        {
            if ( ( tmpl.slot_word( slot, 0 ) & 0x1F ) != 0 ) return;
            Fetch f{ { bound.slot_word( slot, 0 ), bound.slot_word( slot, 1 ), bound.slot_word( slot, 2 ) } };
            uint32_t id = slot * 8;
            out.push_back( { id + 0, f.const_index() * 3 + f.vtx_const_sel() } );
            out.push_back( { id + 1, f.vtx_data_format() } );
            out.push_back( { id + 2, ( f.vtx_format_signed() ? 1u : 0u ) | ( f.vtx_num_format_int() ? 2u : 0u ) } );
            out.push_back( { id + 3, (uint32_t)f.vtx_exp_adjust() } );
            out.push_back( { id + 4, f.vtx_stride() } );
            out.push_back( { id + 5, (uint32_t)f.vtx_offset() } );
            out.push_back( { id + 6, f.dst_swizzle() } );
            out.push_back( { id + 7, f.dst_reg() } );
            return;
        }
        if ( !linked_export( Alu{ { tmpl.slot_word( slot, 0 ), tmpl.slot_word( slot, 1 ), tmpl.slot_word( slot, 2 ) } } ) ) return;
        Alu b{ { bound.slot_word( slot, 0 ), bound.slot_word( slot, 1 ), bound.slot_word( slot, 2 ) } };
        bool live = !dropped( bound, slot ) && b.export_data();
        uint32_t id = kExportSpecBase + slot * 4;
        out.push_back( { id + 0, live ? b.vector_dest() : 0u } );
        out.push_back( { id + 1, live ? b.vector_mask() : 0u } );
        out.push_back( { id + 2, live ? b.scalar_mask() : 0u } );
    } );
    return out;
}

} // namespace ucode
