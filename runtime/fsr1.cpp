// The constants of AMD's FSR 1 passes (ffx_fsr1.h, MIT licence): its shaders are in renderer.cpp.
#include <cmath>
#include <cstdint>
#define A_CPU 1
#include "ffx_a.h"
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"

void fsr1_easu_constants( uint32_t c[16], float in_w, float in_h, float out_w, float out_h )
{
    FsrEasuCon( c, c + 4, c + 8, c + 12, in_w, in_h, in_w, in_h, out_w, out_h );
}

void fsr1_rcas_constants( uint32_t c[4], float sharpness_stops )
{
    FsrRcasCon( c, sharpness_stops );
}
