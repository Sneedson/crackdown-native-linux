#include <cctype>
// Xenos shader microcode -> GLSL 4.50 for Vulkan.
//
// Execution model of the generated code:
//   * temporaries are `vec4 r[64]`, the predicate is `bool p`, the scalar unit's previous result is
//     `float ps`, address registers are `int a0` (constant addressing) and `int aL` (loop index),
//     the level set by SET_TEX_LOD is `float tex_lod`;
//   * control flow is a state machine over control-flow instruction indices (`pc`), so jumps,
//     loops and calls work without restructuring;
//   * vertex fetches read guest memory directly (vfetch); 2D and cube texture fetches sample the texture
//     as a Vulkan image (tfetch, tfetchcube), others decode it from guest memory (tfetch2d; prelude.glsl.h).
// A vertex shader becomes `void xvs(uint vertex_index, out vec4 pos, out vec4 o[16], out float psize)`;
// the renderer's wrapper calls it (more than once for rectangle lists). A pixel shader becomes `main`.
#include "translate.h"
#include "prelude.glsl.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace xlate
{
namespace
{

using namespace ucode;

std::string fmt( const char *f, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
std::string fmt( const char *f, ... )
{
    char buf[512];
    va_list ap;
    va_start( ap, f );
    vsnprintf( buf, sizeof(buf), f, ap );
    va_end( ap );
    return buf;
}

const char *XYZW = "xyzw";

// Swizzle text for a source: component i reads ((swz >> 2i) + i) & 3.
std::string swz4( uint32_t swz )
{
    std::string s = ".";
    for ( int i = 0; i < 4; ++i ) s += XYZW[( ( swz >> ( i * 2 ) ) + i ) & 3];
    return s;
}

struct Emitter
{
    const Program &prog;
    bool pixel;
    std::string out;
    std::string decls;  // global declarations (vertex fetch specialization constants)
    std::vector<bool> declared, exports_declared;  // slots whose constants are declared
    uint32_t slot = 0;  // instruction slot being emitted
    int indent = 2;
    int debug_fetch = -1;  // texture fetch constant whose result the pixel shader outputs (debugging)

    void line( const std::string &s ) { out += std::string( indent * 2, ' ' ) + s + "\n"; }

    // Constant index expression for the n-th constant operand of an instruction (n = 0 or 1).
    std::string const_ref( const Alu &a, uint32_t reg, int n )
    {
        bool rel = n == 0 ? a.const0_rel() : a.const1_rel();
        std::string base = pixel ? fmt( "%u", reg + 256 ) : fmt( "%u", reg );
        if ( !rel ) return "dc.c[" + base + "]";
        return "dc.c[clamp(" + base + ( a.address_absolute() ? " + a0" : " + aL" ) + ", 0, 511)]";
    }

    // Source operand `src` (1..3) of an ALU instruction as a vec4 expression (swizzled, abs, negated).
    std::string source( const Alu &a, int src, int &const_slot )
    {
        uint32_t reg = a.reg( src );
        std::string v;
        if ( a.is_temp( src ) )
        {
            v = reg & 0x40 ? fmt( "r[clamp(%u + aL, 0, 63)]", reg & 0x3F ) : fmt( "r[%u]", reg & 0x3F );
            if ( reg & 0x80 ) v = "abs(" + v + ")";
        }
        else
        {
            v = const_ref( a, reg, const_slot++ );
            if ( a.abs_constants() ) v = "abs(" + v + ")";
        }
        uint32_t swz = a.swizzle( src );
        if ( swz ) v = "(" + v + ")" + swz4( swz );
        if ( a.negate( src ) ) v = "(-" + v + ")";
        return v;
    }

    void alu( const Alu &a )
    {
        uint32_t vop = a.vector_op(), sop = a.scalar_op();
        bool vector_used = a.vector_mask() || ( vop >= 20 && vop <= 27 ) || vop == 29;
        // The scalar unit runs its operation (updating the previous-scalar value) even when it writes
        // no register; only retain_prev leaves everything as it was.
        bool scalar_used = sop != 50;
        if ( !vector_used && !scalar_used ) return;
        std::string cond;
        if ( a.predicated() ) cond = a.pred_condition() ? "if (p) " : "if (!p) ";
        line( cond + "{" );
        ++indent;
        int const_slot = 0;
        // All sources are read before either unit writes (the two units issue together).
        std::string s1 = source( a, 1, const_slot ), s2 = source( a, 2, const_slot ), s3;
        int const_slot_s = const_slot;
        s3 = source( a, 3, const_slot_s );
        if ( vector_used )
        {
            // Vertex shaders: no fused multiply-adds (precise), so two shaders computing one position with
            // differently grouped instructions (dp3 with a 1.0 term vs dp2add) round alike and the colour pass
            // meets the depth pre-pass exactly (window glass drew nothing: black or stale noise).
            line( "vec4 va = " + s1 + ", vb = " + s2 + ", vc = " + s3 + ( pixel ? ", vr;" : "; precise vec4 vr;" ) );
            switch ( vop )
            {
            case 0: line( "vr = va + vb;" ); break;
            case 1: line( "vr = va * vb;" ); break;
            case 2: line( "vr = max(va, vb);" ); break;
            case 3: line( "vr = min(va, vb);" ); break;
            case 4: line( "vr = vec4(equal(va, vb));" ); break;
            case 5: line( "vr = vec4(greaterThan(va, vb));" ); break;
            case 6: line( "vr = vec4(greaterThanEqual(va, vb));" ); break;
            case 7: line( "vr = vec4(notEqual(va, vb));" ); break;
            case 8: line( "vr = fract(va);" ); break;
            case 9: line( "vr = trunc(va);" ); break;
            case 10: line( "vr = floor(va);" ); break;
            case 11: line( "vr = va * vb + vc;" ); break;
            // Conditional moves select (boolean mix): a float mix computes vc + (vb - vc) * t, which loses
            // a small selected value's precision (the depth pre-pass picked projection terms this way, its
            // depth came out off and window glass never passed the colour pass's depth test).
            case 12: line( "vr = mix(vc, vb, equal(va, vec4(0.0)));" ); break;
            case 13: line( "vr = mix(vc, vb, greaterThanEqual(va, vec4(0.0)));" ); break;
            case 14: line( "vr = mix(vc, vb, greaterThan(va, vec4(0.0)));" ); break;
            // Dot products as products summed left to right, the same order for every form: shaders that
            // compute one position with dp3 in one pass and dp2add in the next must round alike (dot() lets
            // the driver pick the order; window glass failed the colour pass's depth test and drew black).
            case 15: line( "vr = vec4(((va.x * vb.x + va.y * vb.y) + va.z * vb.z) + va.w * vb.w);" ); break;
            case 16: line( "vr = vec4((va.x * vb.x + va.y * vb.y) + va.z * vb.z);" ); break;
            case 17: line( "vr = vec4((va.x * vb.x + va.y * vb.y) + vc.x);" ); break;
            case 18: line( "vr = xcube(va, vb);" ); break;
            case 19: line( "vr = vec4(max(max(va.x, va.y), max(va.z, va.w)));" ); break;
            case 20: line( "p = va.w == 0.0 && vb.w == 0.0; vr = p ? vec4(0.0) : vec4(va.x + 1.0);" ); break;
            case 21: line( "p = va.w != 0.0 && vb.w == 0.0; vr = p ? vec4(0.0) : vec4(va.x + 1.0);" ); break;
            case 22: line( "p = va.w > 0.0 && vb.w == 0.0; vr = p ? vec4(0.0) : vec4(va.x + 1.0);" ); break;
            case 23: line( "p = va.w >= 0.0 && vb.w == 0.0; vr = p ? vec4(0.0) : vec4(va.x + 1.0);" ); break;
            case 24: line( pixel ? "if (any(equal(va, vb))) discard; vr = vec4(0.0);" : "vr = vec4(0.0);" ); break;
            case 25: line( pixel ? "if (any(greaterThan(va, vb))) discard; vr = vec4(0.0);" : "vr = vec4(0.0);" ); break;
            case 26: line( pixel ? "if (any(greaterThanEqual(va, vb))) discard; vr = vec4(0.0);" : "vr = vec4(0.0);" ); break;
            case 27: line( pixel ? "if (any(notEqual(va, vb))) discard; vr = vec4(0.0);" : "vr = vec4(0.0);" ); break;
            case 28: line( "vr = vec4(1.0, va.y * vb.y, va.z, vb.w);" ); break;
            case 29: line( "a0 = int(clamp(floor(va.w + 0.5), -256.0, 255.0)); vr = max(va, vb);" ); break;
            default: line( "vr = vec4(0.0);" ); break;
            }
            if ( a.vector_clamp() ) line( "vr = clamp(vr, 0.0, 1.0);" );
        }
        if ( scalar_used )
        {
            // The scalar unit reads two components of source 3: A from the swizzle's w slot, B from its x slot.
            uint32_t swz = a.swizzle( 3 );
            char ca = XYZW[( ( swz >> 6 ) + 3 ) & 3], cb = XYZW[swz & 3];
            if ( sop >= 42 && sop <= 47 )
            {
                // *_CONST_0/1: A is the constant operand (src3 register), B a temporary whose number is packed
                // into the opcode's low bit (bit 0), src3's select bit (bit 1) and swizzle bits 2-5.
                uint32_t treg = ( sop & 1 ) | ( a.is_temp( 3 ) ? 2u : 0u ) | ( swz & 0x3C );
                int cs = 0;
                std::string cst = const_ref( a, a.reg( 3 ), cs );
                // src3's negate modifier applies to both operands (they share the source slot).
                const char *neg = a.negate( 3 ) ? "-" : "";
                line( fmt( "float sa = %s%s.%c, sb = %sr[%u].%c; %sfloat sr;", neg, cst.c_str(), ca, neg, treg, cb, pixel ? "" : "precise " ) );
            }
            else  // s3 is already swizzled: its w and x are those slots
                line( fmt( "float sa = (%s).w, sb = (%s).x; %sfloat sr;", s3.c_str(), s3.c_str(), pixel ? "" : "precise " ) );
            switch ( sop )
            {
            case 0: line( "sr = sa + sb;" ); break;
            case 1: line( "sr = sa + ps;" ); break;
            case 2: line( "sr = sa * sb;" ); break;
            case 3: line( "sr = sa * ps;" ); break;
            case 4: line( "sr = (ps == -3.4028235e38 || isinf(ps) || isnan(ps) || sb <= 0.0 || isinf(sb)) ? -3.4028235e38 : sa * ps;" ); break;
            case 5: line( "sr = max(sa, sb);" ); break;
            case 6: line( "sr = min(sa, sb);" ); break;
            case 7: line( "sr = sa == 0.0 ? 1.0 : 0.0;" ); break;
            case 8: line( "sr = sa > 0.0 ? 1.0 : 0.0;" ); break;
            case 9: line( "sr = sa >= 0.0 ? 1.0 : 0.0;" ); break;
            case 10: line( "sr = sa != 0.0 ? 1.0 : 0.0;" ); break;
            case 11: line( "sr = fract(sa);" ); break;
            case 12: line( "sr = trunc(sa);" ); break;
            case 13: line( "sr = floor(sa);" ); break;
            case 14: line( "sr = exp2(sa);" ); break;
            case 15: line( "sr = sa == 0.0 ? -3.4028235e38 : log2(abs(sa));" ); break;
            case 16: line( "sr = log2(sa);" ); break;
            case 17: line( "sr = sa == 0.0 ? 3.4028235e38 : 1.0 / sa;" ); break;
            case 18: line( "sr = sa == 0.0 ? 0.0 : 1.0 / sa;" ); break;
            case 19: line( "sr = 1.0 / sa;" ); break;
            case 20: line( "sr = sa <= 0.0 ? 3.4028235e38 : inversesqrt(sa);" ); break;
            case 21: line( "sr = sa <= 0.0 ? 0.0 : inversesqrt(sa);" ); break;
            case 22: line( "sr = inversesqrt(sa);" ); break;
            case 23: line( "a0 = int(clamp(floor(sa + 0.5), -256.0, 255.0)); sr = sa;" ); break;
            case 24: line( "a0 = int(clamp(floor(sa), -256.0, 255.0)); sr = sa;" ); break;
            case 25: line( "sr = sa - sb;" ); break;
            case 26: line( "sr = sa - ps;" ); break;
            case 27: line( "p = sa == 0.0; sr = p ? 0.0 : 1.0;" ); break;
            case 28: line( "p = sa != 0.0; sr = p ? 0.0 : 1.0;" ); break;
            case 29: line( "p = sa > 0.0; sr = p ? 0.0 : 1.0;" ); break;
            case 30: line( "p = sa >= 0.0; sr = p ? 0.0 : 1.0;" ); break;
            case 31: line( "p = sa == 1.0; sr = p ? 0.0 : (sa == 0.0 ? 1.0 : sa);" ); break;
            case 32: line( "p = sa - 1.0 <= 0.0; sr = p ? 0.0 : sa - 1.0;" ); break;
            case 33: line( "p = false; sr = 3.4028235e38;" ); break;
            case 34: line( "p = sa == 0.0; sr = sa;" ); break;
            case 35: line( pixel ? "if (sa == 0.0) discard; sr = 0.0;" : "sr = 0.0;" ); break;
            case 36: line( pixel ? "if (sa > 0.0) discard; sr = 0.0;" : "sr = 0.0;" ); break;
            case 37: line( pixel ? "if (sa >= 0.0) discard; sr = 0.0;" : "sr = 0.0;" ); break;
            case 38: line( pixel ? "if (sa != 0.0) discard; sr = 0.0;" : "sr = 0.0;" ); break;
            case 39: line( pixel ? "if (sa == 1.0) discard; sr = 0.0;" : "sr = 0.0;" ); break;
            case 40: line( "sr = sqrt(sa);" ); break;
            case 42: case 43: line( "sr = sa * sb;" ); break;
            case 44: case 45: line( "sr = sa + sb;" ); break;
            case 46: case 47: line( "sr = sa - sb;" ); break;
            case 48: line( "sr = sin(sa);" ); break;
            case 49: line( "sr = cos(sa);" ); break;
            default: line( "sr = ps;" ); break;  // retain_prev and unknowns
            }
            if ( a.scalar_clamp() ) line( "sr = clamp(sr, 0.0, 1.0);" );
            line( "ps = sr;" );
        }
        // Writes.
        if ( !pixel && ucode::linked_export( a ) )
        {
            // An interpolator export: D3D links a vertex shader to the pixel shader it draws with by moving
            // its exports to the registers that shader reads and dropping those it doesn't (ucode::
            // vs_specialization): the register and both write masks are specialization constants.
            if ( slot >= exports_declared.size() ) exports_declared.resize( slot + 1 );
            if ( !exports_declared[slot] )
            decls += fmt( "layout(constant_id = %u) const uint VE%u_R = 0u;\nlayout(constant_id = %u) const uint VE%u_V = 0u;\n"
                          "layout(constant_id = %u) const uint VE%u_S = 0u;\n",
                          ucode::kExportSpecBase + slot * 4, slot, ucode::kExportSpecBase + slot * 4 + 1, slot, ucode::kExportSpecBase + slot * 4 + 2, slot );
            exports_declared[slot] = true;
            // A component in both masks takes 1.0 (see below).
            for ( int i = 0; i < 4; ++i )
                line( fmt( "if (((VE%u_V | VE%u_S) & %uu) != 0u) o[VE%u_R].%c = ((VE%u_V & VE%u_S & %uu) != 0u) ? 1.0 : ((VE%u_V & %uu) != 0u) ? %s : %s;", slot, slot,
                           1u << i, slot, XYZW[i], slot, slot, 1u << i, slot, 1u << i, vector_used ? fmt( "vr.%c", XYZW[i] ).c_str() : "0.0",
                           scalar_used ? "sr" : "0.0" ) );
            --indent;
            line( "}" );
            return;
        }
        if ( vector_used && a.vector_mask() ) write( a.export_data(), a.vector_dest(), a.vector_dest_rel(), a.vector_mask(), "vr" );
        // An exporting instruction sends the scalar result to the same export register as the vector
        // result (with its own write mask); scalar_dest only names a temp when nothing is exported.
        // A component in both masks of an export takes the constant 1.0 (the depth downsample before the
        // effects pass, run when shooting and in cars, writes its alpha that way: as a scalar result it came
        // out 0 and blacked out the composited scene).
        uint32_t both = a.export_data() && vector_used ? a.vector_mask() & a.scalar_mask() : 0;
        if ( scalar_used && ( a.scalar_mask() & ~both ) )
            write( a.export_data(), a.export_data() ? a.vector_dest() : a.scalar_dest(), a.export_data() ? a.vector_dest_rel() : a.scalar_dest_rel(), a.scalar_mask() & ~both, "vec4(sr)" );
        if ( both ) write( true, a.vector_dest(), a.vector_dest_rel(), both, "vec4(1.0)" );
        --indent;
        line( "}" );
    }

    void write( bool exp, uint32_t reg, bool rel, uint32_t mask, const char *value )
    {
        std::string dst;
        if ( !exp ) dst = rel ? fmt( "r[clamp(%u + aL, 0, 63)]", reg ) : fmt( "r[%u]", reg );
        else if ( !pixel )
        {
            if ( reg == 62 ) dst = "pos";
            else if ( reg == 63 ) dst = "xpts";
            else if ( reg < 16 ) dst = fmt( "o[%u]", reg );
            else return;  // memory export (eA/eM): not yet
        }
        else
        {
            if ( reg < 4 ) dst = fmt( "oc[%u]", reg );
            else if ( reg == 61 ) dst = "odepth";
            else return;
        }
        if ( dst == "odepth" )  // depth is the export's first component alone
        {
            if ( mask & 1 ) line( dst + " = (" + value + ").x;" );
            return;
        }
        if ( mask == 0xF ) { line( dst + " = " + value + ";" ); return; }
        std::string m;
        for ( int i = 0; i < 4; ++i ) if ( mask & ( 1 << i ) ) m += XYZW[i];
        line( dst + "." + m + " = (" + value + ")." + m + ";" );
    }

    void fetch( const Fetch &f )
    {
        if ( !f.predicated() ) { fetch_body( f ); return; }
        line( std::string( "if (p == " ) + ( f.pred_condition() ? "true" : "false" ) + ") {" );
        ++indent; fetch_body( f ); --indent;
        line( "}" );
    }

    void fetch_body( const Fetch &f )
    {
        uint32_t dsw = f.dst_swizzle();
        std::string res;
        if ( f.op() == 0 && !pixel )
        {
            // A vertex shader's fetch fields are D3D's to patch when it binds the shader to a vertex
            // declaration (ucode::binding_mask): they are specialization constants (ucode::vs_specialization,
            // ids slot * 8 + field), so a shader's source is the same for every binding and as in the game files.
            uint32_t id = slot * 8;
            if ( slot >= declared.size() ) declared.resize( slot + 1 );
            if ( !declared[slot] )
            decls += fmt( "layout(constant_id = %u) const uint VF%u_C = 0u;\nlayout(constant_id = %u) const uint VF%u_F = 0u;\n"
                          "layout(constant_id = %u) const uint VF%u_G = 0u;\nlayout(constant_id = %u) const int VF%u_E = 0;\n"
                          "layout(constant_id = %u) const uint VF%u_S = 0u;\nlayout(constant_id = %u) const int VF%u_O = 0;\n"
                          "layout(constant_id = %u) const uint VF%u_D = 0u;\nlayout(constant_id = %u) const uint VF%u_R = 0u;\n",
                          id, slot, id + 1, slot, id + 2, slot, id + 3, slot, id + 4, slot, id + 5, slot, id + 6, slot, id + 7, slot );
            declared[slot] = true;
            // The destination register too: D3D reorders a declaration's fetches (each keeps its fields).
            line( fmt( "{ vec4 t = vfetch(VF%u_C, r[%u].%c, VF%u_F, VF%u_G, VF%u_E, VF%u_S, VF%u_O); r[VF%u_R] = fetch_swizzle(r[VF%u_R], t, VF%u_D); }",
                       slot, f.src_reg(), XYZW[f.vtx_src_swizzle()], slot, slot, slot, slot, slot, slot, slot, slot ) );
            return;
        }
        if ( f.op() == 0 )
        {
            uint32_t vf = f.const_index() * 3 + f.vtx_const_sel();
            line( fmt( "{ vec4 t = vfetch(%uu, r[%u].%c, %uu, %uu, %d, %uu, %d);", vf, f.src_reg(), XYZW[f.vtx_src_swizzle()],
                       f.vtx_data_format(), ( f.vtx_format_signed() ? 1u : 0u ) | ( f.vtx_num_format_int() ? 2u : 0u ),
                       f.vtx_exp_adjust(), f.vtx_stride(), f.vtx_offset() ) );
        }
        else if ( f.op() == 1 )
        {
            uint32_t sw = f.tex_src_swizzle();
            std::string coord = fmt( "r[%u]", f.src_reg() );
            std::string uv = coord + "." + XYZW[( sw >> 0 ) & 3] + XYZW[( sw >> 2 ) & 3];
            // Level of detail per surface pixel, as the console takes it: an anti-aliased surface has more image
            // pixels than that, and sharper levels would feed materials that blend by texture height hard noise.
            std::string grad = pixel && f.tex_computed_lod() ? fmt( "vec4(dFdx(vec2(%s)), dFdy(vec2(%s))) * dc.window_offset.w", uv.c_str(), uv.c_str() ) : "vec4(0.0)";
            if ( f.tex_dimension() == 3 )  // cube: (s, t, face) as the cube instruction leaves them
            {
                std::string c = coord + "." + XYZW[( sw >> 0 ) & 3] + XYZW[( sw >> 2 ) & 3] + XYZW[( sw >> 4 ) & 3];
                line( fmt( "{ vec4 t = tfetchcube(%uu, vec3(%s), %uu, %.4f, %s, %s, %s);", f.const_index(), c.c_str(), f.tex_filters(), f.tex_lod_bias(),
                           grad.c_str(), pixel && f.tex_computed_lod() ? "true" : "false", f.tex_register_lod() ? "tex_lod" : "-1000.0" ) );
            }
            else if ( f.tex_dimension() == 1 && pixel && f.tex_computed_lod() && !f.tex_register_lod() && !f.tex_unnormalized() && f.tex_lod_bias() == -2.0f )
                // World materials' light maps (the one fetch biased so, single level): see tfetchsmooth.
                line( fmt( "{ vec4 t = tfetchsmooth(%uu, vec2(%s), %uu, %.4f, %s);", f.const_index(), uv.c_str(), f.tex_filters(), f.tex_lod_bias(), grad.c_str() ) );
            else if ( f.tex_dimension() == 1 )  // 2D: the texture as a Vulkan image
                line( fmt( "{ vec4 t = tfetch(%uu, vec2(%s), %s, %uu, %.4f, %s, %s, vec2(%.1f, %.1f), %s);", f.const_index(), uv.c_str(),
                           f.tex_unnormalized() ? "true" : "false", f.tex_filters(), f.tex_lod_bias(), grad.c_str(),
                           pixel && f.tex_computed_lod() ? "true" : "false", f.tex_offset_x(), f.tex_offset_y(), f.tex_register_lod() ? "tex_lod" : "-1000.0" ) );
            else if ( f.tex_dimension() == 2 )  // 3D: stacked 2D textures, by their third coordinate (tfetchstack)
                line( fmt( "{ vec4 t = tfetchstack(%uu, vec2(%s), %s.%c, %s, %uu, %.4f, %s, %s, vec2(%.1f, %.1f), %s);", f.const_index(), uv.c_str(),
                           coord.c_str(), XYZW[( sw >> 4 ) & 3], f.tex_unnormalized() ? "true" : "false", f.tex_filters(), f.tex_lod_bias(), grad.c_str(),
                           pixel && f.tex_computed_lod() ? "true" : "false", f.tex_offset_x(), f.tex_offset_y(), f.tex_register_lod() ? "tex_lod" : "-1000.0" ) );
            else  // others read from guest memory
                line( fmt( "%s{ vec4 t = tfetch2d(%uu, vec2(%s), %s, %uu, %.4f, %s, vec2(%.1f, %.1f), %s);",
                           f.tex_dimension() == 2 ? fmt( "tex_layer = stack_layer(%uu, %s.%c); ", f.const_index(), coord.c_str(), XYZW[( sw >> 4 ) & 3] ).c_str() : "tex_layer = 0u; ", f.const_index(), uv.c_str(),
                           f.tex_unnormalized() ? "true" : "false", f.tex_filters(), f.tex_lod_bias(), grad.c_str(), f.tex_offset_x(), f.tex_offset_y(),
                           f.tex_register_lod() ? "tex_lod" : "-1000.0" ) );
            if ( (int)f.const_index() == debug_fetch ) line( "  dbg_t = t;" );
        }
        else if ( f.op() == 19 )  // GET_WEIGHTS: a shader blending its own samples (shadow comparisons) asks for the fractions
        {
            uint32_t sw = f.tex_src_swizzle();
            line( fmt( "{ vec4 t = tweights(%uu, vec2(r[%u].%c%c), %s, vec2(%.1f, %.1f));", f.const_index(), f.src_reg(), XYZW[( sw >> 0 ) & 3], XYZW[( sw >> 2 ) & 3],
                       f.tex_unnormalized() ? "true" : "false", f.tex_offset_x(), f.tex_offset_y() ) );
        }
        else if ( f.op() == 24 )  // SET_TEX_LOD: the level later fetches with use_register_lod read
        {
            line( fmt( "tex_lod = r[%u].%c;", f.src_reg(), XYZW[f.tex_src_swizzle() & 3] ) );
            return;
        }
        else
        {
            line( "// unsupported fetch op " + std::to_string( f.op() ) );
            return;
        }
        ++indent;
        for ( int i = 0; i < 4; ++i )
        {
            uint32_t s = ( dsw >> ( i * 3 ) ) & 7;
            if ( s == 7 ) continue;
            std::string v = s < 4 ? fmt( "t.%c", XYZW[s] ) : s == 4 ? "0.0" : s == 5 ? "1.0" : "0.0";
            line( fmt( "r[%u].%c = %s;", f.dst_reg(), XYZW[i], v.c_str() ) );
        }
        --indent;
        line( "}" );
    }

    void clause( const Cf &c )
    {
        for ( uint32_t k = 0; k < c.count(); ++k )
        {
            uint32_t slot = c.address() + k;
            this->slot = slot;
            uint32_t w[3] = { prog.slot_word( slot, 0 ), prog.slot_word( slot, 1 ), prog.slot_word( slot, 2 ) };
            if ( ( c.sequence() >> ( k * 2 ) ) & 1 ) fetch( Fetch{ { w[0], w[1], w[2] } } );
            else alu( Alu{ { w[0], w[1], w[2] } } );
        }
    }

    std::string bool_const( uint32_t b ) { return fmt( "((dc.bools[%u] >> %uu) & 1u) != 0u", b >> 5, b & 31 ); }

    // Straight-line programs (no jumps, loops or calls: most of them) are emitted as plain code. The
    // general pc/switch state machine around them made the NVIDIA compiler take minutes or hang on
    // big vertex shaders with texture fetches.
    bool straight_line() const
    {
        for ( const Cf &c : prog.cf )
            switch ( c.op() )
            {
            case CfOp::LoopStart: case CfOp::LoopEnd: case CfOp::CondJmp: case CfOp::CondCall: case CfOp::Return: return false;
            default: if ( c.is_end() ) return true;
            }
        return true;
    }

    void exec( const Cf &c )
    {
        switch ( c.op() )
        {
        case CfOp::Exec: case CfOp::ExecEnd: clause( c ); break;
        case CfOp::CondExec: case CfOp::CondExecEnd: case CfOp::CondExecPredClean: case CfOp::CondExecPredCleanEnd:
            line( "if ((" + bool_const( c.bool_address() ) + ") == " + ( c.condition() ? "true" : "false" ) + ") {" );
            ++indent; clause( c ); --indent;
            line( "}" );
            break;
        case CfOp::CondPredExec: case CfOp::CondPredExecEnd:
            line( std::string( "if (p == " ) + ( c.condition() ? "true" : "false" ) + ") {" );
            ++indent; clause( c ); --indent;
            line( "}" );
            break;
        default: break;
        }
    }

    void body()
    {
        if ( straight_line() )
        {
            for ( const Cf &c : prog.cf )
            {
                exec( c );
                if ( c.is_end() ) break;
            }
            return;
        }
        line( "int pc = 0;" );
        line( "int lp = -1; int lcount[4]; int lstep[4]; int lret[4]; int cret[4]; int csp = -1;" );
        line( "for (int guard = 0; guard < 65536; ++guard) {" );
        ++indent;
        line( "switch (pc) {" );
        for ( size_t i = 0; i < prog.cf.size(); ++i )
        {
            const Cf &c = prog.cf[i];
            line( fmt( "case %zu:", i ) );
            ++indent;
            switch ( c.op() )
            {
            case CfOp::Exec: case CfOp::ExecEnd: case CfOp::CondExec: case CfOp::CondExecEnd: case CfOp::CondExecPredClean:
            case CfOp::CondExecPredCleanEnd: case CfOp::CondPredExec: case CfOp::CondPredExecEnd: exec( c ); break;
            case CfOp::LoopStart:
                // loop constant: count bits 0-7, start bits 8-15, step bits 16-23
                line( fmt( "{ uint lc = dc.loops[%u]; ++lp; lcount[lp] = int(lc & 255u); lstep[lp] = int(int(lc << 8) >> 24); aL = int((lc >> 8) & 255u); lret[lp] = %zu;", c.loop_id(), i + 1 ) );
                line( fmt( "  if (lcount[lp] == 0) { --lp; pc = %u; continue; } }", c.target() ) );
                break;
            case CfOp::LoopEnd:
                line( "if (lp >= 0) { aL += lstep[lp]; if (--lcount[lp] > 0) { pc = lret[lp]; continue; } --lp; }" );
                break;
            case CfOp::CondJmp:
            {
                std::string cond = c.unconditional() ? "true" : c.predicated_jmp() ? std::string( "p == " ) + ( c.condition() ? "true" : "false" )
                                                                                   : "(" + bool_const( c.bool_address() ) + ") == " + ( c.condition() ? "true" : "false" );
                line( "if (" + cond + ") { pc = " + std::to_string( c.target() ) + "; continue; }" );
                break;
            }
            case CfOp::CondCall:
            {
                std::string cond = c.unconditional() ? "true" : "(" + bool_const( c.bool_address() ) + ") == " + ( c.condition() ? "true" : "false" );
                line( fmt( "if (%s && csp < 3) { cret[++csp] = %zu; pc = %u; continue; }", cond.c_str(), i + 1, c.target() ) );
                break;
            }
            case CfOp::Return: line( "if (csp >= 0) { pc = cret[csp--]; continue; }" ); break;
            default: break;  // nop, alloc, vfetch_done
            }
            if ( c.is_end() ) line( "XRET;" );
            --indent;
        }
        line( "default: XRET;" );
        line( "}" );
        line( "XRET;" );
        --indent;
        line( "}" );
    }
};

} // namespace

std::string translate( const Program &p, std::string *error, int debug_fetch )
{
    if ( p.cf.empty() ) { if ( error ) *error = "no control flow"; return ""; }
    Emitter e{ p, p.type == 1 };
    e.debug_fetch = p.type == 1 ? debug_fetch : -1;
    std::string s;
    if ( p.type == 0 )
    {
        s += "#undef XRET\n#define XRET { psize = xpts.x; return; }\n";
        s += "void xvs(uint vertex_index, out vec4 pos, out vec4 o[16], out float psize) {\n";
        s += "  vec4 r[64]; for (int i = 0; i < 64; ++i) r[i] = vec4(0.0);\n";
        s += "  for (int i = 0; i < 16; ++i) o[i] = vec4(0.0);\n";
        s += "  pos = vec4(0.0, 0.0, 0.0, 1.0); vec4 xpts = vec4(1.0); psize = 1.0;\n";
        s += "  r[0].x = float(vertex_index);\n";
    }
    else
    {
        if ( e.debug_fetch >= 100 )  // 100 + n: show interpolator n (x, y wrapped to 0-1 as red, green)
            s += fmt( "#undef XRET\n#define XRET { oc[0] = vec4(fract(interp[%d].xy), 0.0, 1.0); return; }\n", e.debug_fetch - 100 );
        else
        s += e.debug_fetch >= 0 ? "#undef XRET\n#define XRET { oc[0] = vec4(dbg_t.rgb, 1.0); return; }\nvec4 dbg_t = vec4(1.0, 0.0, 1.0, 1.0);\n"
                                : "#undef XRET\n#define XRET return\n";
        s += "void xps(vec4 interp[16], out vec4 oc[4], out float odepth) {\n";
        s += "  vec4 r[64]; for (int i = 0; i < 64; ++i) r[i] = vec4(0.0);\n";
        s += "  for (int i = 0; i < 16; ++i) r[i] = interp[i];\n";
        // Parameter generation (SQ_PROGRAM_CNTL bit 18): the pixel's screen position in the register
        // SQ_CONTEXT_MISC names, which the game's lighting reads its light buffer at.
        s += "  if (dc.param_gen < 64u) r[dc.param_gen] = vec4(gl_FragCoord.xy / dc.window_offset.zz - ((dc.vte & 0x10000u) != 0u ? 0.5 : 0.0), 0.0, 0.0);\n";
        s += "  for (int i = 0; i < 4; ++i) oc[i] = vec4(0.0);\n";
        s += "  odepth = gl_FragCoord.z;\n";
    }
    s += "  bool p = false; float ps = 0.0; int a0 = 0; int aL = 0; float tex_lod = -1000.0;\n";
    e.body();
    // Pixel shaders whose registers are only ever named by number get them as separate variables (r0, r1,
    // ...): an array indexed at run time anywhere (the parameter register, relative addressing) lives in
    // the GPU's slow local memory for every access. The parameter register is then set through a switch.
    if ( e.pixel && e.out.find( "r[clamp(" ) == std::string::npos && e.out.find( "r[VF" ) == std::string::npos )
    {
        std::string out;
        bool used[64] = {};
        for ( size_t i = 0; i < e.out.size(); )
        {
            size_t j = e.out.find( "r[", i );
            bool name_start = j != std::string::npos && ( j == 0 || !( isalnum( (unsigned char)e.out[j - 1] ) || e.out[j - 1] == '_' ) );
            size_t k = j == std::string::npos ? j : j + 2;
            unsigned n = 0;
            bool digits = false;
            while ( name_start && k < e.out.size() && isdigit( (unsigned char)e.out[k] ) ) n = n * 10 + ( e.out[k++] - '0' ), digits = true;
            if ( j == std::string::npos ) { out.append( e.out, i, std::string::npos ); break; }
            if ( !name_start || !digits || k >= e.out.size() || e.out[k] != ']' || n >= 64 ) { out.append( e.out, i, j + 2 - i ); i = j + 2; continue; }
            out.append( e.out, i, j - i );
            out += "r" + std::to_string( n );
            used[n] = true;
            i = k + 1;
        }
        std::string head = "void xps(vec4 interp[16], out vec4 oc[4], out float odepth) {\n";
        for ( int n = 0; n < 64; ++n )
            if ( used[n] ) head += n < 16 ? fmt( "  vec4 r%d = interp[%d];\n", n, n ) : fmt( "  vec4 r%d = vec4(0.0);\n", n );
        head += "  switch (dc.param_gen) {\n";
        for ( int n = 0; n < 64; ++n )
            if ( used[n] ) head += fmt( "  case %du: r%d = vec4(gl_FragCoord.xy / dc.window_offset.zz - ((dc.vte & 0x10000u) != 0u ? 0.5 : 0.0), 0.0, 0.0); break;\n", n, n );
        head += "  default: break;\n  }\n";
        const std::string arr = "  vec4 r[64]; for (int i = 0; i < 64; ++i) r[i] = vec4(0.0);\n  for (int i = 0; i < 16; ++i) r[i] = interp[i];\n";
        const std::string gen = "  if (dc.param_gen < 64u) r[dc.param_gen] = vec4(gl_FragCoord.xy / dc.window_offset.zz - ((dc.vte & 0x10000u) != 0u ? 0.5 : 0.0), 0.0, 0.0);\n";
        size_t at = s.find( "void xps(" );
        size_t a1 = s.find( arr ), g1 = s.find( gen );
        if ( at != std::string::npos && a1 != std::string::npos && g1 != std::string::npos )
        {
            s.erase( g1, gen.size() );
            s.erase( a1, arr.size() );
            s.replace( at, s.find( '\n', at ) + 1 - at, head );
            e.out = out;
        }
    }
    s = e.decls + s;
    s += e.out;
    s += "  XRET;\n}\n";
    if ( e.pixel && e.out.find( "odepth" ) != std::string::npos ) s += "#define XWRITES_DEPTH 1\n";  // oDepth export: kPixelMain passes it on
    if ( e.pixel && e.out.find( "discard" ) != std::string::npos ) s += "#define XDISCARDS 1\n";  // (kPixelMain: no early depth test)
    return s;
}

// The renderer's debug switches that have shaders decode textures from memory (CRACKDOWN_DECODED_*):
// only with them do shaders carry the decoder next to the image fetches.
bool tex_decoded_fallback()
{
    static const bool on = getenv( "CRACKDOWN_DECODED_TEXTURES" ) || getenv( "CRACKDOWN_DECODED_BASE" ) || getenv( "CRACKDOWN_DECODED_PS" ) ||
                           getenv( "CRACKDOWN_DECODED_ODD" );
    return on;
}

std::string module_source( const Program &p, int debug_fetch )
{
    std::string body = translate( p, nullptr, debug_fetch );
    if ( body.empty() ) return "";
    std::string src = std::string( kPrelude ) + body + ( p.type == 1 ? kPixelMain : kVertexMain );
    // CRACKDOWN_TEX_CHECK=<tolerance> (debug): native texture samples compared with the memory decode
    // (prelude.glsl.h tfetch). Here so the shader tools build the same shaders.
    if ( const char *tc = getenv( "CRACKDOWN_TEX_CHECK" ) ) src.insert( src.find( '\n' ) + 1, std::string( "#define TEX_CHECK " ) + tc + "\n" );
    if ( tex_decoded_fallback() ) src.insert( src.find( '\n' ) + 1, "#define TEX_DECODED_FALLBACK 1\n" );
    if ( p.type == 1 ) src.insert( src.find( '\n' ) + 1, "#define XPIXEL 1\n" );
    return src;
}

uint64_t source_hash( const std::string &src, int kind )
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)kind;
    for ( unsigned char ch : src ) h = ( h ^ ch ) * 1099511628211ull;
    return h;
}

} // namespace xlate
