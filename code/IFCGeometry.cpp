/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2010, assimp team
All rights reserved.

Redistribution and use of this software in source and binary forms, 
with or without modification, are permitted provided that the 
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file  IFCGeometry.cpp
 *  @brief Geometry conversion and synthesis for IFC
 */

#include "AssimpPCH.h"

#ifndef ASSIMP_BUILD_NO_IFC_IMPORTER
#include "IFCUtil.h"
#include "PolyTools.h"
#include "ProcessHelper.h"

#include "../contrib/poly2tri/poly2tri/poly2tri.h"
#include "../contrib/clipper/clipper.hpp"

#include <iterator>

namespace Assimp {
	namespace IFC {

		using ClipperLib::ulong64;
		// XXX use full -+ range ...
		const ClipperLib::long64 max_ulong64 = 1518500249; // clipper.cpp / hiRange var

		//#define to_int64(p)  (static_cast<ulong64>( std::max( 0., std::min( static_cast<IfcFloat>((p)), 1.) ) * max_ulong64 ))
#define to_int64(p)  (static_cast<ulong64>(static_cast<IfcFloat>((p) ) * max_ulong64 ))
#define from_int64(p) (static_cast<IfcFloat>((p)) / max_ulong64)
#define one_vec (IfcVector2(static_cast<IfcFloat>(1.0),static_cast<IfcFloat>(1.0)))


		bool GenerateOpenings(std::vector<TempOpening>& openings,
			const std::vector<IfcVector3>& nors, 
			TempMesh& curmesh,
			bool check_intersection = true,
			bool generate_connection_geometry = true);


// ------------------------------------------------------------------------------------------------
bool ProcessPolyloop(const IfcPolyLoop& loop, TempMesh& meshout, ConversionData& /*conv*/)
{
	size_t cnt = 0;
	BOOST_FOREACH(const IfcCartesianPoint& c, loop.Polygon) {
		IfcVector3 tmp;
		ConvertCartesianPoint(tmp,c);

		meshout.verts.push_back(tmp);
		++cnt;
	}

	meshout.vertcnt.push_back(cnt);

	// zero- or one- vertex polyloops simply ignored
	if (meshout.vertcnt.back() > 1) { 
		return true;
	}
	
	if (meshout.vertcnt.back()==1) {
		meshout.vertcnt.pop_back();
		meshout.verts.pop_back();
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
void ProcessPolygonBoundaries(TempMesh& result, const TempMesh& inmesh, size_t master_bounds = (size_t)-1) 
{
	// handle all trivial cases
	if(inmesh.vertcnt.empty()) {
		return;
	}
	if(inmesh.vertcnt.size() == 1) {
		result.Append(inmesh);
		return;
	}

	ai_assert(std::count(inmesh.vertcnt.begin(), inmesh.vertcnt.end(), 0) == 0);

	typedef std::vector<unsigned int>::const_iterator face_iter;

	face_iter begin = inmesh.vertcnt.begin(), end = inmesh.vertcnt.end(), iit;
	std::vector<unsigned int>::const_iterator outer_polygon_it = end;

	// major task here: given a list of nested polygon boundaries (one of which
	// is the outer contour), reduce the triangulation task arising here to
	// one that can be solved using the "quadrulation" algorithm which we use
	// for pouring windows out of walls. The algorithm does not handle all
	// cases but at least it is numerically stable and gives "nice" triangles.

	// first compute normals for all polygons using Newell's algorithm
	// do not normalize 'normals', we need the original length for computing the polygon area
	std::vector<IfcVector3> normals;
	inmesh.ComputePolygonNormals(normals,false);

	// One of the polygons might be a IfcFaceOuterBound (in which case `master_bounds` 
	// is its index). Sadly we can't rely on it, the docs say 'At most one of the bounds 
	// shall be of the type IfcFaceOuterBound' 
	IfcFloat area_outer_polygon = 1e-10f;
	if (master_bounds != (size_t)-1) {
		ai_assert(master_bounds < inmesh.vertcnt.size());
		outer_polygon_it = begin + master_bounds;
	}
	else {
		for(iit = begin; iit != end; iit++) {
			// find the polygon with the largest area and take it as the outer bound. 
			IfcVector3& n = normals[std::distance(begin,iit)];
			const IfcFloat area = n.SquareLength();
			if (area > area_outer_polygon) {
				area_outer_polygon = area;
				outer_polygon_it = iit;
			}
		}
	}

	ai_assert(outer_polygon_it != end);

	const size_t outer_polygon_size = *outer_polygon_it;
	const IfcVector3& master_normal = normals[std::distance(begin, outer_polygon_it)];
	const IfcVector3& master_normal_norm = IfcVector3(master_normal).Normalize();


	// Generate fake openings to meet the interface for the quadrulate
	// algorithm. It boils down to generating small boxes given the
	// inner polygon and the surface normal of the outer contour.
	// It is important that we use the outer contour's normal because
	// this is the plane onto which the quadrulate algorithm will
	// project the entire mesh.
	std::vector<TempOpening> fake_openings;
	fake_openings.reserve(inmesh.vertcnt.size()-1);

	std::vector<IfcVector3>::const_iterator vit = inmesh.verts.begin(), outer_vit;

	for(iit = begin; iit != end; vit += *iit++) {
		if (iit == outer_polygon_it) {
			outer_vit = vit;
			continue;
		}

		// Filter degenerate polygons to keep them from causing trouble later on
		IfcVector3& n = normals[std::distance(begin,iit)];
		const IfcFloat area = n.SquareLength();
		if (area < 1e-5f) {
			IFCImporter::LogWarn("skipping degenerate polygon (ProcessPolygonBoundaries)");
			continue;
		}

		fake_openings.push_back(TempOpening());
		TempOpening& opening = fake_openings.back();

		opening.extrusionDir = master_normal;
		opening.solid = NULL;

		opening.profileMesh = boost::make_shared<TempMesh>();
		opening.profileMesh->verts.reserve(*iit);
		opening.profileMesh->vertcnt.push_back(*iit);

		std::copy(vit, vit + *iit, std::back_inserter(opening.profileMesh->verts)); 
	}

	// fill a mesh with ONLY the main polygon 
	TempMesh temp;
	temp.verts.reserve(outer_polygon_size);
	temp.vertcnt.push_back(outer_polygon_size);
	std::copy(outer_vit, outer_vit+outer_polygon_size,
		std::back_inserter(temp.verts));

	GenerateOpenings(fake_openings, normals, temp, false, false);
	result.Append(temp);
}

// ------------------------------------------------------------------------------------------------
void ProcessConnectedFaceSet(const IfcConnectedFaceSet& fset, TempMesh& result, ConversionData& conv)
{
	BOOST_FOREACH(const IfcFace& face, fset.CfsFaces) {
		// size_t ob = -1, cnt = 0;
		TempMesh meshout;
		BOOST_FOREACH(const IfcFaceBound& bound, face.Bounds) {
			
			if(const IfcPolyLoop* const polyloop = bound.Bound->ToPtr<IfcPolyLoop>()) {
				if(ProcessPolyloop(*polyloop, meshout,conv)) {

					// The outer boundary is better determined by checking which
					// polygon covers the largest area.

					//if(bound.ToPtr<IfcFaceOuterBound>()) {
					//	ob = cnt;
					//}
					//++cnt;

				}
			}
			else {
				IFCImporter::LogWarn("skipping unknown IfcFaceBound entity, type is " + bound.Bound->GetClassName());
				continue;
			}

			// And this, even though it is sometimes TRUE and sometimes FALSE,
			// does not really improve results.

			/*if(!IsTrue(bound.Orientation)) {
				size_t c = 0;
				BOOST_FOREACH(unsigned int& c, meshout.vertcnt) {
					std::reverse(result.verts.begin() + cnt,result.verts.begin() + cnt + c);
					cnt += c;
				}
			}*/
		}
		ProcessPolygonBoundaries(result, meshout);
	}
}

// ------------------------------------------------------------------------------------------------
void ProcessRevolvedAreaSolid(const IfcRevolvedAreaSolid& solid, TempMesh& result, ConversionData& conv)
{
	TempMesh meshout;

	// first read the profile description
	if(!ProcessProfile(*solid.SweptArea,meshout,conv) || meshout.verts.size()<=1) {
		return;
	}

	IfcVector3 axis, pos;
	ConvertAxisPlacement(axis,pos,solid.Axis);

	IfcMatrix4 tb0,tb1;
	IfcMatrix4::Translation(pos,tb0);
	IfcMatrix4::Translation(-pos,tb1);

	const std::vector<IfcVector3>& in = meshout.verts;
	const size_t size=in.size();
	
	bool has_area = solid.SweptArea->ProfileType == "AREA" && size>2;
	const IfcFloat max_angle = solid.Angle*conv.angle_scale;
	if(fabs(max_angle) < 1e-3) {
		if(has_area) {
			result = meshout;
		}
		return;
	}

	const unsigned int cnt_segments = std::max(2u,static_cast<unsigned int>(16 * fabs(max_angle)/AI_MATH_HALF_PI_F));
	const IfcFloat delta = max_angle/cnt_segments;

	has_area = has_area && fabs(max_angle) < AI_MATH_TWO_PI_F*0.99;
	
	result.verts.reserve(size*((cnt_segments+1)*4+(has_area?2:0)));
	result.vertcnt.reserve(size*cnt_segments+2);

	IfcMatrix4 rot;
	rot = tb0 * IfcMatrix4::Rotation(delta,axis,rot) * tb1;

	size_t base = 0;
	std::vector<IfcVector3>& out = result.verts;

	// dummy data to simplify later processing
	for(size_t i = 0; i < size; ++i) {
		out.insert(out.end(),4,in[i]);
	}

	for(unsigned int seg = 0; seg < cnt_segments; ++seg) {
		for(size_t i = 0; i < size; ++i) {
			const size_t next = (i+1)%size;

			result.vertcnt.push_back(4);
			const IfcVector3& base_0 = out[base+i*4+3],base_1 = out[base+next*4+3];

			out.push_back(base_0);
			out.push_back(base_1);
			out.push_back(rot*base_1);
			out.push_back(rot*base_0);
		}
		base += size*4;
	}

	out.erase(out.begin(),out.begin()+size*4);

	if(has_area) {
		// leave the triangulation of the profile area to the ear cutting 
		// implementation in aiProcess_Triangulate - for now we just
		// feed in two huge polygons.
		base -= size*8;
		for(size_t i = size; i--; ) {
			out.push_back(out[base+i*4+3]);
		}
		for(size_t i = 0; i < size; ++i ) {
			out.push_back(out[i*4]);
		}
		result.vertcnt.push_back(size);
		result.vertcnt.push_back(size);
	}

	IfcMatrix4 trafo;
	ConvertAxisPlacement(trafo, solid.Position);
	
	result.Transform(trafo);
	IFCImporter::LogDebug("generate mesh procedurally by radial extrusion (IfcRevolvedAreaSolid)");
}



// ------------------------------------------------------------------------------------------------
void ProcessSweptDiskSolid(const IfcSweptDiskSolid solid, TempMesh& result, ConversionData& conv)
{
	const Curve* const curve = Curve::Convert(*solid.Directrix, conv);
	if(!curve) {
		IFCImporter::LogError("failed to convert Directrix curve (IfcSweptDiskSolid)");
		return;
	}

	const std::vector<IfcVector3>& in = result.verts;
	const size_t size=in.size();

	const unsigned int cnt_segments = 16;
	const IfcFloat deltaAngle = AI_MATH_TWO_PI/cnt_segments;

	const size_t samples = curve->EstimateSampleCount(solid.StartParam,solid.EndParam);

	result.verts.reserve(cnt_segments * samples * 4);
	result.vertcnt.reserve((cnt_segments - 1) * samples);

	std::vector<IfcVector3> points;
	points.reserve(cnt_segments * samples);

	TempMesh temp;
	curve->SampleDiscrete(temp,solid.StartParam,solid.EndParam);
	const std::vector<IfcVector3>& curve_points = temp.verts;

	if(curve_points.empty()) {
		IFCImporter::LogWarn("curve evaluation yielded no points (IfcSweptDiskSolid)");
		return;
	}

	IfcVector3 current = curve_points[0];
	IfcVector3 previous = current;
	IfcVector3 next;

	IfcVector3 startvec;
	startvec.x = 1.0f;
	startvec.y = 1.0f;
	startvec.z = 1.0f;

	unsigned int last_dir = 0;

	// generate circles at the sweep positions
	for(size_t i = 0; i < samples; ++i) {

		if(i != samples - 1) {
			next = curve_points[i + 1];
		}

		// get a direction vector reflecting the approximate curvature (i.e. tangent)
		IfcVector3 d = (current-previous) + (next-previous);
	
		d.Normalize();

		// figure out an arbitrary point q so that (p-q) * d = 0,
		// try to maximize ||(p-q)|| * ||(p_last-q_last)|| 
		IfcVector3 q;
		bool take_any = false;

		for (unsigned int i = 0; i < 2; ++i, take_any = true) {
			if ((last_dir == 0 || take_any) && abs(d.x) > 1e-6) {
				q.y = startvec.y;
				q.z = startvec.z;
				q.x = -(d.y * q.y + d.z * q.z) / d.x;
				last_dir = 0;
				break;
			}
			else if ((last_dir == 1 || take_any) && abs(d.y) > 1e-6) {
				q.x = startvec.x;
				q.z = startvec.z;
				q.y = -(d.x * q.x + d.z * q.z) / d.y;
				last_dir = 1;
				break;
			}
			else if ((last_dir == 2 && abs(d.z) > 1e-6) || take_any) { 
				q.y = startvec.y;
				q.x = startvec.x;
				q.z = -(d.y * q.y + d.x * q.x) / d.z;
				last_dir = 2;
				break;
			}
		}

		q *= solid.Radius / q.Length();
		startvec = q;

		// generate a rotation matrix to rotate q around d
		IfcMatrix4 rot;
		IfcMatrix4::Rotation(deltaAngle,d,rot);

		for (unsigned int seg = 0; seg < cnt_segments; ++seg, q *= rot ) {
			points.push_back(q + current);	
		}

		previous = current;
		current = next;
	}

	// make quads
	for(size_t i = 0; i < samples - 1; ++i) {

		const aiVector3D& this_start = points[ i * cnt_segments ];

		// locate corresponding point on next sample ring
		unsigned int best_pair_offset = 0;
		float best_distance_squared = 1e10f;
		for (unsigned int seg = 0; seg < cnt_segments; ++seg) {
			const aiVector3D& p = points[ (i+1) * cnt_segments + seg];
			const float l = (p-this_start).SquareLength();

			if(l < best_distance_squared) {
				best_pair_offset = seg;
				best_distance_squared = l;
			}
		}

		for (unsigned int seg = 0; seg < cnt_segments; ++seg) {

			result.verts.push_back(points[ i * cnt_segments + (seg % cnt_segments)]);
			result.verts.push_back(points[ i * cnt_segments + (seg + 1) % cnt_segments]);
			result.verts.push_back(points[ (i+1) * cnt_segments + ((seg + 1 + best_pair_offset) % cnt_segments)]);
			result.verts.push_back(points[ (i+1) * cnt_segments + ((seg + best_pair_offset) % cnt_segments)]);

			IfcVector3& v1 = *(result.verts.end()-1);
			IfcVector3& v2 = *(result.verts.end()-2);
			IfcVector3& v3 = *(result.verts.end()-3);
			IfcVector3& v4 = *(result.verts.end()-4);

			if (((v4-v3) ^ (v4-v1)) * (v4 - curve_points[i]) < 0.0f) {			
				std::swap(v4, v1);
				std::swap(v3, v2);
			} 

			result.vertcnt.push_back(4);
		}
	}

	IFCImporter::LogDebug("generate mesh procedurally by sweeping a disk along a curve (IfcSweptDiskSolid)");
}

// ------------------------------------------------------------------------------------------------
IfcMatrix3 DerivePlaneCoordinateSpace(const TempMesh& curmesh, bool& ok, IfcFloat* d = NULL) 
{
	const std::vector<IfcVector3>& out = curmesh.verts;
	IfcMatrix3 m;

	ok = true;

	const size_t s = out.size();
	assert(curmesh.vertcnt.size() == 1 && curmesh.vertcnt.back() == s);

	const IfcVector3 any_point = out[s-1];
	IfcVector3 nor; 

	// The input polygon is arbitrarily shaped, therefore we might need some tries
	// until we find a suitable normal. Note that Newells algorithm would give
	// a more robust result, but this variant also gives us a suitable first
	// axis for the 2D coordinate space on the polygon plane, exploiting the
	// fact that the input polygon is nearly always a quad.
	bool done = false;
	size_t base = s-curmesh.vertcnt.back(), i, j;
	for (i = base; !done && i < s-1; !done && ++i) {
		for (j = i+1; j < s; ++j) {
			nor = -((out[i]-any_point)^(out[j]-any_point));
			if(fabs(nor.Length()) > 1e-8f) {
				done = true;
				break;
			}
		}
	}

	if(!done) {
		ok = false;
		return m;
	}

	nor.Normalize();

	IfcVector3 r = (out[i]-any_point);
	r.Normalize();

	if(d) {
		*d = -any_point * nor;
	}

	// Reconstruct orthonormal basis
	// XXX use Gram Schmidt for increased robustness
	IfcVector3 u = r ^ nor;
	u.Normalize();

	m.a1 = r.x;
	m.a2 = r.y;
	m.a3 = r.z;

	m.b1 = u.x;
	m.b2 = u.y;
	m.b3 = u.z;

	m.c1 = nor.x;
	m.c2 = nor.y;
	m.c3 = nor.z;

	return m;
}

// ------------------------------------------------------------------------------------------------
bool TryAddOpenings_Poly2Tri(const std::vector<TempOpening>& openings,const std::vector<IfcVector3>& nors, 
	TempMesh& curmesh)
{
	IFCImporter::LogWarn("forced to use poly2tri fallback method to generate wall openings");
	std::vector<IfcVector3>& out = curmesh.verts;

	bool result = false;

	// Try to derive a solid base plane within the current surface for use as 
	// working coordinate system. 
	bool ok;
	const IfcMatrix3& m = DerivePlaneCoordinateSpace(curmesh, ok);
	if (!ok) {
		return false;
	}

	const IfcMatrix3 minv = IfcMatrix3(m).Inverse();
	const IfcVector3& nor = IfcVector3(m.c1, m.c2, m.c3);

	IfcFloat coord = -1;

	std::vector<IfcVector2> contour_flat;
	contour_flat.reserve(out.size());

	IfcVector2 vmin, vmax;
	MinMaxChooser<IfcVector2>()(vmin, vmax);
	
	// Move all points into the new coordinate system, collecting min/max verts on the way
	BOOST_FOREACH(IfcVector3& x, out) {
		const IfcVector3 vv = m * x;

		// keep Z offset in the plane coordinate system. Ignoring precision issues
		// (which  are present, of course), this should be the same value for
		// all polygon vertices (assuming the polygon is planar).


		// XXX this should be guarded, but we somehow need to pick a suitable
		// epsilon
		// if(coord != -1.0f) {
		//	assert(fabs(coord - vv.z) < 1e-3f);
		// }

		coord = vv.z;

		vmin = std::min(IfcVector2(vv.x, vv.y), vmin);
		vmax = std::max(IfcVector2(vv.x, vv.y), vmax);

		contour_flat.push_back(IfcVector2(vv.x,vv.y));
	}
		
	// With the current code in DerivePlaneCoordinateSpace, 
	// vmin,vmax should always be the 0...1 rectangle (+- numeric inaccuracies) 
	// but here we won't rely on this.

	vmax -= vmin;

	// If this happens then the projection must have been wrong.
	assert(vmax.Length());

	ClipperLib::ExPolygons clipped;
	ClipperLib::Polygons holes_union;


	IfcVector3 wall_extrusion;
	bool do_connections = false, first = true;

	try {

		ClipperLib::Clipper clipper_holes;
		size_t c = 0;

		BOOST_FOREACH(const TempOpening& t,openings) {
			const IfcVector3& outernor = nors[c++];
			const IfcFloat dot = nor * outernor;
			if (fabs(dot)<1.f-1e-6f) {
				continue;
			}

			const std::vector<IfcVector3>& va = t.profileMesh->verts;
			if(va.size() <= 2) {
				continue;	
			}
		
			std::vector<IfcVector2> contour;

			BOOST_FOREACH(const IfcVector3& xx, t.profileMesh->verts) {
				IfcVector3 vv = m *  xx, vv_extr = m * (xx + t.extrusionDir);
				
				const bool is_extruded_side = fabs(vv.z - coord) > fabs(vv_extr.z - coord);
				if (first) {
					first = false;
					if (dot > 0.f) {
						do_connections = true;
						wall_extrusion = t.extrusionDir;
						if (is_extruded_side) {
							wall_extrusion = - wall_extrusion;
						}
					}
				}

				// XXX should not be necessary - but it is. Why? For precision reasons?
				vv = is_extruded_side ? vv_extr : vv;
				contour.push_back(IfcVector2(vv.x,vv.y));
			}

			ClipperLib::Polygon hole;
			BOOST_FOREACH(IfcVector2& pip, contour) {
				pip.x  = (pip.x - vmin.x) / vmax.x;
				pip.y  = (pip.y - vmin.y) / vmax.y;

				hole.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
			}

			if (!ClipperLib::Orientation(hole)) {
				std::reverse(hole.begin(), hole.end());
			//	assert(ClipperLib::Orientation(hole));
			}

			/*ClipperLib::Polygons pol_temp(1), pol_temp2(1);
			pol_temp[0] = hole;

			ClipperLib::OffsetPolygons(pol_temp,pol_temp2,5.0);
			hole = pol_temp2[0];*/

			clipper_holes.AddPolygon(hole,ClipperLib::ptSubject);
		}

		clipper_holes.Execute(ClipperLib::ctUnion,holes_union,
			ClipperLib::pftNonZero,
			ClipperLib::pftNonZero);

		if (holes_union.empty()) {
			return false;
		}

		// Now that we have the big union of all holes, subtract it from the outer contour
		// to obtain the final polygon to feed into the triangulator.
		{
			ClipperLib::Polygon poly;
			BOOST_FOREACH(IfcVector2& pip, contour_flat) {
				pip.x  = (pip.x - vmin.x) / vmax.x;
				pip.y  = (pip.y - vmin.y) / vmax.y;

				poly.push_back(ClipperLib::IntPoint( to_int64(pip.x), to_int64(pip.y) ));
			}

			if (ClipperLib::Orientation(poly)) {
				std::reverse(poly.begin(), poly.end());
			}
			clipper_holes.Clear();
			clipper_holes.AddPolygon(poly,ClipperLib::ptSubject);

			clipper_holes.AddPolygons(holes_union,ClipperLib::ptClip);
			clipper_holes.Execute(ClipperLib::ctDifference,clipped,
				ClipperLib::pftNonZero,
				ClipperLib::pftNonZero);
		}

	}
	catch (const char* sx) {
		IFCImporter::LogError("Ifc: error during polygon clipping, skipping openings for this face: (Clipper: " 
			+ std::string(sx) + ")");

		return false;
	}

	std::vector<IfcVector3> old_verts;
	std::vector<unsigned int> old_vertcnt;

	old_verts.swap(curmesh.verts);
	old_vertcnt.swap(curmesh.vertcnt);


	// add connection geometry to close the adjacent 'holes' for the openings
	// this should only be done from one side of the wall or the polygons 
	// would be emitted twice.
	if (false && do_connections) {

		std::vector<IfcVector3> tmpvec;
		BOOST_FOREACH(ClipperLib::Polygon& opening, holes_union) {

			assert(ClipperLib::Orientation(opening));

			tmpvec.clear();

			BOOST_FOREACH(ClipperLib::IntPoint& point, opening) {

				tmpvec.push_back( minv * IfcVector3(
					vmin.x + from_int64(point.X) * vmax.x, 
					vmin.y + from_int64(point.Y) * vmax.y,
					coord));
			}

			for(size_t i = 0, size = tmpvec.size(); i < size; ++i) {
				const size_t next = (i+1)%size;

				curmesh.vertcnt.push_back(4);

				const IfcVector3& in_world = tmpvec[i];
				const IfcVector3& next_world = tmpvec[next];

				// Assumptions: no 'partial' openings, wall thickness roughly the same across the wall
				curmesh.verts.push_back(in_world);
				curmesh.verts.push_back(in_world+wall_extrusion);
				curmesh.verts.push_back(next_world+wall_extrusion);
				curmesh.verts.push_back(next_world);
			}
		}
	}
	
	std::vector< std::vector<p2t::Point*> > contours;
	BOOST_FOREACH(ClipperLib::ExPolygon& clip, clipped) {
		
		contours.clear();

		// Build the outer polygon contour line for feeding into poly2tri
		std::vector<p2t::Point*> contour_points;
		BOOST_FOREACH(ClipperLib::IntPoint& point, clip.outer) {
			contour_points.push_back( new p2t::Point(from_int64(point.X), from_int64(point.Y)) );
		}

		p2t::CDT* cdt ;
		try {
			// Note: this relies on custom modifications in poly2tri to raise runtime_error's
			// instead if assertions. These failures are not debug only, they can actually
			// happen in production use if the input data is broken. An assertion would be
			// inappropriate.
			cdt = new p2t::CDT(contour_points);
		}
		catch(const std::exception& e) {
			IFCImporter::LogError("Ifc: error during polygon triangulation, skipping some openings: (poly2tri: " 
				+ std::string(e.what()) + ")");
			continue;
		}
		

		// Build the poly2tri inner contours for all holes we got from ClipperLib
		BOOST_FOREACH(ClipperLib::Polygon& opening, clip.holes) {
			
			contours.push_back(std::vector<p2t::Point*>());
			std::vector<p2t::Point*>& contour = contours.back();

			BOOST_FOREACH(ClipperLib::IntPoint& point, opening) {
				contour.push_back( new p2t::Point(from_int64(point.X), from_int64(point.Y)) );
			}

			cdt->AddHole(contour);
		}
		
		try {
			// Note: See above
			cdt->Triangulate();
		}
		catch(const std::exception& e) {
			IFCImporter::LogError("Ifc: error during polygon triangulation, skipping some openings: (poly2tri: " 
				+ std::string(e.what()) + ")");
			continue;
		}

		const std::vector<p2t::Triangle*>& tris = cdt->GetTriangles();

		// Collect the triangles we just produced
		BOOST_FOREACH(p2t::Triangle* tri, tris) {
			for(int i = 0; i < 3; ++i) {

				const IfcVector2& v = IfcVector2( 
					static_cast<IfcFloat>( tri->GetPoint(i)->x ), 
					static_cast<IfcFloat>( tri->GetPoint(i)->y )
				);

				assert(v.x <= 1.0 && v.x >= 0.0 && v.y <= 1.0 && v.y >= 0.0);
				const IfcVector3 v3 = minv * IfcVector3(vmin.x + v.x * vmax.x, vmin.y + v.y * vmax.y,coord) ; 

				curmesh.verts.push_back(v3);
			}
			curmesh.vertcnt.push_back(3);
		}

		result = true;
	}

	if (!result) {
		// revert -- it's a shame, but better than nothing
		curmesh.verts.insert(curmesh.verts.end(),old_verts.begin(), old_verts.end());
		curmesh.vertcnt.insert(curmesh.vertcnt.end(),old_vertcnt.begin(), old_vertcnt.end());

		IFCImporter::LogError("Ifc: revert, could not generate openings for this wall");
	}

	return result;
}

// ------------------------------------------------------------------------------------------------
struct DistanceSorter {

	DistanceSorter(const IfcVector3& base) : base(base) {}

	bool operator () (const TempOpening& a, const TempOpening& b) const {
		return (a.profileMesh->Center()-base).SquareLength() < (b.profileMesh->Center()-base).SquareLength();
	}

	IfcVector3 base;
};

// ------------------------------------------------------------------------------------------------
struct XYSorter {

	// sort first by X coordinates, then by Y coordinates
	bool operator () (const IfcVector2&a, const IfcVector2& b) const {
		if (a.x == b.x) {
			return a.y < b.y;
		}
		return a.x < b.x;
	}
};

typedef std::pair< IfcVector2, IfcVector2 > BoundingBox;
typedef std::map<IfcVector2,size_t,XYSorter> XYSortedField;


// ------------------------------------------------------------------------------------------------
void QuadrifyPart(const IfcVector2& pmin, const IfcVector2& pmax, XYSortedField& field, 
	const std::vector< BoundingBox >& bbs, 
	std::vector<IfcVector2>& out)
{
	if (!(pmin.x-pmax.x) || !(pmin.y-pmax.y)) {
		return;
	}

	IfcFloat xs = 1e10, xe = 1e10;	
	bool found = false;

	// Search along the x-axis until we find an opening
	XYSortedField::iterator start = field.begin();
	for(; start != field.end(); ++start) {
		const BoundingBox& bb = bbs[(*start).second];
		if(bb.first.x >= pmax.x) {
			break;
		} 

		if (bb.second.x > pmin.x && bb.second.y > pmin.y && bb.first.y < pmax.y) {
			xs = bb.first.x;
			xe = bb.second.x;
			found = true;
			break;
		}
	}

	if (!found) {
		// the rectangle [pmin,pend] is opaque, fill it
		out.push_back(pmin);
		out.push_back(IfcVector2(pmin.x,pmax.y));
		out.push_back(pmax);
		out.push_back(IfcVector2(pmax.x,pmin.y));
		return;
	}

	xs = std::max(pmin.x,xs);
	xe = std::min(pmax.x,xe);

	// see if there's an offset to fill at the top of our quad
	if (xs - pmin.x) {
		out.push_back(pmin);
		out.push_back(IfcVector2(pmin.x,pmax.y));
		out.push_back(IfcVector2(xs,pmax.y));
		out.push_back(IfcVector2(xs,pmin.y));
	}

	// search along the y-axis for all openings that overlap xs and our quad
	IfcFloat ylast = pmin.y;
	found = false;
	for(; start != field.end(); ++start) {
		const BoundingBox& bb = bbs[(*start).second];
		if (bb.first.x > xs || bb.first.y >= pmax.y) {
			break;
		}

		if (bb.second.y > ylast) {

			found = true;
			const IfcFloat ys = std::max(bb.first.y,pmin.y), ye = std::min(bb.second.y,pmax.y);
			if (ys - ylast > 0.0f) {
				QuadrifyPart( IfcVector2(xs,ylast), IfcVector2(xe,ys) ,field,bbs,out);
			}

			// the following are the window vertices

			/*wnd.push_back(IfcVector2(xs,ys));
			wnd.push_back(IfcVector2(xs,ye));
			wnd.push_back(IfcVector2(xe,ye));
			wnd.push_back(IfcVector2(xe,ys));*/
			ylast = ye;
		}
	}
	if (!found) {
		// the rectangle [pmin,pend] is opaque, fill it
		out.push_back(IfcVector2(xs,pmin.y));
		out.push_back(IfcVector2(xs,pmax.y));
		out.push_back(IfcVector2(xe,pmax.y));
		out.push_back(IfcVector2(xe,pmin.y));
		return;
	}
	if (ylast < pmax.y) {
		QuadrifyPart( IfcVector2(xs,ylast), IfcVector2(xe,pmax.y) ,field,bbs,out);
	}

	// now for the whole rest
	if (pmax.x-xe) {
		QuadrifyPart(IfcVector2(xe,pmin.y), pmax ,field,bbs,out);
	}
}

typedef std::vector<IfcVector2> Contour;

struct ProjectedWindowContour
{
	Contour contour;
	BoundingBox bb;


	ProjectedWindowContour(const Contour& contour, const BoundingBox& bb)
		: contour(contour)
		, bb(bb)
	{}


	bool IsInvalid() const {
		return contour.empty();
	}

	void FlagInvalid() {
		contour.clear();
	}
};

typedef std::vector< ProjectedWindowContour > ContourVector;

// ------------------------------------------------------------------------------------------------
bool BoundingBoxesOverlapping( const BoundingBox &ibb, const BoundingBox &bb ) 
{
	// count the '=' case as non-overlapping but as adjacent to each other
	return ibb.first.x < bb.second.x && ibb.second.x > bb.first.x &&
		ibb.first.y < bb.second.y && ibb.second.y > bb.first.y;
}

// ------------------------------------------------------------------------------------------------
bool IsDuplicateVertex(const IfcVector2& vv, const std::vector<IfcVector2>& temp_contour)
{
	// sanity check for duplicate vertices
	BOOST_FOREACH(const IfcVector2& cp, temp_contour) {
		if ((cp-vv).SquareLength() < 1e-5f) {
			return true;
		}
	}  
	return false;
}

// ------------------------------------------------------------------------------------------------
void ExtractVerticesFromClipper(const ClipperLib::Polygon& poly, std::vector<IfcVector2>& temp_contour, 
	bool filter_duplicates = false)
{
	temp_contour.clear();
	BOOST_FOREACH(const ClipperLib::IntPoint& point, poly) {
		IfcVector2 vv = IfcVector2( from_int64(point.X), from_int64(point.Y));
		vv = std::max(vv,IfcVector2());
		vv = std::min(vv,one_vec);

		if (!filter_duplicates || !IsDuplicateVertex(vv, temp_contour)) {
			temp_contour.push_back(vv);
		}
	}
}

// ------------------------------------------------------------------------------------------------
BoundingBox GetBoundingBox(const ClipperLib::Polygon& poly)
{
	IfcVector2 newbb_min, newbb_max;
	MinMaxChooser<IfcVector2>()(newbb_min, newbb_max);

	BOOST_FOREACH(const ClipperLib::IntPoint& point, poly) {
		IfcVector2 vv = IfcVector2( from_int64(point.X), from_int64(point.Y));

		// sanity rounding
		vv = std::max(vv,IfcVector2());
		vv = std::min(vv,one_vec);

		newbb_min = std::min(newbb_min,vv);
		newbb_max = std::max(newbb_max,vv);
	}
	return BoundingBox(newbb_min, newbb_max);
}

// ------------------------------------------------------------------------------------------------
void InsertWindowContours(const ContourVector& contours,
	const std::vector<TempOpening>& openings,
	TempMesh& curmesh)
{
	// fix windows - we need to insert the real, polygonal shapes into the quadratic holes that we have now
	for(size_t i = 0; i < contours.size();++i) {
		const BoundingBox& bb = contours[i].bb;
		const std::vector<IfcVector2>& contour = contours[i].contour;
		if(contour.empty()) {
			continue;
		}

		// check if we need to do it at all - many windows just fit perfectly into their quadratic holes,
		// i.e. their contours *are* already their bounding boxes.
		if (contour.size() == 4) {
			std::set<IfcVector2,XYSorter> verts;
			for(size_t n = 0; n < 4; ++n) {
				verts.insert(contour[n]);
			}
			const std::set<IfcVector2,XYSorter>::const_iterator end = verts.end();
			if (verts.find(bb.first)!=end && verts.find(bb.second)!=end
				&& verts.find(IfcVector2(bb.first.x,bb.second.y))!=end 
				&& verts.find(IfcVector2(bb.second.x,bb.first.y))!=end 
				) {
					continue;
			}
		}

		const IfcFloat diag = (bb.first-bb.second).Length();
		const IfcFloat epsilon = diag/1000.f;

		// walk through all contour points and find those that lie on the BB corner
		size_t last_hit = -1, very_first_hit = -1;
		IfcVector2 edge;
		for(size_t n = 0, e=0, size = contour.size();; n=(n+1)%size, ++e) {

			// sanity checking
			if (e == size*2) {
				IFCImporter::LogError("encountered unexpected topology while generating window contour");
				break;
			}

			const IfcVector2& v = contour[n];

			bool hit = false;
			if (fabs(v.x-bb.first.x)<epsilon) {
				edge.x = bb.first.x;
				hit = true;
			}
			else if (fabs(v.x-bb.second.x)<epsilon) {
				edge.x = bb.second.x;
				hit = true;
			}

			if (fabs(v.y-bb.first.y)<epsilon) {
				edge.y = bb.first.y;
				hit = true;
			}
			else if (fabs(v.y-bb.second.y)<epsilon) {
				edge.y = bb.second.y;
				hit = true;
			}

			if (hit) {
				if (last_hit != (size_t)-1) {

					const size_t old = curmesh.verts.size();
					size_t cnt = last_hit > n ? size-(last_hit-n) : n-last_hit;
					for(size_t a = last_hit, e = 0; e <= cnt; a=(a+1)%size, ++e) {
						// hack: this is to fix cases where opening contours are self-intersecting.
						// Clipper doesn't produce such polygons, but as soon as we're back in
						// our brave new floating-point world, very small distances are consumed
						// by the maximum available precision, leading to self-intersecting
						// polygons. This fix makes concave windows fail even worse, but
						// anyway, fail is fail.
						if ((contour[a] - edge).SquareLength() > diag*diag*0.7) {
							continue;
						}
						curmesh.verts.push_back(IfcVector3(contour[a].x, contour[a].y, 0.0f));
					}

					if (edge != contour[last_hit]) {

						IfcVector2 corner = edge;

						if (fabs(contour[last_hit].x-bb.first.x)<epsilon) {
							corner.x = bb.first.x;
						}
						else if (fabs(contour[last_hit].x-bb.second.x)<epsilon) {
							corner.x = bb.second.x;
						}

						if (fabs(contour[last_hit].y-bb.first.y)<epsilon) {
							corner.y = bb.first.y;
						}
						else if (fabs(contour[last_hit].y-bb.second.y)<epsilon) {
							corner.y = bb.second.y;
						}

						curmesh.verts.push_back(IfcVector3(corner.x, corner.y, 0.0f));
					}
					else if (cnt == 1) {
						// avoid degenerate polygons (also known as lines or points)
						curmesh.verts.erase(curmesh.verts.begin()+old,curmesh.verts.end());
					}

					if (const size_t d = curmesh.verts.size()-old) {
						curmesh.vertcnt.push_back(d);
						std::reverse(curmesh.verts.rbegin(),curmesh.verts.rbegin()+d);
					}
					if (n == very_first_hit) {
						break;
					}
				}
				else {
					very_first_hit = n;
				}

				last_hit = n;
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
void MergeWindowContours (const std::vector<IfcVector2>& a, 
	const std::vector<IfcVector2>& b, 
	ClipperLib::ExPolygons& out) 
{
	out.clear();

	ClipperLib::Clipper clipper;
	ClipperLib::Polygon clip;

	BOOST_FOREACH(const IfcVector2& pip, a) {
		clip.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
	}

	if (ClipperLib::Orientation(clip)) {
		std::reverse(clip.begin(), clip.end());
	}

	clipper.AddPolygon(clip, ClipperLib::ptSubject);
	clip.clear();

	BOOST_FOREACH(const IfcVector2& pip, b) {
		clip.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
	}

	if (ClipperLib::Orientation(clip)) {
		std::reverse(clip.begin(), clip.end());
	}

	clipper.AddPolygon(clip, ClipperLib::ptSubject);
	clipper.Execute(ClipperLib::ctUnion, out,ClipperLib::pftNonZero,ClipperLib::pftNonZero);
}

// ------------------------------------------------------------------------------------------------
// Subtract a from b
void MakeDisjunctWindowContours (const std::vector<IfcVector2>& a, 
	const std::vector<IfcVector2>& b, 
	ClipperLib::ExPolygons& out) 
{
	out.clear();

	ClipperLib::Clipper clipper;
	ClipperLib::Polygon clip;

	BOOST_FOREACH(const IfcVector2& pip, a) {
		clip.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
	}

	if (ClipperLib::Orientation(clip)) {
		std::reverse(clip.begin(), clip.end());
	}

	clipper.AddPolygon(clip, ClipperLib::ptClip);
	clip.clear();

	BOOST_FOREACH(const IfcVector2& pip, b) {
		clip.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
	}

	if (ClipperLib::Orientation(clip)) {
		std::reverse(clip.begin(), clip.end());
	}

	clipper.AddPolygon(clip, ClipperLib::ptSubject);
	clipper.Execute(ClipperLib::ctDifference, out,ClipperLib::pftNonZero,ClipperLib::pftNonZero);
}

// ------------------------------------------------------------------------------------------------
void CleanupWindowContour(ProjectedWindowContour& window)
{
	std::vector<IfcVector2> scratch;
	std::vector<IfcVector2>& contour = window.contour;

	ClipperLib::Polygon subject;
	ClipperLib::Clipper clipper;
	ClipperLib::ExPolygons clipped;

	BOOST_FOREACH(const IfcVector2& pip, contour) {
		subject.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
	}

	clipper.AddPolygon(subject,ClipperLib::ptSubject);
	clipper.Execute(ClipperLib::ctUnion,clipped,ClipperLib::pftNonZero,ClipperLib::pftNonZero);

	// This should yield only one polygon or something went wrong 
	if (clipped.size() != 1) {

		// Empty polygon? drop the contour altogether
		if(clipped.empty()) {
			IFCImporter::LogError("error during polygon clipping, window contour is degenerate");
			window.FlagInvalid();
			return;
		}

		// Else: take the first only
		IFCImporter::LogError("error during polygon clipping, window contour is not convex");
	}

	ExtractVerticesFromClipper(clipped[0].outer, scratch);
	// Assume the bounding box doesn't change during this operation
}

// ------------------------------------------------------------------------------------------------
void CleanupWindowContours(ContourVector& contours)
{
	// Use PolyClipper to clean up window contours
	try {
		BOOST_FOREACH(ProjectedWindowContour& window, contours) {
			CleanupWindowContour(window);
		}
	}
	catch (const char* sx) {
		IFCImporter::LogError("error during polygon clipping, window shape may be wrong: (Clipper: " 
			+ std::string(sx) + ")");
	}
}

// ------------------------------------------------------------------------------------------------
void CleanupOuterContour(const std::vector<IfcVector2>& contour_flat, TempMesh& curmesh)
{
	std::vector<IfcVector3> vold;
	std::vector<unsigned int> iold;

	vold.reserve(curmesh.verts.size());
	iold.reserve(curmesh.vertcnt.size());

	// Fix the outer contour using polyclipper
	try {

		ClipperLib::Polygon subject;
		ClipperLib::Clipper clipper;
		ClipperLib::ExPolygons clipped;

		ClipperLib::Polygon clip;
		clip.reserve(contour_flat.size());
		BOOST_FOREACH(const IfcVector2& pip, contour_flat) {
			clip.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
		}

		if (!ClipperLib::Orientation(clip)) {
			std::reverse(clip.begin(), clip.end());
		}

		// We need to run polyclipper on every single polygon -- we can't run it one all
		// of them at once or it would merge them all together which would undo all
		// previous steps
		subject.reserve(4);
		size_t index = 0;
		size_t countdown = 0;
		BOOST_FOREACH(const IfcVector3& pip, curmesh.verts) {
			if (!countdown) {
				countdown = curmesh.vertcnt[index++];
				if (!countdown) {
					continue;
				}
			}
			subject.push_back(ClipperLib::IntPoint(  to_int64(pip.x), to_int64(pip.y) ));
			if (--countdown == 0) {
				if (!ClipperLib::Orientation(subject)) {
					std::reverse(subject.begin(), subject.end());
				}

				clipper.AddPolygon(subject,ClipperLib::ptSubject);
				clipper.AddPolygon(clip,ClipperLib::ptClip);

				clipper.Execute(ClipperLib::ctIntersection,clipped,ClipperLib::pftNonZero,ClipperLib::pftNonZero);

				BOOST_FOREACH(const ClipperLib::ExPolygon& ex, clipped) {
					iold.push_back(ex.outer.size());
					BOOST_FOREACH(const ClipperLib::IntPoint& point, ex.outer) {
						vold.push_back(IfcVector3(
							from_int64(point.X), 
							from_int64(point.Y),
							0.0f));
					}
				}

				subject.clear();
				clipped.clear();
				clipper.Clear();
			}
		}
	}
	catch (const char* sx) {
		IFCImporter::LogError("Ifc: error during polygon clipping, wall contour line may be wrong: (Clipper: " 
			+ std::string(sx) + ")");

		return;
	}

	// swap data arrays
	std::swap(vold,curmesh.verts);
	std::swap(iold,curmesh.vertcnt);
}

typedef std::vector<TempOpening*> OpeningRefs;
typedef std::vector<OpeningRefs > OpeningRefVector;

typedef std::vector<std::pair<
	ContourVector::const_iterator, 
	Contour::const_iterator> 
> ContourRefVector; 

// ------------------------------------------------------------------------------------------------
bool BoundingBoxesAdjacent(const BoundingBox& bb, const BoundingBox& ibb)
{
	// TODO: I'm pretty sure there is a much more compact way to check this
	const IfcFloat epsilon = 1e-5f;
	return	(fabs(bb.second.x - ibb.first.x) < epsilon && bb.first.y <= ibb.second.y && bb.second.y >= ibb.first.y) ||
		(fabs(bb.first.x - ibb.second.x) < epsilon && ibb.first.y <= bb.second.y && ibb.second.y >= bb.first.y) || 
		(fabs(bb.second.y - ibb.first.y) < epsilon && bb.first.x <= ibb.second.x && bb.second.x >= ibb.first.x) ||
		(fabs(bb.first.y - ibb.second.y) < epsilon && ibb.first.x <= bb.second.x && ibb.second.x >= bb.first.x);
}

// ------------------------------------------------------------------------------------------------
// Check if m0,m1 intersects n0,n1 assuming same ordering of the points in the line segments
// output the intersection points on n0,n1
bool IntersectingLineSegments(const IfcVector2& n0, const IfcVector2& n1, 
	const IfcVector2& m0, const IfcVector2& m1,
	IfcVector2& out0, IfcVector2& out1)
{
	const IfcVector2& m0_to_m1 = m1 - m0;
	const IfcVector2& m0_to_n1 = n1 - m0;
	const IfcVector2& n0_to_n1 = n1 - n0;
	const IfcVector2& n0_to_m1 = m1 - n0;

	const IfcFloat m0_to_m1_len = m0_to_m1.SquareLength();
	const IfcFloat m0_to_n1_len = m0_to_n1.SquareLength();
	const IfcFloat n0_to_n1_len = n0_to_n1.SquareLength();
	const IfcFloat n0_to_m1_len = n0_to_m1.SquareLength();

	if (m0_to_m1_len < m0_to_n1_len) {
		return false;
	}

	if (n0_to_n1_len < n0_to_m1_len) {
		return false;
	}

	const IfcFloat epsilon = 1e-5f;
	if (fabs((m0_to_m1 * n0_to_n1) - sqrt(m0_to_m1_len) * sqrt(n0_to_n1_len)) > epsilon) {
		return false;
	}

	if (fabs((m0_to_m1 * m0_to_n1) - sqrt(m0_to_m1_len) * sqrt(m0_to_n1_len)) > epsilon) {
		return false;
	}

	// XXX this condition is probably redundant (or at least a check against > 0 is sufficient)
	if (fabs((n0_to_n1 * n0_to_m1) - sqrt(n0_to_n1_len) * sqrt(n0_to_m1_len)) > epsilon) {
		return false;
	}

	// determine intersection points

	return true;
}

// ------------------------------------------------------------------------------------------------
void FindAdjacentContours(const ContourVector::const_iterator current, const ContourVector& contours)
{
	const BoundingBox& bb = (*current).bb;

	// First step to find possible adjacent contours is to check for adjacent bounding
	// boxes. If the bounding boxes are not adjacent, the contours lines cannot possibly be.
	for (ContourVector::const_iterator it = contours.begin(), end = contours.end(); it != end; ++it) {
		if ((*it).IsInvalid()) {
			continue;
		}

		if(it == current) {
			continue;
		}

		const BoundingBox& ibb = (*it).bb;

		// Assumption: the bounding boxes are pairwise disjoint
		ai_assert(!BoundingBoxesOverlapping(bb, ibb));

		if (BoundingBoxesAdjacent(bb, ibb)) {

			// Now do a each-against-everyone check for intersecting contour
			// lines. This obviously scales terribly, but in typical real
			// world Ifc files it will not matter since most windows that
			// are adjacent to each others are rectangular anyway.

			const Contour& ncontour = (*current).contour;
			const Contour& mcontour = (*it).contour;

			for (size_t n = 0, nend = ncontour.size(); n < nend; ++n) {
				const IfcVector2& n0 = ncontour[n];
				const IfcVector2& n1 = ncontour[(n+1) % ncontour.size()];

				for (size_t m = 0, mend = mcontour.size(); m < nend; ++m) {
					const IfcVector2& m0 = ncontour[m];
					const IfcVector2& m1 = ncontour[(m+1) % mcontour.size()];

					IfcVector2 isect0, isect1;
					if (IntersectingLineSegments(n0,n1, m0, m1, isect0, isect1)) {
						// Find intersection range
					}
				}
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
void CloseWindows(const ContourVector& contours, 		  
	const IfcMatrix4& minv, 
	OpeningRefVector contours_to_openings, 
	TempMesh& curmesh)
{
	// For all contour points, check if one of the assigned openings does
	// already have points assigned to it. In this case, assume this is
	// the other side of the wall and generate connections between
	// the two holes in order to close the window. 

	// All this gets complicated by the fact that contours may pertain to
	// multiple openings(due to merging of adjacent or overlapping openings). 
	// The code is based on the assumption that this happens symmetrically
	// on both sides of the wall. If it doesn't (which would be a bug anyway)
	// wrong geometry may be generated.
	for (ContourVector::const_iterator it = contours.begin(), end = contours.end(); it != end; ++it) {
		if ((*it).IsInvalid()) {
			continue;
		}
		OpeningRefs& refs = contours_to_openings[std::distance(contours.begin(), it)];

		bool has_other_side = false;
		BOOST_FOREACH(const TempOpening* opening, refs) {
			if(!opening->wallPoints.empty()) {
				has_other_side = true;
				break;
			}
		}

		ContourRefVector adjacent_contours;
		FindAdjacentContours(it, contours);

		const Contour::const_iterator cbegin = (*it).contour.begin(), cend = (*it).contour.end();

		if (has_other_side) {
			curmesh.verts.reserve(curmesh.verts.size() + (*it).contour.size() * 4);
			curmesh.vertcnt.reserve(curmesh.vertcnt.size() + (*it).contour.size());

			// XXX this algorithm is really a bit inefficient - both in terms
			// of constant factor and of asymptotic runtime.
			size_t vstart = curmesh.verts.size();
			bool outer_border = false;
			IfcVector2 last_proj_point;
			IfcVector3 last_diff;

			const IfcFloat border_epsilon_upper = static_cast<IfcFloat>(1-1e-4);
			const IfcFloat border_epsilon_lower = static_cast<IfcFloat>(1e-4);

			bool start_is_outer_border = false;

			for (Contour::const_iterator cit = cbegin; cit != cend; ++cit) {
				const IfcVector2& proj_point = *cit;

				// Locate the closest opposite point. This should be a good heuristic to
				// connect only the points that are really intended to be connected.
				IfcFloat best = static_cast<IfcFloat>(1e10);
				IfcVector3 bestv;

				const IfcVector3& world_point = minv * IfcVector3(proj_point.x,proj_point.y,0.0f);

				BOOST_FOREACH(const TempOpening* opening, refs) {
					BOOST_FOREACH(const IfcVector3& other, opening->wallPoints) {
						const IfcFloat sqdist = (world_point - other).SquareLength();
						if (sqdist < best) {
							bestv = other;
							best = sqdist;
						}
					}
				}

				// Check if this connection is along the outer boundary of the projection
				// plane. In such a case we better drop it because such 'edges' should
				// not have any geometry to close them (think of door openings).
				bool drop_this_edge = false;
				if (proj_point.x <= border_epsilon_lower || proj_point.x >= border_epsilon_upper ||
					proj_point.y <= border_epsilon_lower || proj_point.y >= border_epsilon_upper) {

					if (outer_border) {
						ai_assert(cit != cbegin);
						if (fabs((proj_point.x - last_proj_point.x) * (proj_point.y - last_proj_point.y)) < 1e-5f) {
							drop_this_edge = true;

							curmesh.verts.pop_back();
							curmesh.verts.pop_back();
						}
					}
					else if (cit == cbegin) {
						start_is_outer_border = true;
					}
					outer_border = true;
				}
				else {
					outer_border = false;
				}

				last_proj_point = proj_point;

				IfcVector3 diff = bestv - world_point;
				diff.Normalize();

				if (!drop_this_edge) {
					curmesh.verts.push_back(bestv);
					curmesh.verts.push_back(world_point);

					curmesh.vertcnt.push_back(4);
				}

				last_diff = diff;

				if (cit != cbegin) {
					curmesh.verts.push_back(world_point);
					curmesh.verts.push_back(bestv);

					if (cit == cend - 1) {

						// Check if the final connection (last to first element) is itself
						// a border edge that needs to be dropped.
						if (start_is_outer_border && outer_border &&
							fabs((proj_point.x - (*cbegin).x) * (proj_point.y - (*cbegin).y)) < 1e-5f) {

							curmesh.vertcnt.pop_back();
							curmesh.verts.pop_back();
							curmesh.verts.pop_back();
						}
						else {
							curmesh.verts.push_back(curmesh.verts[vstart]);
							curmesh.verts.push_back(curmesh.verts[vstart+1]);
						}
					}
				}
			}
		}
		else {
			BOOST_FOREACH(TempOpening* opening, refs) {
				opening->wallPoints.reserve(opening->wallPoints.capacity() + (*it).contour.size());
				for (Contour::const_iterator cit = cbegin; cit != cend; ++cit) {

					const IfcVector2& proj_point = *cit;
					opening->wallPoints.push_back(minv * IfcVector3(proj_point.x,proj_point.y,0.0f));
				}
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
void Quadrify(const std::vector< BoundingBox >& bbs, TempMesh& curmesh)
{
	ai_assert(curmesh.IsEmpty());

	std::vector<IfcVector2> quads;
	quads.reserve(bbs.size()*4);

	// sort openings by x and y axis as a preliminiary to the QuadrifyPart() algorithm
	XYSortedField field;
	for (std::vector<BoundingBox>::const_iterator it = bbs.begin(); it != bbs.end(); ++it) {
		if (field.find((*it).first) != field.end()) {
			IFCImporter::LogWarn("constraint failure during generation of wall openings, results may be faulty");
		}
		field[(*it).first] = std::distance(bbs.begin(),it);
	}

	QuadrifyPart(IfcVector2(),one_vec,field,bbs,quads);
	ai_assert(!(quads.size() % 4));

	curmesh.vertcnt.resize(quads.size()/4,4);
	curmesh.verts.reserve(quads.size());
	BOOST_FOREACH(const IfcVector2& v2, quads) {
		curmesh.verts.push_back(IfcVector3(v2.x, v2.y, static_cast<IfcFloat>(0.0)));
	}
}

// ------------------------------------------------------------------------------------------------
void Quadrify(const ContourVector& contours, TempMesh& curmesh)
{
	std::vector<BoundingBox> bbs;
	bbs.reserve(contours.size());

	BOOST_FOREACH(const ContourVector::value_type& val, contours) {
		bbs.push_back(val.bb);
	}

	Quadrify(bbs, curmesh);
}

// ------------------------------------------------------------------------------------------------
IfcMatrix4 ProjectOntoPlane(std::vector<IfcVector2>& out_contour, const TempMesh& in_mesh, 
	IfcFloat& out_base_d, bool &ok)
{
	const std::vector<IfcVector3>& in_verts = in_mesh.verts;
	ok = true;

	IfcMatrix4 m = IfcMatrix4(DerivePlaneCoordinateSpace(in_mesh, ok, &out_base_d));
	if(!ok) {
		return IfcMatrix4();
	}


	IfcFloat coord = -1;
	out_contour.reserve(in_verts.size());

	IfcVector2 vmin, vmax;
	MinMaxChooser<IfcVector2>()(vmin, vmax);

	// Project all points into the new coordinate system, collect min/max verts on the way
	BOOST_FOREACH(const IfcVector3& x, in_verts) {
		const IfcVector3& vv = m * x;
		// keep Z offset in the plane coordinate system. Ignoring precision issues
		// (which  are present, of course), this should be the same value for
		// all polygon vertices (assuming the polygon is planar).

		// XXX this should be guarded, but we somehow need to pick a suitable
		// epsilon
		// if(coord != -1.0f) {
		//	assert(fabs(coord - vv.z) < 1e-3f);
		// }
		coord = vv.z;
		vmin = std::min(IfcVector2(vv.x, vv.y), vmin);
		vmax = std::max(IfcVector2(vv.x, vv.y), vmax);

		out_contour.push_back(IfcVector2(vv.x,vv.y));
	}

	// Further improve the projection by mapping the entire working set into
	// [0,1] range. This gives us a consistent data range so all epsilons
	// used below can be constants.
	vmax -= vmin;
	BOOST_FOREACH(IfcVector2& vv, out_contour) {
		vv.x  = (vv.x - vmin.x) / vmax.x;
		vv.y  = (vv.y - vmin.y) / vmax.y;

		// sanity rounding
		vv = std::max(vv,IfcVector2());
		vv = std::min(vv,one_vec);
	}

	IfcMatrix4 mult;
	mult.a1 = static_cast<IfcFloat>(1.0) / vmax.x;
	mult.b2 = static_cast<IfcFloat>(1.0) / vmax.y;

	mult.a4 = -vmin.x * mult.a1;
	mult.b4 = -vmin.y * mult.b2;
	mult.c4 = -coord;
	m = mult * m;

	return m;
}

// ------------------------------------------------------------------------------------------------
bool GenerateOpenings(std::vector<TempOpening>& openings,
	const std::vector<IfcVector3>& nors, 
	TempMesh& curmesh,
	bool check_intersection,
	bool generate_connection_geometry)
{
	std::vector<IfcVector3>& out = curmesh.verts;
	OpeningRefVector contours_to_openings;

	// Try to derive a solid base plane within the current surface for use as 
	// working coordinate system. Map all vertices onto this plane and 
	// rescale them to [0,1] range. This normalization means all further
	// epsilons need not be scaled.
	bool ok = true;

	std::vector<IfcVector2> contour_flat;
	IfcFloat base_d;

	const IfcMatrix4& m = ProjectOntoPlane(contour_flat, curmesh, base_d, ok);
	if(!ok) {
		return false;
	}

	const IfcVector3& nor = IfcVector3(m.c1, m.c2, m.c3);

	// Obtain inverse transform for getting back to world space later on
	const IfcMatrix4& minv = IfcMatrix4(m).Inverse();

	// Compute bounding boxes for all 2D openings in projection space
	ContourVector contours;

	std::vector<IfcVector2> temp_contour;

	size_t c = 0;
	BOOST_FOREACH(TempOpening& opening,openings) {
		std::vector<IfcVector3> profile_verts = opening.profileMesh->verts;
		std::vector<unsigned int> profile_vertcnts = opening.profileMesh->vertcnt;
		if(profile_verts.size() <= 2) {
			continue;	
		}

		IfcVector2 vpmin,vpmax;
		MinMaxChooser<IfcVector2>()(vpmin,vpmax);

		// The opening meshes are real 3D meshes so skip over all faces
		// clearly facing into the wrong direction. Also, we need to check
		// whether the meshes do actually intersect the base surface plane.
		// This is done by recording minimum and maximum values for the
		// d component of the plane equation for all polys and checking
		// against surface d.
		IfcFloat dmin, dmax;
		MinMaxChooser<IfcFloat>()(dmin,dmax);

		temp_contour.clear();
		for (size_t f = 0, vi_total = 0, fend = profile_vertcnts.size(); f < fend; ++f) {
			const IfcVector3& face_nor = ((profile_verts[vi_total+2] - profile_verts[vi_total]) ^
				(profile_verts[vi_total+1] - profile_verts[vi_total])).Normalize();

			const IfcFloat abs_dot_face_nor = abs(nor * face_nor);
			if (abs_dot_face_nor < 0.5) {
				vi_total += profile_vertcnts[f];
				continue;
			}

			for (unsigned int vi = 0, vend = profile_vertcnts[f]; vi < vend; ++vi, ++vi_total) {
				const IfcVector3& x = profile_verts[vi_total];

				if(check_intersection) {
					const IfcFloat vert_d = -(x * nor);
					dmin = std::min(dmin, vert_d);
					dmax = std::max(dmax, vert_d);
				}

				const IfcVector3& v = m * x;
				IfcVector2 vv(v.x, v.y);

				// sanity rounding
				vv = std::max(vv,IfcVector2());
				vv = std::min(vv,one_vec);

				vpmin = std::min(vpmin,vv);
				vpmax = std::max(vpmax,vv);

				if (!IsDuplicateVertex(vv, temp_contour)) {
					temp_contour.push_back(vv);
				}		
			}
		}

		if(temp_contour.size() <= 2) {
			continue;
		}

		// TODO: This epsilon may be too large
		const IfcFloat epsilon = fabs(dmax-dmin) * 0.01;
		if (check_intersection && (base_d < dmin-epsilon || base_d > dmax+epsilon)) {
			continue;
		}

		BoundingBox bb = BoundingBox(vpmin,vpmax);

		// Skip over very small openings - these are likely projection errors
		// (i.e. they don't belong to this side of the wall)
		if(fabs(vpmax.x - vpmin.x) * fabs(vpmax.y - vpmin.y) < static_cast<IfcFloat>(1e-5)) {
			continue;
		}
		std::vector<TempOpening*> joined_openings(1, &opening);

		// See if this BB intersects or is in close adjacency to any other BB we have so far.
		for (ContourVector::iterator it = contours.begin(); it != contours.end(); ) {
			const BoundingBox& ibb = (*it).bb;

			if (BoundingBoxesOverlapping(ibb, bb)) {

				const std::vector<IfcVector2>& other = (*it).contour;
				ClipperLib::ExPolygons poly;

				// First check whether subtracting the old contour (to which ibb belongs)
				// from the new contour (to which bb belongs) yields an updated bb which
				// no longer overlaps ibb
				MakeDisjunctWindowContours(other, temp_contour, poly);
				if(poly.size() == 1) {
					
					const BoundingBox& newbb = GetBoundingBox(poly[0].outer);
					if (!BoundingBoxesOverlapping(ibb, newbb )) {
						 // Good guy bounding box
						 bb = newbb ;

						 ExtractVerticesFromClipper(poly[0].outer, temp_contour, false);
						 continue;
					}
				}

				// Take these two overlapping contours and try to merge them. If they 
				// overlap (which should not happen, but in fact happens-in-the-real-
				// world [tm] ), resume using a single contour and a single bounding box.
				MergeWindowContours(temp_contour, other, poly);

				if (poly.size() > 1) { 
					return TryAddOpenings_Poly2Tri(openings, nors, curmesh);
				}
				else if (poly.size() == 0) {
					IFCImporter::LogWarn("ignoring duplicate opening");
					temp_contour.clear();
					break;
				}
				else {
					IFCImporter::LogDebug("merging overlapping openings");				
					ExtractVerticesFromClipper(poly[0].outer, temp_contour, true);

					// Generate the union of the bounding boxes
					bb.first = std::min(bb.first, ibb.first);
					bb.second = std::max(bb.second, ibb.second);

					// Update contour-to-opening tables accordingly
					if (generate_connection_geometry) {
						std::vector<TempOpening*>& t = contours_to_openings[std::distance(contours.begin(),it)]; 
						joined_openings.insert(joined_openings.end(), t.begin(), t.end());

						contours_to_openings.erase(contours_to_openings.begin() + std::distance(contours.begin(),it));
					}

					contours.erase(it);

					// Restart from scratch because the newly formed BB might now
					// overlap any other BB which its constituent BBs didn't
					// previously overlap.
					it = contours.begin();
					continue;
				}
			}
			++it;
		}

		if(!temp_contour.empty()) {
			if (generate_connection_geometry) {
				contours_to_openings.push_back(std::vector<TempOpening*>(
					joined_openings.begin(),
					joined_openings.end()));
			}

			contours.push_back(ProjectedWindowContour(temp_contour, bb));
		}
	}

	// Check if we still have any openings left - it may well be that this is
	// not the cause, for example if all the opening candidates don't intersect
	// this surface or point into a direction perpendicular to it.
	if (contours.empty()) {
		return false;
	}

	curmesh.Clear();

	// Generate a base subdivision into quads to accommodate the given list
	// of window bounding boxes.
	Quadrify(contours,curmesh);

	// Run a sanity cleanup pass on the window contours to avoid generating
	// artifacts during the contour generation phase later on.
	CleanupWindowContours(contours);

	// Previously we reduced all windows to rectangular AABBs in projection
	// space, now it is time to fill the gaps between the BBs and the real
	// window openings.
	InsertWindowContours(contours,openings, curmesh);

	// Clip the entire outer contour of our current result against the real
	// outer contour of the surface. This is necessary because the result
	// of the Quadrify() algorithm is always a square area spanning
	// over [0,1]^2 (i.e. entire projection space).
	CleanupOuterContour(contour_flat, curmesh);

	// Undo the projection and get back to world (or local object) space
	BOOST_FOREACH(IfcVector3& v3, curmesh.verts) {
		v3 = minv * v3;
	}
	
	// TODO:
	// This should connect the window openings on both sides of the wall,
	// but it produces lots of artifacts which are not resolved yet.
	// Most of all, it makes all cases in which adjacent openings are
	// not correctly merged together glaringly obvious.
	if (generate_connection_geometry) {
		CloseWindows(contours, minv, contours_to_openings, curmesh);
	}
	return true;
}


// ------------------------------------------------------------------------------------------------
void ProcessExtrudedAreaSolid(const IfcExtrudedAreaSolid& solid, TempMesh& result, 
	ConversionData& conv)
{
	TempMesh meshout;
	
	// First read the profile description
	if(!ProcessProfile(*solid.SweptArea,meshout,conv) || meshout.verts.size()<=1) {
		return;
	}

	IfcVector3 dir;
	ConvertDirection(dir,solid.ExtrudedDirection);

	dir *= solid.Depth;

	// Outline: assuming that `meshout.verts` is now a list of vertex points forming 
	// the underlying profile, extrude along the given axis, forming new
	// triangles.
	
	std::vector<IfcVector3>& in = meshout.verts;
	const size_t size=in.size();

	const bool has_area = solid.SweptArea->ProfileType == "AREA" && size>2;
	if(solid.Depth < 1e-3) {
		if(has_area) {
			meshout = result;
		}
		return;
	}

	result.verts.reserve(size*(has_area?4:2));
	result.vertcnt.reserve(meshout.vertcnt.size()+2);

	// First step: transform all vertices into the target coordinate space
	IfcMatrix4 trafo;
	ConvertAxisPlacement(trafo, solid.Position);
	BOOST_FOREACH(IfcVector3& v,in) {
		v *= trafo;
	}
	
	IfcVector3 min = in[0];
	dir *= IfcMatrix3(trafo);


	std::vector<IfcVector3> nors;
	const bool openings = !!conv.apply_openings && conv.apply_openings->size();
	
	// Compute the normal vectors for all opening polygons as a prerequisite
	// to TryAddOpenings_Poly2Tri()
	// XXX this belongs into the aforementioned function
	if (openings) {

		if (!conv.settings.useCustomTriangulation) {	         
			// it is essential to apply the openings in the correct spatial order. The direction	 
			// doesn't matter, but we would screw up if we started with e.g. a door in between	 
			// two windows.	 
			std::sort(conv.apply_openings->begin(),conv.apply_openings->end(),
				DistanceSorter(min));	 
		}
	
		nors.reserve(conv.apply_openings->size());
		BOOST_FOREACH(TempOpening& t,*conv.apply_openings) {
			TempMesh& bounds = *t.profileMesh.get();
		
			if (bounds.verts.size() <= 2) {
				nors.push_back(IfcVector3());
				continue;
			}
			nors.push_back(((bounds.verts[2]-bounds.verts[0])^(bounds.verts[1]-bounds.verts[0]) ).Normalize());
		}
	}
	

	TempMesh temp;
	TempMesh& curmesh = openings ? temp : result;
	std::vector<IfcVector3>& out = curmesh.verts;
 
	size_t sides_with_openings = 0;
	for(size_t i = 0; i < size; ++i) {
		const size_t next = (i+1)%size;

		curmesh.vertcnt.push_back(4);
		
		out.push_back(in[i]);
		out.push_back(in[i]+dir);
		out.push_back(in[next]+dir);
		out.push_back(in[next]);

		if(openings) {
			if(GenerateOpenings(*conv.apply_openings,nors,temp)) {
				++sides_with_openings;
			}
			
			result.Append(temp);
			temp.Clear();
		}
	}
	
	size_t sides_with_v_openings = 0;
	if(has_area) {

		for(size_t n = 0; n < 2; ++n) {
			for(size_t i = size; i--; ) {
				out.push_back(in[i]+(n?dir:IfcVector3()));
			}

			curmesh.vertcnt.push_back(size);
			if(openings && size > 2) {
				if(GenerateOpenings(*conv.apply_openings,nors,temp)) {
					++sides_with_v_openings;
				}

				result.Append(temp);
				temp.Clear();
			}
		}
	}

	if(openings && ((sides_with_openings == 1 && sides_with_openings) || (sides_with_v_openings == 2 && sides_with_v_openings))) {
		IFCImporter::LogWarn("failed to resolve all openings, presumably their topology is not supported by Assimp");
	}

	IFCImporter::LogDebug("generate mesh procedurally by extrusion (IfcExtrudedAreaSolid)");
}

// ------------------------------------------------------------------------------------------------
void ProcessSweptAreaSolid(const IfcSweptAreaSolid& swept, TempMesh& meshout, 
	ConversionData& conv)
{
	if(const IfcExtrudedAreaSolid* const solid = swept.ToPtr<IfcExtrudedAreaSolid>()) {
		ProcessExtrudedAreaSolid(*solid,meshout,conv);
	}
	else if(const IfcRevolvedAreaSolid* const rev = swept.ToPtr<IfcRevolvedAreaSolid>()) {
		ProcessRevolvedAreaSolid(*rev,meshout,conv);
	}
	else {
		IFCImporter::LogWarn("skipping unknown IfcSweptAreaSolid entity, type is " + swept.GetClassName());
	}
}

// ------------------------------------------------------------------------------------------------
enum Intersect {
	Intersect_No,
	Intersect_LiesOnPlane,
	Intersect_Yes
};

// ------------------------------------------------------------------------------------------------
Intersect IntersectSegmentPlane(const IfcVector3& p,const IfcVector3& n, const IfcVector3& e0, 
	const IfcVector3& e1, 
	IfcVector3& out) 
{
	const IfcVector3 pdelta = e0 - p, seg = e1-e0;
	const IfcFloat dotOne = n*seg, dotTwo = -(n*pdelta);

	if (fabs(dotOne) < 1e-6) {
		return fabs(dotTwo) < 1e-6f ? Intersect_LiesOnPlane : Intersect_No;
	}

	const IfcFloat t = dotTwo/dotOne;
	// t must be in [0..1] if the intersection point is within the given segment
	if (t > 1.f || t < 0.f) {
		return Intersect_No;
	}
	out = e0+t*seg;
	return Intersect_Yes;
}

// ------------------------------------------------------------------------------------------------
void ProcessBooleanHalfSpaceDifference(const IfcHalfSpaceSolid* hs, TempMesh& result, 
	const TempMesh& first_operand, 
	ConversionData& conv)
{
	ai_assert(hs != NULL);

	const IfcPlane* const plane = hs->BaseSurface->ToPtr<IfcPlane>();
	if(!plane) {
		IFCImporter::LogError("expected IfcPlane as base surface for the IfcHalfSpaceSolid");
		return;
	}

	// extract plane base position vector and normal vector
	IfcVector3 p,n(0.f,0.f,1.f);
	if (plane->Position->Axis) {
		ConvertDirection(n,plane->Position->Axis.Get());
	}
	ConvertCartesianPoint(p,plane->Position->Location);

	if(!IsTrue(hs->AgreementFlag)) {
		n *= -1.f;
	}

	// clip the current contents of `meshout` against the plane we obtained from the second operand
	const std::vector<IfcVector3>& in = first_operand.verts;
	std::vector<IfcVector3>& outvert = result.verts;

	std::vector<unsigned int>::const_iterator begin = first_operand.vertcnt.begin(), 
		end = first_operand.vertcnt.end(), iit;

	outvert.reserve(in.size());
	result.vertcnt.reserve(first_operand.vertcnt.size());

	unsigned int vidx = 0;
	for(iit = begin; iit != end; vidx += *iit++) {

		unsigned int newcount = 0;
		for(unsigned int i = 0; i < *iit; ++i) {
			const IfcVector3& e0 = in[vidx+i], e1 = in[vidx+(i+1)%*iit];

			// does the next segment intersect the plane?
			IfcVector3 isectpos;
			const Intersect isect = IntersectSegmentPlane(p,n,e0,e1,isectpos);
			if (isect == Intersect_No || isect == Intersect_LiesOnPlane) {
				if ( (e0-p).Normalize()*n > 0 ) {
					outvert.push_back(e0);
					++newcount;
				}
			}
			else if (isect == Intersect_Yes) {
				if ( (e0-p).Normalize()*n > 0 ) {
					// e0 is on the right side, so keep it 
					outvert.push_back(e0);
					outvert.push_back(isectpos);
					newcount += 2;
				}
				else {
					// e0 is on the wrong side, so drop it and keep e1 instead
					outvert.push_back(isectpos);
					++newcount;
				}
			}
		}	

		if (!newcount) {
			continue;
		}

		IfcVector3 vmin,vmax;
		ArrayBounds(&*(outvert.end()-newcount),newcount,vmin,vmax);

		// filter our IfcFloat points - those may happen if a point lies
		// directly on the intersection line. However, due to IfcFloat
		// precision a bitwise comparison is not feasible to detect
		// this case.
		const IfcFloat epsilon = (vmax-vmin).SquareLength() / 1e6f;
		FuzzyVectorCompare fz(epsilon);

		std::vector<IfcVector3>::iterator e = std::unique( outvert.end()-newcount, outvert.end(), fz );

		if (e != outvert.end()) {
			newcount -= static_cast<unsigned int>(std::distance(e,outvert.end()));
			outvert.erase(e,outvert.end());
		}
		if (fz(*( outvert.end()-newcount),outvert.back())) {
			outvert.pop_back();
			--newcount;
		}
		if(newcount > 2) {
			result.vertcnt.push_back(newcount);
		}
		else while(newcount-->0) {
			result.verts.pop_back();
		}

	}
	IFCImporter::LogDebug("generating CSG geometry by plane clipping (IfcBooleanClippingResult)");
}

// ------------------------------------------------------------------------------------------------
void ProcessBooleanExtrudedAreaSolidDifference(const IfcExtrudedAreaSolid* as, TempMesh& result, 
   const TempMesh& first_operand, 
   ConversionData& conv)
{
	ai_assert(as != NULL);

	// This case is handled by reduction to an instance of the quadrify() algorithm.
	// Obviously, this won't work for arbitrarily complex cases. In fact, the first
	// operand should be near-planar. Luckily, this is usually the case in Ifc 
	// buildings.

	boost::shared_ptr<TempMesh> meshtmp(new TempMesh());
	ProcessExtrudedAreaSolid(*as,*meshtmp,conv);

	std::vector<TempOpening> openings(1, TempOpening(as,IfcVector3(0,0,0),meshtmp));

	result = first_operand;

	TempMesh temp;

	std::vector<IfcVector3>::const_iterator vit = first_operand.verts.begin();
	BOOST_FOREACH(unsigned int pcount, first_operand.vertcnt) {
		temp.Clear();

		temp.verts.insert(temp.verts.end(), vit, vit + pcount);
		temp.vertcnt.push_back(pcount);

		// The algorithms used to generate mesh geometry sometimes
		// spit out lines or other degenerates which must be
		// filtered to avoid running into assertions later on.

		// ComputePolygonNormal returns the Newell normal, so the
		// length of the normal is the area of the polygon.
		const IfcVector3& normal = temp.ComputeLastPolygonNormal(false);
		if (normal.SquareLength() < static_cast<IfcFloat>(1e-5)) {
			IFCImporter::LogWarn("skipping degenerate polygon (ProcessBooleanExtrudedAreaSolidDifference)");
			continue;
		}

		GenerateOpenings(openings, std::vector<IfcVector3>(1,IfcVector3(1,0,0)), temp);
		result.Append(temp);

		vit += pcount;
	}

	IFCImporter::LogDebug("generating CSG geometry by geometric difference to a solid (IfcExtrudedAreaSolid)");
}

// ------------------------------------------------------------------------------------------------
void ProcessBoolean(const IfcBooleanResult& boolean, TempMesh& result, ConversionData& conv)
{
	// supported CSG operations:
	//   DIFFERENCE
	if(const IfcBooleanResult* const clip = boolean.ToPtr<IfcBooleanResult>()) {
		if(clip->Operator != "DIFFERENCE") {
			IFCImporter::LogWarn("encountered unsupported boolean operator: " + (std::string)clip->Operator);
			return;
		}

		// supported cases (1st operand):
		//  IfcBooleanResult -- call ProcessBoolean recursively
		//  IfcSweptAreaSolid -- obtain polygonal geometry first

		// supported cases (2nd operand):
		//  IfcHalfSpaceSolid -- easy, clip against plane
		//  IfcExtrudedAreaSolid -- reduce to an instance of the quadrify() algorithm

		
		const IfcHalfSpaceSolid* const hs = clip->SecondOperand->ResolveSelectPtr<IfcHalfSpaceSolid>(conv.db);
		const IfcExtrudedAreaSolid* const as = clip->SecondOperand->ResolveSelectPtr<IfcExtrudedAreaSolid>(conv.db);
		if(!hs && !as) {
			IFCImporter::LogError("expected IfcHalfSpaceSolid or IfcExtrudedAreaSolid as second clipping operand");
			return;
		}

		TempMesh first_operand;
		if(const IfcBooleanResult* const op0 = clip->FirstOperand->ResolveSelectPtr<IfcBooleanResult>(conv.db)) {
			ProcessBoolean(*op0,first_operand,conv);
		}
		else if (const IfcSweptAreaSolid* const swept = clip->FirstOperand->ResolveSelectPtr<IfcSweptAreaSolid>(conv.db)) {
			ProcessSweptAreaSolid(*swept,first_operand,conv);
		}
		else {
			IFCImporter::LogError("expected IfcSweptAreaSolid or IfcBooleanResult as first clipping operand");
			return;
		}

		if(hs) {
			ProcessBooleanHalfSpaceDifference(hs, result, first_operand, conv);
		}
		else {
			ProcessBooleanExtrudedAreaSolidDifference(as, result, first_operand, conv);
		}
	}
	else {
		IFCImporter::LogWarn("skipping unknown IfcBooleanResult entity, type is " + boolean.GetClassName());
	}
}

// ------------------------------------------------------------------------------------------------
bool ProcessGeometricItem(const IfcRepresentationItem& geo, std::vector<unsigned int>& mesh_indices, 
	ConversionData& conv)
{
	bool fix_orientation = true;
	boost::shared_ptr< TempMesh > meshtmp = boost::make_shared<TempMesh>(); 
	if(const IfcShellBasedSurfaceModel* shellmod = geo.ToPtr<IfcShellBasedSurfaceModel>()) {
		BOOST_FOREACH(boost::shared_ptr<const IfcShell> shell,shellmod->SbsmBoundary) {
			try {
				const EXPRESS::ENTITY& e = shell->To<ENTITY>();
				const IfcConnectedFaceSet& fs = conv.db.MustGetObject(e).To<IfcConnectedFaceSet>(); 

				ProcessConnectedFaceSet(fs,*meshtmp.get(),conv);
			}
			catch(std::bad_cast&) {
				IFCImporter::LogWarn("unexpected type error, IfcShell ought to inherit from IfcConnectedFaceSet");
			}
		}
	}
	else  if(const IfcConnectedFaceSet* fset = geo.ToPtr<IfcConnectedFaceSet>()) {
		ProcessConnectedFaceSet(*fset,*meshtmp.get(),conv);
	}	
	else  if(const IfcSweptAreaSolid* swept = geo.ToPtr<IfcSweptAreaSolid>()) {
		ProcessSweptAreaSolid(*swept,*meshtmp.get(),conv);
	}   
	else  if(const IfcSweptDiskSolid* disk = geo.ToPtr<IfcSweptDiskSolid>()) {
		ProcessSweptDiskSolid(*disk,*meshtmp.get(),conv);
		fix_orientation = false;
	}   
	else if(const IfcManifoldSolidBrep* brep = geo.ToPtr<IfcManifoldSolidBrep>()) {
		ProcessConnectedFaceSet(brep->Outer,*meshtmp.get(),conv);
	} 
	else if(const IfcFaceBasedSurfaceModel* surf = geo.ToPtr<IfcFaceBasedSurfaceModel>()) {
		BOOST_FOREACH(const IfcConnectedFaceSet& fc, surf->FbsmFaces) {
			ProcessConnectedFaceSet(fc,*meshtmp.get(),conv);
		}
	}  
	else  if(const IfcBooleanResult* boolean = geo.ToPtr<IfcBooleanResult>()) {
		ProcessBoolean(*boolean,*meshtmp.get(),conv);
	}
	else if(geo.ToPtr<IfcBoundingBox>()) {
		// silently skip over bounding boxes
		return false; 
	} 
	else {
		IFCImporter::LogWarn("skipping unknown IfcGeometricRepresentationItem entity, type is " + geo.GetClassName());
		return false;
	}

	meshtmp->RemoveAdjacentDuplicates();
	meshtmp->RemoveDegenerates();

	// Do we just collect openings for a parent element (i.e. a wall)? 
	// In such a case, we generate the polygonal extrusion mesh as usual,
	// but attach it to a TempOpening instance which will later be applied
	// to the wall it pertains to.
	if(conv.collect_openings) {
		conv.collect_openings->push_back(TempOpening(geo.ToPtr<IfcSolidModel>(),IfcVector3(0,0,0),meshtmp));
		return true;
	} 

	if(fix_orientation) {
		meshtmp->FixupFaceOrientation();
	}

	aiMesh* const mesh = meshtmp->ToMesh();
	if(mesh) {
		mesh->mMaterialIndex = ProcessMaterials(geo,conv);
		mesh_indices.push_back(conv.meshes.size());
		conv.meshes.push_back(mesh);
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
void AssignAddedMeshes(std::vector<unsigned int>& mesh_indices,aiNode* nd,
	ConversionData& /*conv*/)
{
	if (!mesh_indices.empty()) {

		// make unique
		std::sort(mesh_indices.begin(),mesh_indices.end());
		std::vector<unsigned int>::iterator it_end = std::unique(mesh_indices.begin(),mesh_indices.end());

		const size_t size = std::distance(mesh_indices.begin(),it_end);

		nd->mNumMeshes = size;
		nd->mMeshes = new unsigned int[nd->mNumMeshes];
		for(unsigned int i = 0; i < nd->mNumMeshes; ++i) {
			nd->mMeshes[i] = mesh_indices[i];
		}
	}
}

// ------------------------------------------------------------------------------------------------
bool TryQueryMeshCache(const IfcRepresentationItem& item, 
	std::vector<unsigned int>& mesh_indices, 
	ConversionData& conv) 
{
	ConversionData::MeshCache::const_iterator it = conv.cached_meshes.find(&item);
	if (it != conv.cached_meshes.end()) {
		std::copy((*it).second.begin(),(*it).second.end(),std::back_inserter(mesh_indices));
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
void PopulateMeshCache(const IfcRepresentationItem& item, 
	const std::vector<unsigned int>& mesh_indices, 
	ConversionData& conv)
{
	conv.cached_meshes[&item] = mesh_indices;
}

// ------------------------------------------------------------------------------------------------
bool ProcessRepresentationItem(const IfcRepresentationItem& item, 
	std::vector<unsigned int>& mesh_indices, 
	ConversionData& conv)
{
	if (!TryQueryMeshCache(item,mesh_indices,conv)) {
		if(ProcessGeometricItem(item,mesh_indices,conv)) {
			if(mesh_indices.size()) {
				PopulateMeshCache(item,mesh_indices,conv);
			}
		}
		else return false;
	}
	return true;
}

#undef to_int64
#undef from_int64
#undef one_vec

} // ! IFC
} // ! Assimp

#endif 
