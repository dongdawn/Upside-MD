#include "md_export.h"
#include "coord.h"
#include "affine.h"
#include <cmath>
#include <vector>

using namespace std;

uint32_t
pack_atom(const float x[3]) {
    const int   shift = 1<<9;
    const float scale = 20.f / (1<<10);  // resolution is 0.02 angstroms
    const unsigned int z_mask = (1<<10)-1;

    uint32_t xi[3];
    for(int i=0; i<3; ++i) xi[i] = (int)round(x[i]/scale) + shift;

    // indicate invalid value by returning -1 as unsigned
    // NaN will also give -1u
    bool has_nan = (x[0]+x[1]+x[2])!=(x[0]+x[1]+x[2]);
    if(xi[0]>z_mask || xi[1]>z_mask || xi[2]>z_mask || has_nan) return -1u;
    return xi[0]<<20 | xi[1]<<10 | xi[2]<< 0;
}

namespace {


inline float
nonbonded_kernel_over_r(float r_mag2)
{
    // V(r) = 1/(1+exp(s*(r**2-d**2)))
    // V(d+width) is approximately V(d) + V'(d)*width
    // this equals 1/2 - (1/2)^2 * 2*s*r * width = (1/2) * (1 - s*(r*width))
    // Based on this, to get a characteristic scale of width,
    //   s should be 1/(wall_radius * width)

    // V'(r) = -2*s*r * z/(1+z)^2 where z = exp(s*(r**2-d**2))
    // V'(r)/r = -2*s*z / (1+z)^2

    const float wall = 3.2f;  // corresponds to vdW *diameter*
    const float wall_squared = wall*wall;  
    const float width = 0.15f;
    const float scale_factor = 1.f/(wall*width);  // ensure character

    // overflow protection prevents NaN
    float z = min(exp(scale_factor * (r_mag2-wall_squared)), 1e12f);
    float w = 1.f/(1.f + z);  // include protection from 0

    float deriv_over_r = -2.f*scale_factor * z * (w*w);

    return deriv_over_r;
}

inline void 
unpack_atom(float x[3], unsigned int packed_atom)
{
    // 10 binary digits for each place
    const int   shift = 1<<9;
    const float scale = 20.f / (1<<10);  // resolution is 0.02 angstroms
    const unsigned int   z_mask = (1<<10)-1;

    // unpack reference position with 10 bit precision for each component,
    //   where the range of reference positions for each component is (roughly) [-10.,10.]
    x[0] = scale * ((int)(packed_atom>>20 & z_mask) - shift);
    x[1] = scale * ((int)(packed_atom>>10 & z_mask) - shift);
    x[2] = scale * ((int)(packed_atom>> 0 & z_mask) - shift);
}


// now a float3 variety
inline float3 
unpack_atom(unsigned int packed_atom)
{
    float ret[3]; 
    unpack_atom(ret, packed_atom);
    return make_float3(ret[0], ret[1], ret[2]);
}


template <typename AffineCoordT>
inline void affine_pairs_body(
        AffineCoordT &body1,
        AffineCoordT &body2,
        int n_atoms1, const float3* restrict rpos1,
        int n_atoms2, const float3* restrict rpos2)
{
    for(int i1=0; i1<n_atoms1; ++i1) {
        const float3 x1 = rpos1[i1];

        for(int i2=0; i2<n_atoms2; ++i2) {
            const float3 x2 = rpos2[i2];

            const float3 r = x1-x2;
            const float rmag2 = mag2(r);
            if(rmag2>4.0f*4.0f) continue;
            const float deriv_over_r = nonbonded_kernel_over_r(mag2(r));
            const float3 g = deriv_over_r*r;

            body1.add_deriv_at_location(x1,  g);
            body2.add_deriv_at_location(x2, -g);
        }
    }
}

}

void affine_pairs(
        const float* restrict rigid_body,
        float*       restrict rigid_body_deriv,
        const PackedRefPos* restrict ref_pos,
        const AffineParams* restrict params,
        float energy_scale,
        float dist_cutoff,
        int n_res)
{
    float dist_cutoff2 = dist_cutoff*dist_cutoff;
    vector<AffineCoord> coords;
    coords.reserve(n_res);
    for(int nr=0; nr<n_res; ++nr) 
        coords.emplace_back(rigid_body, rigid_body_deriv, params[nr].residue);

    vector<int>    ref_pos_atoms (n_res);
    vector<float3> ref_pos_coords(n_res*4);

    for(int nr=0; nr<n_res; ++nr) {
        ref_pos_atoms[nr] = ref_pos[nr].n_atoms;
        for(int na=0; na<4; ++na) ref_pos_coords[nr*4+na] = coords[nr].apply(unpack_atom(ref_pos[nr].pos[na]));
    }

    for(int nr1=0; nr1<n_res; ++nr1) {
        for(int nr2=nr1+2; nr2<n_res; ++nr2) {  // do not interact with nearest neighbors
            if(mag2(coords[nr1].tf3()-coords[nr2].tf3()) < dist_cutoff2) {
                affine_pairs_body(
                        coords[nr1],        coords[nr2], 
                        ref_pos_atoms[nr1], &ref_pos_coords[nr1*4],
                        ref_pos_atoms[nr2], &ref_pos_coords[nr2*4]);
            }
        }
    }

    for(int nr=0; nr<n_res; ++nr) {
        for(int d=0; d<6; ++d) coords[nr].d[d] *= energy_scale;
        coords[nr].flush();
    }
}