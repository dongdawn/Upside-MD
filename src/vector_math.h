#ifndef VECTOR_MATH_H
#define VECTOR_MATH_H

#include <cmath>
#include <type_traits>
#include <memory>
#include <cstdio>

struct VecArray {
    float* v;
    int component_offset;

    VecArray(float* v_, int component_offset_):
        v(v_), component_offset(component_offset_) {}

    VecArray shifted(int shift_amount) {
        return VecArray(v + shift_amount*component_offset, component_offset);
    }

    float& operator()(int i_comp, int i_elem) {
        return v[i_comp*component_offset + i_elem];
    }

    const float& operator()(int i_comp, int i_elem) const {
        return v[i_comp*component_offset + i_elem];
    }
};


inline void swap(VecArray &a, VecArray &b) {
    { auto tmp=a.v; a.v=b.v; b.v=tmp; }
    { auto tmp=a.component_offset; a.component_offset=b.component_offset; b.component_offset=tmp; }
}


struct SysArray {
    float* v;
    int system_offset;
    int component_offset;

    SysArray(float* v_, int system_offset_, int component_offset_):
        v(v_), system_offset(system_offset_), component_offset(component_offset_) {}
    SysArray():
        v(nullptr), system_offset(0), component_offset(0) {}
    
    VecArray operator[](int ns) {
        return VecArray(v + ns*system_offset, component_offset);
    }

    const VecArray operator[](int ns) const {
        return VecArray(v + ns*system_offset, component_offset);
    }
};


struct SysArrayStorage {
    int n_system;
    int n_dim;
    int n_elem;

    int component_offset;
    int system_offset;
    std::unique_ptr<float[], std::default_delete<float[]>> storage;

    SysArrayStorage(): SysArrayStorage(0,0,0) {}

    SysArrayStorage(int n_system_, int n_dim_, int n_elem_):
        n_system(n_system_), n_dim(n_dim_), n_elem(n_elem_),
        component_offset(n_elem),               // FIXME add padding for SIMD alignment
        system_offset(n_dim*component_offset),  // FIXME add padding for NUMA alignment
        storage(new float[n_system*system_offset])
    {}

    SysArray       array()       {return SysArray(storage.get(), system_offset, component_offset);}
    const SysArray array() const {return SysArray(storage.get(), system_offset, component_offset);}

    void reset(int n_system_, int n_dim_, int n_elem_) {
        n_system = n_system_;
        n_dim = n_dim_;
        n_elem = n_elem_;
        component_offset = n_elem;
        system_offset = n_dim*component_offset;
        storage.reset(new float[n_system*n_dim*n_elem]);
    }

    VecArray       operator[](int ns)       {return array()[ns];}
    const VecArray operator[](int ns) const {return array()[ns];}
};


static void fill(VecArray v, int n_dim, int n_elem, float fill_value) {
    for(int d=0; d<n_dim; ++d) 
        for(int ne=0; ne<n_elem; ++ne) 
            v(d,ne) = fill_value;
}


static void fill(SysArray s, int n_system, int n_dim, int n_elem, float value) {
    #pragma omp parallel for schedule(static,1)
    for(int ns=0; ns<n_system; ++ns) {
        fill(s[ns], n_dim, n_elem, value);
    }
}


struct range {
    int start;
    int stop;
    int stride;

    range(int stop_): start(0), stop(stop_), stride(1) {}
    range(int start_, int stop_): start(start_), stop(stop_), stride(1) {}
    range(int start_, int stop_, int stride_): start(start_), stop(stop_), stride(stride_) {}

    struct iterator {
        int index;
        int stride;

        iterator(int index_, int stride_): index(index_), stride(stride_) {}
        // this operator!= is designed to reprod
        bool operator!=(iterator other) {return index<other.index;}
        int  operator*() {return index;}
        iterator& operator++() {
            index+=stride;
            return *this;
        }
    };

   iterator begin() {return iterator(start, stride);}
   iterator end()   {return iterator(stop,  stride);}
};


template <int ndim, typename ScalarT = float>
struct
// alignas(std::alignment_of<ScalarT>::value)  // GCC 4.8.1 does not like this line
 Vec {
    ScalarT v[ndim];

    ScalarT&       x()       {return           v[0];}
    const ScalarT& x() const {return           v[0];}
    ScalarT&       y()       {return ndim>=1 ? v[1] : v[0];}
    const ScalarT& y() const {return ndim>=1 ? v[1] : v[0];}
    ScalarT&       z()       {return ndim>=2 ? v[2] : v[0];}
    const ScalarT& z() const {return ndim>=2 ? v[2] : v[0];}
    ScalarT&       w()       {return ndim>=3 ? v[3] : v[0];}
    const ScalarT& w() const {return ndim>=3 ? v[3] : v[0];}

    ScalarT& operator[](int i) {return v[i];}
    const ScalarT& operator[](int i) const {return v[i];}
};

typedef Vec<2,float> float2;
typedef Vec<3,float> float3;
typedef Vec<4,float> float4;

template <int D>
inline Vec<D,float> load_vec(const VecArray& a, int idx) {
    Vec<D,float> r;
    #pragma unroll
    for(int d=0; d<D; ++d) r[d] = a(d,idx);
    return r;
}


template <int D>
inline void store_vec(VecArray a, int idx, const Vec<D,float>& r) {
    #pragma unroll
    for(int d=0; d<D; ++d) a(d,idx) = r[d];
}

template <int D>
inline void update_vec(VecArray a, int idx, const Vec<D> &r) {
    store_vec(a,idx, load_vec<D>(a,idx) + r);
}


//! Get component of vector by index

static const float M_PI_F   = 3.141592653589793f;   //!< value of pi as float
static const float M_1_PI_F = 0.3183098861837907f;  //!< value of 1/pi as float

inline float rsqrt(float x) {return 1.f/sqrtf(x);}  //!< reciprocal square root (1/sqrt(x))
inline float sqr  (float x) {return x*x;}  //!< square a number (x^2)
inline float rcp  (float x) {return 1.f/x;}  //!< reciprocal of number
inline float blendv(bool b, float x, float y) {return b ? x : y;}

template <int D, typename S>
inline Vec<D,S> vec_rcp(const Vec<D,S>& x) {
    Vec<D,S> y;
    for(int i=0; i<D; ++i) y[i] = rcp(x[i]);
    return y;
}



// FIXME more general blendv needed
template <int D>
inline Vec<D,float> blendv(bool which, const Vec<D,float>& a, const Vec<D,float>& b) {
    return which ? a : b;
}




inline Vec<1> make_vec1(float x                           ) {Vec<1> a; a[0]=x;                         return a;}
inline float2 make_vec2(float x, float y                  ) {float2 a; a[0]=x; a[1]=y;                 return a;}
inline float3 make_vec3(float x, float y, float z         ) {float3 a; a[0]=x; a[1]=y; a[2]=z;         return a;}
inline float4 make_vec4(float x, float y, float z, float w) {float4 a; a[0]=x; a[1]=y; a[2]=z; a[3]=w; return a;}
//! make float4 from float3 (as x,y,z) and scalar (as w)
inline float4 make_vec4(float3 v, float w) { return make_vec4(v.x(),v.y(),v.z(),w); } 

inline float3 xyz(const float4& x) { return make_vec3(x.x(),x.y(),x.z()); } //!< return x,y,z as float3 from a float4

//! \cond
template <int D, typename S> 
inline Vec<D,S> operator+(const Vec<D,S>& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]+b[i];
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator-(const Vec<D,S>& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]-b[i];
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator*(const Vec<D,S>& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]*b[i];
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator/(const Vec<D,S>& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]/b[i];
    return c;
}


template <int D, typename S> 
inline Vec<D,S> operator+(const S& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a   +b[i];
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator-(const S& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a   -b[i];
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator*(const S& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a   *b[i];
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator/(const S& a, const Vec<D,S>& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a   /b[i];
    return c;
}


template <int D, typename S> 
inline Vec<D,S> operator+(const Vec<D,S>& a, const S& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]+b;
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator-(const Vec<D,S>& a, const S& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]-b;
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator*(const Vec<D,S>& a, const S& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]*b;
    return c;
}
template <int D, typename S> 
inline Vec<D,S> operator/(const Vec<D,S>& a, const S& b) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = a[i]/b;
    return c;
}


template <int D, typename S> 
inline Vec<D,S>& operator+=(Vec<D,S>& a, const Vec<D,S>& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]+=b[i];
    return a;
}
template <int D, typename S> 
inline Vec<D,S>& operator-=(Vec<D,S>& a, const Vec<D,S>& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]-=b[i];
    return a;
}
template <int D, typename S> 
inline Vec<D,S>& operator*=(Vec<D,S>& a, const Vec<D,S>& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]*=b[i];
    return a;
}
template <int D, typename S> 
inline Vec<D,S>& operator/=(Vec<D,S>& a, const Vec<D,S>& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]/=b[i];
    return a;
}


template <int D, typename S> 
inline Vec<D,S>& operator+=(Vec<D,S>& a, const S& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]+=b;
    return a;
}
template <int D, typename S> 
inline Vec<D,S>& operator-=(Vec<D,S>& a, const S& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]-=b;
    return a;
}
template <int D, typename S> 
inline Vec<D,S>& operator*=(Vec<D,S>& a, const S& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]*=b;
    return a;
}
template <int D, typename S> 
inline Vec<D,S>& operator/=(Vec<D,S>& a, const S& b) {
    #pragma unroll
    for(int i=0; i<D; ++i) a[i]/=b;
    return a;
}


template <int D, typename S> 
inline Vec<D,S> operator-(const Vec<D,S>& a) {
    Vec<D,S> c;
    #pragma unroll
    for(int i=0; i<D; ++i) c[i] = -a[i];
    return c;
}
//! \endcond

// FIXME how to handle sqrtf, rsqrtf for simd types?
// I will assume there are functions rsqrt(s) and rcp(s) and sqrt(s) and I will write sqr(s)

template <typename S>
inline S zero() {return 0.f;} // depend on implicit conversion

template <typename S>
inline S one () {return 1.f;} // depend on implicit conversion

template <int D, typename S = float>
inline Vec<D,S> make_zero() {
    Vec<D,S> ret;
    for(int i=0; i<D; ++i) ret[i] = zero<S>();
    return ret;
}

template <int D, typename S = float>
inline Vec<D,S> make_one () {
    Vec<D,S> ret;
    for(int i=0; i<D; ++i) ret[i] = one <S>();
    return ret;
}

template<int start_loc, int stop_loc, int D, typename S>
inline Vec<stop_loc-start_loc,S> extract(const Vec<D,S>& x) {
    static_assert(0<=start_loc, "extract start must be non-negative");
    static_assert(start_loc<=stop_loc, "extract start must be <= extract stop");
    static_assert(stop_loc<=D, "extract stop must be <= dimension");

    Vec<stop_loc-start_loc,S> ret;
    #pragma unroll
    for(int i=0; i<stop_loc-start_loc; ++i) ret[i] = x[i+start_loc];
    return ret;
}

template <typename S>
inline S a_sqrt(const S& a) {
    return a * rsqrt(a);
}

template <int D, typename S>
inline S mag2(const Vec<D,S>& a) {
    S m = zero<S>();
    #pragma unroll
    for(int i=0; i<D; ++i) m += sqr(a[i]);
    return m;
}

template <int ndim, typename S>
inline S inv_mag(const Vec<ndim,S>& a) {
    return rsqrt(mag2(a));
}

template <int ndim, typename S>
inline S inv_mag2(const Vec<ndim,S>& a) {
    return rcp(mag2(a));
}

template <int ndim, typename S>
inline S mag(const Vec<ndim,S>& a) {
    return a_sqrt(mag2(a));
}

template <int D, typename S>
inline S sum(const Vec<D,S>& a) {
    S m = zero<S>();
    #pragma unroll
    for(int i=0; i<D; ++i) m += a[i];
    return m;
}



//! cross-product of the vectors a and b
template <typename S>
inline Vec<3,S> cross(const Vec<3,S>& a, const Vec<3,S>& b){
    Vec<3,S> c;
    c[0] = a.y()*b.z() - a.z()*b.y();
    c[1] = a.z()*b.x() - a.x()*b.z();
    c[2] = a.x()*b.y() - a.y()*b.x();
    return c;
}

template <int D, typename S>
inline Vec<D,S> normalized(const Vec<D,S>& a) { return a*inv_mag(a); }

template <int D, typename S>
inline float dot(const Vec<D,S>& a, const Vec<D,S>& b){
    S c = zero<S>();
    #pragma unroll
    for(int i=0; i<D; ++i) c += a[i]*b[i];
    return c;
}


template <int D, typename S>
Vec<D,S> left_multiply_matrix(Vec<D*D,S> m, Vec<D,S> v) {
    Vec<D,S> mv;
    for(int i=0; i<D; ++i) {
        float x = 0.f;
        for(int j=0; j<D; ++j)
            x += m[i*D+j] * v[j];
        mv[i] = x;
    }
    return mv;
}

template <int D, typename S>
Vec<D,S> right_multiply_matrix(Vec<D,S> v, Vec<D*D,S> m) {
    Vec<D,S> vm;
    for(int j=0; j<D; ++j) {
        float x = 0.f;
        for(int i=0; i<D; ++i)
            x += v[i]*m[i*D+j];
        vm[j] = x;
    }
    return vm;
}

template <int D, typename S>
S min(Vec<D,S> y) {
    S x = y[0];
    for(int i=1; i<D; ++i) x = blendv((y[i]<x), y[i], x);
    return x;
}

template <int D, typename S>
S max(Vec<D,S> y) {
    S x = y[0];
    for(int i=1; i<D; ++i) x = blendv((x<=y[i]), y[i], x);
    return x;
}



// FIXME assume implementation of blendv functions
// I probably need a logical type to make this work right


//! sigmoid function and its derivative 

//! Value of function is 1/(1+exp(x)) and the derivative is 
//! exp(x)/(1+exp(x))^2
inline float2 sigmoid(float x) {
#ifdef APPROX_SIGMOID
    float z = rsqrt(4.f+x*x);
    return make_vec2(0.5f*(1.f + x*z), (2.f*z)*(z*z));
#else
    //return make_vec2(0.5f*(tanh(0.5f*x) + 1.f), 0.5f / (1.f + cosh(x)));
    float z = expf(-x);
    float w = 1.f/(1.f+z);
    return make_vec2(w, z*w*w);
#endif
}


// Sigmoid-like function that has zero derivative outside (-1/sharpness,1/sharpness)
// This function is 1 for large negative values and 0 for large positive values
template <typename S>
inline Vec<2,S> compact_sigmoid(const S& x, const S& sharpness) {
    S y = x*sharpness;
    Vec<2,S> z = make_vec2(S(0.25f)*(y+S(2.f))*(y-S(1.f))*(y-one<S>()), (sharpness*S(0.75f))*(sqr(y)-one<S>()));
    z = blendv((y>S( 1.f)), make_vec2(zero<S>(), zero<S>()), z);
    z = blendv((y<S(-1.f)), make_vec2(one <S>(), zero<S>()), z);
    return z;
}


//! Sigmoid-like function that has zero derivative outside the two intervals (-half_width-1/sharpness,-half_width+1/sharpness)
//! and (half_width-1/sharpness,half_width+1/sharpness).  The function is the product of opposing half-sigmoids.
template <typename S>
inline Vec<2,S> compact_double_sigmoid(const S& x, const S& half_width, const S& sharpness) {
    Vec<2,S> v1 = compact_sigmoid( x-half_width, sharpness);
    Vec<2,S> v2 = compact_sigmoid(-x-half_width, sharpness);
    return make_vec2(v1.x()*v2.x(), v1.y()*v2.x()-v1.x()*v2.y());
}


//! compact_double_sigmoid that also handles periodicity
//! note that both theta and center must be in the range (-PI,PI)
template <typename S>
inline Vec<2,S> angular_compact_double_sigmoid(const S& theta, const S& center, const S& half_width, const S& sharpness) {
    S dev = theta - center;
    dev = blendv((dev <-M_PI_F), dev + S(2.f*M_PI_F), dev);
    dev = blendv((dev > M_PI_F), dev - S(2.f*M_PI_F), dev);
    return compact_double_sigmoid(dev, half_width, sharpness);
}

//! order is value, then dvalue/dphi, dvalue/dpsi
template <typename S>
inline Vec<3,S> rama_box(const Vec<2,S>& rama, const Vec<2,S>& center, const Vec<2,S>& half_width, const S& sharpness) {
    Vec<2,S> phi = angular_compact_double_sigmoid(rama.x(), center.x(), half_width.x(), sharpness);
    Vec<2,S> psi = angular_compact_double_sigmoid(rama.y(), center.y(), half_width.y(), sharpness);
    return make_vec3(phi.x()*psi.x(), phi.y()*psi.x(), phi.x()*psi.y());
}


//! Compute a dihedral angle and its derivative from four positions

//! The angle is always in the range [-pi,pi].  If the arguments are NaN or Inf, the result is undefined.
//! The arguments d1,d2,d3,d4 are output values, and d1 corresponds to the derivative of the dihedral angle
//! with respect to r1.
template <typename S>
static float dihedral_germ(
        Vec<3,S>  r1, Vec<3,S>  r2, Vec<3,S>  r3, Vec<3,S>  r4,
        Vec<3,S> &d1, Vec<3,S> &d2, Vec<3,S> &d3, Vec<3,S> &d4)
    // Formulas and notation taken from Blondel and Karplus, 1995
{
    float3 F = r1-r2;
    float3 G = r2-r3;
    float3 H = r4-r3;

    float3 A = cross(F,G);
    float3 B = cross(H,G);
    float3 C = cross(B,A);

    float inv_Amag2 = inv_mag2(A);
    float inv_Bmag2 = inv_mag2(B);

    float Gmag2    = mag2(G);
    float inv_Gmag = rsqrt(Gmag2);
    float Gmag     = Gmag2 * inv_Gmag;

    d1 = -Gmag * inv_Amag2 * A;
    d4 =  Gmag * inv_Bmag2 * B;

    float3 f_mid =  dot(F,G)*inv_Amag2*inv_Gmag * A - dot(H,G)*inv_Bmag2*inv_Gmag * B;

    d2 = -d1 + f_mid;
    d3 = -d4 - f_mid;

    return atan2f(dot(C,G), dot(A,B) * Gmag);
}


static void print(const VecArray &a, int n_dim, int n_elem, const char* txt) {
    for(int ne: range(n_elem)) {
        printf("%s% 4i  ", txt, ne);
        for(int nd: range(n_dim)) printf(" % .2f", a(nd,ne));
        printf("\n");
    }
}
#endif
