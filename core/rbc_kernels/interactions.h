#pragma once

#include <core/utils/cuda_common.h>
#include <core/pvs/object_vector.h>
#include <core/mesh.h>

struct GPU_RBCparameters
{
	float gammaC, gammaT;
	float mpow, lmax, kbToverp;
	float area0, totArea0, totVolume0;
	float cost0kb, sint0kb;
	float ka0, kv0, kd0, kp0;
};

__global__ void computeAreaAndVolume(OVviewWithAreaVolume view, MeshView mesh)
{
	const int objId = blockIdx.y;
	float2 a_v = make_float2(0.0f);

	for(int i = blockIdx.x * blockDim.x + threadIdx.x; i < mesh.ntriangles; i += blockDim.x * gridDim.x)
	{
		int3 ids = mesh.triangles[i];

		float3 v0 = f4tof3( view.particles[ 2 * ids.x+objId*mesh.nvertices ] );
		float3 v1 = f4tof3( view.particles[ 2 * ids.y+objId*mesh.nvertices ] );
		float3 v2 = f4tof3( view.particles[ 2 * ids.z+objId*mesh.nvertices ] );

		a_v.x += 0.5f * length(cross(v1 - v0, v2 - v0));
		a_v.y += 0.1666666667f * (- v0.z*v1.y*v2.x + v0.z*v1.x*v2.y + v0.y*v1.z*v2.x
				- v0.x*v1.z*v2.y - v0.y*v1.x*v2.z + v0.x*v1.y*v2.z);
	}

	a_v = warpReduce( a_v, [] (float a, float b) { return a+b; } );
	if ((threadIdx.x & (warpSize - 1)) == 0)
	{
		atomicAdd(&view.area_volumes[objId], a_v);
	}
}


// **************************************************************************************************
// **************************************************************************************************

__device__ __forceinline__ float fastPower(const float x, const float k)
{
	if (fabsf(k - 2.0f) < 1e-6f) return x*x;
	if (fabsf(k - 1.0f) < 1e-6f) return x;
	if (fabsf(k - 0.5f) < 1e-6f) return sqrtf(fabsf(x));

    return powf(fabsf(x), k);
}


__device__ __forceinline__ float3 _fangle(const float3 v1, const float3 v2, const float3 v3,
		const float totArea, const float totVolume, GPU_RBCparameters parameters)
{
	const float3 x21 = v2 - v1;
	const float3 x32 = v3 - v2;
	const float3 x31 = v3 - v1;

	const float3 normal = cross(x21, x31);

	const float area = 0.5f * length(normal);
	const float area_1 = 1.0f / area;

	const float coefArea = -0.25f * (
			parameters.ka0 * (totArea - parameters.totArea0) * area_1
			- parameters.kd0 * (area - parameters.area0) / (4.0f * area * parameters.area0) );

	const float coeffVol = parameters.kv0 * (totVolume - parameters.totVolume0);
	const float3 fArea = coefArea * cross(normal, x32);
	const float3 fVolume = coeffVol * cross(v3, v2);

	float r = length(v2 - v1);
	r = r < 0.0001f ? 0.0001f : r;
	const float xx = r / parameters.lmax;

	const float IbforceI_wcl = parameters.kbToverp * ( 0.25f / sqr(1.0f-xx) - 0.25f + xx ) / r;

	const float IbforceI_pow = -parameters.kp0 / (fastPower(r, parameters.mpow) * r);

	return fArea + fVolume + (IbforceI_wcl + IbforceI_pow) * x21;
}

__device__ __forceinline__ float3 _fvisc(const float3 v1, const float3 v2, const float3 u1, const float3 u2, GPU_RBCparameters parameters)
{
	const float3 du = u2 - u1;
	const float3 dr = v1 - v2;

	return du*parameters.gammaT + dr * parameters.gammaC*dot(du, dr) / dot(dr, dr);
}

template <int maxDegree>
__device__ float3 bondTriangleForce(
		Particle p, int locId, int rbcId,
		OVviewWithAreaVolume view,
		MeshView mesh,
		GPU_RBCparameters parameters)
{
	const float3 r0 = p.r;
	const float3 u0 = p.u;

	const int startId = maxDegree * locId;
	int idv1 = mesh.adjacent[startId];
	Particle p1(view.particles, rbcId*mesh.nvertices + idv1);
	float3 r1 = p1.r;
	float3 u1 = p1.u;

	float3 f = make_float3(0.0f);

#pragma unroll 2
	for (int i=1; i<=maxDegree; i++)
	{
		int idv2 = mesh.adjacent[startId + (i % maxDegree)];
		if (idv2 == -1) break;

		Particle p2(view.particles, rbcId*mesh.nvertices + idv2);
		float3 r2 = p1.r;
		float3 u2 = p1.u;

		f += _fangle(r0, r1, r2, view.area_volumes[rbcId].x, view.area_volumes[rbcId].y, parameters) +
			 _fvisc (r0, r1, u0, u1, parameters);

		r1 = r2;
		u1 = u2;
	}

	return f;
}

// **************************************************************************************************

template<int update>
__device__  __forceinline__  float3 _fdihedral(float3 v1, float3 v2, float3 v3, float3 v4, GPU_RBCparameters parameters)
{
	const float3 ksi   = cross(v1 - v2, v1 - v3);
	const float3 dzeta = cross(v3 - v4, v2 - v4);

	const float overIksiI   = rsqrtf(dot(ksi, ksi));
	const float overIdzetaI = rsqrtf(dot(dzeta, dzeta));

	const float cosTheta = dot(ksi, dzeta) * overIksiI * overIdzetaI;
	const float IsinThetaI2 = 1.0f - cosTheta*cosTheta;

	const float rawST_1 = rsqrtf(max(IsinThetaI2, 1.0e-6f));
	const float sinTheta_1 = copysignf( rawST_1, dot(ksi - dzeta, v4 - v1) );
	const float beta = parameters.cost0kb - cosTheta * parameters.sint0kb * sinTheta_1;

	float b11 = -beta * cosTheta * overIksiI*overIksiI;
	float b12 =  beta *            overIksiI*overIdzetaI;
	float b22 = -beta * cosTheta * overIdzetaI*overIdzetaI;

	if (update == 1)
		return cross(ksi, v3 - v2)*b11 + cross(dzeta, v3 - v2)*b12;
	else if (update == 2)
		return cross(ksi, v1 - v3)*b11 + ( cross(ksi, v3 - v4) + cross(dzeta, v1 - v3) )*b12 + cross(dzeta, v3 - v4)*b22;
	else return make_float3(0.0f);
}


template <int maxDegree>
__device__ float3 dihedralForce(
		Particle p, int locId, int rbcId,
		OVviewWithAreaVolume view,
		MeshView mesh,
		GPU_RBCparameters parameters)
{
	const int shift = 2*rbcId*mesh.nvertices;
	const float3 r0 = p.r;

	const int startId = maxDegree * locId;
	int idv1 = mesh.adjacent[startId];
	int idv2 = mesh.adjacent[startId+1];

	float3 r1 = Float3_int(view.particles[shift + 2*idv1]).v;
	float3 r2 = Float3_int(view.particles[shift + 2*idv2]).v;

	float3 f = make_float3(0.0f);

	//       v4
	//     /   \
	//   v1 --> v2 --> v3
	//     \   /
	//       V
	//       v0

	// dihedrals: 0124, 0123

#pragma unroll 2
	for (int i=1; i<=maxDegree; i++)
	{
		int idv3 = mesh.adjacent       [startId + ( (i+1) % maxDegree )];
		int idv4 = mesh.adjacent_second[startId + (  i    % maxDegree )];

		if (idv3 == -1 && idv4 == -1) break;

		float3 r3, r4;
		if (idv3 != -1) r3 = Float3_int(view.particles[shift + 2*idv3]).v;
		r4 =				 Float3_int(view.particles[shift + 2*idv4]).v;


		f +=    _fdihedral<1>(r0, r2, r1, r4, parameters);
		if (idv3 != -1)
			f+= _fdihedral<2>(r1, r0, r2, r3, parameters);

		r1 = r2;
		r2 = r3;
	}

	return f;
}

template <int maxDegree>
//__launch_bounds__(128, 12)
__global__ void computeMembraneForces(
		OVviewWithAreaVolume view,
		MeshView mesh,
		GPU_RBCparameters parameters)
{
	const int pid = threadIdx.x + blockDim.x * blockIdx.x;
	const int locId = pid % mesh.nvertices;
	const int rbcId = pid / mesh.nvertices;

	// RBC particles are at the same time mesh vertices
	assert(view.objSize == mesh.nvertices);
	assert(view.particles == mesh.vertices);

	if (pid >= view.nObjects * mesh.nvertices) return;

	Particle p(view.particles, pid);

	float3 f = bondTriangleForce<maxDegree>(p, locId, rbcId, view, mesh, parameters)
			 + dihedralForce    <maxDegree>(p, locId, rbcId, view, mesh, parameters);

	atomicAdd(view.forces + pid, f);
}





