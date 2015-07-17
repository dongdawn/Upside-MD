#include "deriv_engine.h"
#include "timing.h"
#include "affine.h"
#include <vector>
#include "spline.h"
#include "state_logger.h"
#include <initializer_list>

using namespace std;
using namespace h5;

enum class PlaceType {SCALAR, VECTOR, POINT};

struct PlaceParam {
    int layer_idx;
    CoordPair affine_residue;
    CoordPair rama_residue;
};

template <int n_pos_dim> 
struct RigidPlacementNode: public CoordNode {
    vector<PlaceType> signature;

    CoordNode& rama;
    CoordNode& alignment;

    vector<PlaceParam> params;
    LayeredPeriodicSpline2D<n_pos_dim> spline;
    SysArrayStorage rama_deriv;

    RigidPlacementNode(hid_t grp, CoordNode& rama_, CoordNode& alignment_):
        CoordNode(rama_.n_system, get_dset_size(1,grp,"layer_index")[0], n_pos_dim),
        rama(rama_), alignment(alignment_),
        params(n_elem), 
        spline(
                get_dset_size(4, grp, "placement_data")[0],
                get_dset_size(4, grp, "placement_data")[1],
                get_dset_size(4, grp, "placement_data")[2]),

        rama_deriv(n_system, 2*n_pos_dim, n_elem) // first is all phi deriv then all psi deriv
    {
        // verify that the signature is as expected
        int n_pos_dim_input = 0;
        traverse_string_dset<1>(grp, "signature", [&](size_t i, string x){
                if(x == "scalar") {
                    signature.push_back(PlaceType::SCALAR);
                    n_pos_dim_input += 1;
                } else if(x == "vector") {
                    signature.push_back(PlaceType::VECTOR);
                    n_pos_dim_input += 3;
                } else if(x == "point") {
                    signature.push_back(PlaceType::POINT);
                    n_pos_dim_input += 3;
                } else {
                    throw string("unrecognized type in signature");
                }});
        if(n_pos_dim_input != n_pos_dim) 
            throw string("number of dimensions in input signature does not "
                    "match compiled n_pos_dim.  Unable to continue.");

        check_size(grp, "layer_index",    n_elem);
        check_size(grp, "affine_residue", n_elem);
        check_size(grp, "rama_residue",   n_elem);
        check_size(grp, "placement_data", spline.n_layer, spline.nx, spline.ny, n_pos_dim);

        traverse_dset<1,int>(grp, "layer_index",    [&](size_t np, int x){params[np].layer_idx  = x;});
        traverse_dset<1,int>(grp, "affine_residue", [&](size_t np, int x){params[np].affine_residue.index = x;});
        traverse_dset<1,int>(grp, "rama_residue",   [&](size_t np, int x){params[np].rama_residue.index  = x;});

        {
            vector<double> all_data_to_fit;
            traverse_dset<4,double>(grp, "placement_data", [&](size_t nl, size_t ix, size_t iy,size_t d, double x) {
                    all_data_to_fit.push_back(x);});
            spline.fit_spline(all_data_to_fit.data());
        }

        if(logging(LOG_EXTENSIVE)) {
            // FIXME prepend the logging with the class name for disambiguation
            default_logger->add_logger<float>("placement_pos", {n_system, n_elem, n_pos_dim}, [&](float* buffer) {
                    SysArray pos = coords().value;
                    for(int ns: range(n_system))
                        for(int ne: range(n_elem))
                            for(int d: range(n_pos_dim))
                                buffer[ns*n_elem*n_pos_dim + ne*n_pos_dim + d] = pos[ns](d,ne);});
        }

        for(auto &p: params) rama     .slot_machine.add_request(1, p.rama_residue);
        for(auto &p: params) alignment.slot_machine.add_request(1, p.affine_residue);
    }


    virtual void compute_value(ComputeMode mode) {
        Timer timer(string("placement"));

        const float scale_x = spline.nx * (0.5f/M_PI_F - 1e-7f);
        const float scale_y = spline.ny * (0.5f/M_PI_F - 1e-7f);
        const float shift = M_PI_F;

        SysArray pos_s    = coords().value;
        SysArray rama_s   = rama.coords().value;
        SysArray affine_s = alignment.coords().value;

        for(int ns=0; ns<n_system; ++ns) {
            VecArray affine_pos = affine_s [ns];
            VecArray rama_pos   = rama_s   [ns];
            VecArray pos        = pos_s    [ns];
            VecArray phi_d      = rama_deriv[ns];
            VecArray psi_d      = rama_deriv[ns].shifted(n_pos_dim);

            for(int ne: range(n_elem)) {
                auto aff = load_vec<7>(affine_pos, params[ne].affine_residue.index);
                auto r   = load_vec<2>(rama_pos,   params[ne].rama_residue.index);
                auto t   = extract<0,3>(aff);
                float U[9]; quat_to_rot(U, aff.v+3);

                float val[n_pos_dim*3];  // 3 here is deriv_x, deriv_y, value
                spline.evaluate_value_and_deriv(val, params[ne].layer_idx, 
                        (r[0]+shift)*scale_x, (r[1]+shift)*scale_y);

                int j = 0; // index of dimension that we are on

                #define READ3(i,j) make_vec3(val[((i)+0)*3+(j)], val[((i)+1)*3+(j)], val[((i)+2)*3+(j)])
                for(PlaceType type: signature) {
                    switch(type) {
                        case PlaceType::SCALAR:
                            phi_d(j,ne) = val[j*3+0] * scale_x;
                            psi_d(j,ne) = val[j*3+1] * scale_y;
                            pos  (j,ne) = val[j*3+2];
                            j += 1;
                            break;
                        case PlaceType::VECTOR:
                        case PlaceType::POINT:
                            // point and vector differ only in shifting the final output
                            store_vec(phi_d.shifted(j), ne, scale_x * apply_rotation(U, READ3(j,0)));
                            store_vec(psi_d.shifted(j), ne, scale_y * apply_rotation(U, READ3(j,1)));

                            store_vec(pos.shifted(j), ne, (type==PlaceType::POINT
                                     ? apply_affine  (U,t, READ3(j,2))
                                     : apply_rotation(U,   READ3(j,2))));
                            
                            j += 3;
                            break;
                    }
                }
                #undef READ3
            }
        }
    }

    virtual void propagate_deriv() {
        Timer timer(string("placement_deriv"));

        // FIXME need to move the energy scaling back to the rotamer;
        SysArray pos_s = coords().value;

        #pragma omp parallel for
        for(int ns=0; ns<n_system; ++ns) {
           VecArray pos   = pos_s[ns];
           VecArray accum = slot_machine.accum_array()[ns];
           VecArray r_accum = rama.slot_machine.accum_array()[ns];
           VecArray a_accum = alignment.slot_machine.accum_array()[ns];
           VecArray affine_pos = alignment.coords().value[ns];

           vector<Vec<n_pos_dim>> sens(n_elem);
           for(auto &s: sens) s = make_zero<n_pos_dim>();

           for(auto tape_elem: slot_machine.deriv_tape) {
               for(int rec=0; rec<int(tape_elem.output_width); ++rec)
                   sens[tape_elem.atom] += load_vec<n_pos_dim>(accum, tape_elem.loc + rec);
           }

           for(int ne: range(n_elem)) {
               auto d = sens[ne];

               auto rd = make_vec2(
                           dot(d, load_vec<n_pos_dim>(rama_deriv[ns].shifted(0),ne)),
                           dot(d, load_vec<n_pos_dim>(rama_deriv[ns].shifted(n_pos_dim),ne)));

               store_vec(r_accum, params[ne].rama_residue.slot, rd);

               // only difference between points and vectors is whether to subtract off the translation
               auto z = make_zero<6>();
               int j=0;

               auto t  = load_vec<3>(affine_pos, params[ne].affine_residue.index);
               for(PlaceType type: signature) {
                   switch(type) {
                       case PlaceType::SCALAR:
                           j+=1;  // no affine derivative
                           break;
                       case PlaceType::VECTOR:
                       case PlaceType::POINT:
                           auto x  = load_vec<3>(pos.shifted(j), ne);
                           auto dx = make_vec3(d[j+0], d[j+1], d[j+2]);

                           // torque relative to the residue center
                           auto tq = cross((type==PlaceType::POINT?x-t:x), dx);

                           // only points, not vectors, contribute to the CoM derivative
                           if(type==PlaceType::POINT) { z[0] += dx[0]; z[1] += dx[1]; z[2] += dx[2]; }
                           z[3] += tq[0]; z[4] += tq[1]; z[5] += tq[2];
                           j += 3;
                           break;
                   }
               }
               store_vec(a_accum, params[ne].affine_residue.slot, z);
           }
        }    
    }

   virtual double test_value_deriv_agreement() {
       // FIXME I can't test agreement since I don't store derivatives in the standard place
       return -1;
   }
};

static RegisterNodeType<RigidPlacementNode<3>,2> placement3_node("placement3");
static RegisterNodeType<RigidPlacementNode<3>,2> placement_rotamer_node("placement_rotamer");
static RegisterNodeType<RigidPlacementNode<1>,2> placement_scalar_node("placement_scalar");
