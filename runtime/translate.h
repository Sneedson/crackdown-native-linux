// Xenos shader microcode -> GLSL (Vulkan). Fetches read guest memory from a storage buffer, so
// vertex and texture data never need copying; per-draw constants come from a storage buffer too.
#pragma once
#include "ucode.h"

#include <string>

namespace xlate
{

// Bindings shared by every translated shader and the renderer.
//   set 0 binding 0: guest physical memory (uint words)  -- `mem`
//   set 0 binding 1: draw constants (see DrawConstants in renderer.h) -- `dc`
// Vertex outputs / pixel inputs: location 0-15 = interpolators o0-o15.
// debug_fetch >= 0 (pixel shaders): output that texture fetch constant's result instead of the colour.
// debug_fetch >= 100: output interpolator (debug_fetch - 100) instead.
std::string translate( const ucode::Program &p, std::string *error = nullptr, int debug_fetch = -1 );

// A shader module's whole GLSL source as the renderer compiles it (prelude, translated body, stage main);
// "" if the program doesn't translate. Shared with devtools/shader_aot, which compiles the game files'
// shaders ahead of time into the same cache.
std::string module_source( const ucode::Program &p, int debug_fetch = -1 );
bool tex_decoded_fallback();  // shaders decode textures where the renderer asks (debug)
// The name of a source's compiled SPIR-V in the cache (~/.cache/crackdown-recomp/spirv/<name>.spv):
// a hash of the source and the shaderc stage kind.
uint64_t source_hash( const std::string &src, int kind );

} // namespace xlate
