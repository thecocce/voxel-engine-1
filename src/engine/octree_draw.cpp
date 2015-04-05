/*
    Voxel-Engine - A CPU based sparse octree renderer.
    Copyright (C) 2013,2014,2015  B.J. Conijn <bcmpinc@users.sourceforge.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstdio>
#include <cassert>
#include <algorithm>
#include <xmmintrin.h>

#include "quadtree.h"
#include "timing.h"
#include "octree.h"

#define static_assert(test, message) typedef char static_assert__##message[(test)?1:-1]

using std::max;
using std::min;

static quadtree face;
static octree * root;
static int C; //< The corner that is furthest away from the camera.
static int count, count_oct, count_quad;

// Array with x1, x2, y1, y2. Note that x2-x1 = y2-y1.
typedef int32_t v4si __attribute__ ((vector_size (16)));

const v4si quad_permutation[8] = {
    {},{},{},{},
    {0,0,3,3},{1,1,3,3},{0,0,2,2},{1,1,2,2},
};

static const int32_t SCENE_DEPTH = 26;

static const int DX=4, DY=2, DZ=1;
static const __m128i DELTA[8]={
    _mm_set_epi32(0,-1,-1,-1),
    _mm_set_epi32(0, 1,-1,-1),
    _mm_set_epi32(0,-1, 1,-1),
    _mm_set_epi32(0, 1, 1,-1),
    _mm_set_epi32(0,-1,-1, 1),
    _mm_set_epi32(0, 1,-1, 1),
    _mm_set_epi32(0,-1, 1, 1),
    _mm_set_epi32(0, 1, 1, 1),
};

const v4si nil = {};

static inline int movemask_epi32(__m128i v) {
    return _mm_movemask_ps(_mm_castsi128_ps(v));
}

/** Returns true if quadtree node is rendered 
 * Function is assumed to be called only if quadtree node is not yet fully rendered.
 * The bound parameter is the quadnode projected on the plane containing the furthest corner of the octree node.
 * The dx,dy,dz values represent how this projection changes when traversing an edge to one of the other corners.
 * Furthermore, pos is the location of the center of the octree node, relative to the viewer in octree space.
 * For leaf nodes (and their 'childs') octnode will be a color and >= 0xff000000u.
 */
static bool traverse(
    const int32_t quadnode, const uint32_t octnode,
    const v4si bound, const v4si dx, const v4si dy, const v4si dz, const v4si dltz, const v4si dgtz,
    const __m128i pos, const int depth
){    
    count++;
    // Recursion
    int va = _mm_cvtsi128_si32((__m128i)bound);
    int vb = _mm_extract_epi32((__m128i)bound, 1);
    if (depth>=0 && vb - va < 2<<SCENE_DEPTH) {
        __m128i octant = _mm_cmplt_epi32(pos, _mm_setzero_si128());
        int furthest = movemask_epi32(_mm_shuffle_epi32(octant, 0xc6));
        if (octnode < 0xff000000) {
            // Traverse octree
            for (int k = 0; k<8; k++) {
                int i = furthest^k;
                if (!root[octnode].has_index(i)) continue;
                int j = root[octnode].position(i);
                v4si new_bound = bound<<1;
                if ((C^i)&DX) new_bound += dx;
                if ((C^i)&DY) new_bound += dy;
                if ((C^i)&DZ) new_bound += dz;
                v4si ltz = (new_bound - dltz)<0;
                v4si gtz = (new_bound - dgtz)>0;
                if ((ltz[0] & gtz[1] & ltz[2] & gtz[3]) == 0) continue; // frustum occlusion
                count_oct++;
                if (traverse(quadnode, root[octnode].child[j], new_bound, dx, dy, dz, dltz, dgtz, _mm_add_epi32(pos, _mm_slli_epi32(DELTA[i], depth)), depth-1)) return true;
            }
        } else {
            // Duplicate leaf node
            for (int k = 0; k<7; k++) {
                int i = furthest^k;
                v4si new_bound = bound<<1;
                if ((C^i)&DX) new_bound += dx;
                if ((C^i)&DY) new_bound += dy;
                if ((C^i)&DZ) new_bound += dz;
                v4si ltz = (new_bound - dltz)<0;
                v4si gtz = (new_bound - dgtz)>0;
                if ((ltz[0] & gtz[1] & ltz[2] & gtz[3]) == 0) continue; // frustum occlusion
                count_oct++;
                if (traverse(quadnode, octnode, new_bound, dx, dy, dz, dltz, dgtz, _mm_add_epi32(pos, _mm_slli_epi32(DELTA[i], depth)), depth-1)) return true;
            }
        }
        return false;
    } else {
        // Traverse quadtree 
        int mask = face.children[quadnode];
        for (int i = 4; i<8; i++) {
            if (!(mask&(1<<i))) continue;
            v4si new_bound = (bound + __builtin_shuffle(bound,quad_permutation[i])) >> 1;
            v4si new_dx = (dx + __builtin_shuffle(dx,quad_permutation[i])) >> 1;
            v4si new_dy = (dy + __builtin_shuffle(dy,quad_permutation[i])) >> 1;
            v4si new_dz = (dz + __builtin_shuffle(dz,quad_permutation[i])) >> 1;
            v4si new_dltz = (new_dx<0)*new_dx + (new_dy<0)*new_dy + (new_dz<0)*new_dz;
            v4si new_dgtz = (new_dx>0)*new_dx + (new_dy>0)*new_dy + (new_dz>0)*new_dz;
            v4si ltz = (new_bound - new_dltz)<0;
            v4si gtz = (new_bound - new_dgtz)>0;
            if ((ltz[0] & gtz[1] & ltz[2] & gtz[3]) == 0) continue; // frustum occlusion
            if (quadnode<quadtree::M) {
                bool r = traverse(quadnode*4+i, octnode, new_bound, new_dx, new_dy, new_dz, new_dltz, new_dgtz, pos, depth);
                mask &= ~(r<<i); 
                count_quad++;
            } else if (octnode < 0xff000000u) {
                face.draw(quadnode*4+i, root[octnode].avgcolor); // Rendering
                mask &= ~(1<<i);
            } else {
                face.draw(quadnode*4+i, octnode); // Rendering
                mask &= ~(1<<i);
            }
        }
        face.children[quadnode] = mask;
        return mask == 0;
    }
}

/** Render the octree to the OpenGL cubemap texture. 
 */
void octree_draw(octree_file* file, surface surf, view_pane view, glm::dvec3 position, glm::dmat3 orientation) {
    Timer t_global;
    
    double timer_prepare;
    double timer_query;
    double timer_transfer;
    
    assert(quadtree::SIZE >= surf.width);
    assert(quadtree::SIZE >= surf.height);
    double quadtree_bounds[] = {
        view.left,
       (view.left + (view.right -view.left)*(double)quadtree::SIZE/surf.width ),
       (view.top  + (view.bottom-view.top )*(double)quadtree::SIZE/surf.height),
        view.top,
    };

    root = file->root;
    face.surf = surf;
    
    Timer t_prepare;
        
    // Prepare the occlusion quadtree
    face.build();
    
    timer_prepare = t_prepare.elapsed();

    Timer t_query;
    count_oct = count_quad = count = 0;
    // Do the actual rendering of the scene (i.e. execute the query).
    v4si bounds[8];
    int max_z=-1<<31;
    for (int i=0; i<8; i++) {
        // Compute position of octree corners in camera-space
        v4si vertex = (v4si)DELTA[i]<<SCENE_DEPTH;
        glm::dvec3 coord = orientation * (glm::dvec3(vertex[0], vertex[1], vertex[2]) - position);
        v4si b = {
            (int)(coord.z*quadtree_bounds[0] - coord.x),
            (int)(coord.z*quadtree_bounds[1] - coord.x),
            (int)(coord.z*quadtree_bounds[2] - coord.y),
            (int)(coord.z*quadtree_bounds[3] - coord.y),
        };
        bounds[i] = b;
        if (max_z < coord.z) {
            max_z = coord.z;
            C = i;
        }
    }
    __m128i pos = _mm_set_epi32(0, -(int)position.z, -(int)position.y, -(int)position.x);
    v4si new_dx = (bounds[C^DX]-bounds[C]);
    v4si new_dy = (bounds[C^DY]-bounds[C]);
    v4si new_dz = (bounds[C^DZ]-bounds[C]);
    v4si new_dltz = (new_dx<0)*new_dx + (new_dy<0)*new_dy + (new_dz<0)*new_dz;
    v4si new_dgtz = (new_dx>0)*new_dx + (new_dy>0)*new_dy + (new_dz>0)*new_dz;
    traverse(-1, 0, bounds[C], new_dx, new_dy, new_dz, new_dltz, new_dgtz, pos, SCENE_DEPTH-1);
    
    
    timer_query = t_query.elapsed();

    Timer t_transfer;
    
    // Send the image data to OpenGL.
    // glTexImage2D( cubetargets[i], 0, 4, quadtree::SIZE, quadtree::SIZE, 0, GL_BGRA, GL_UNSIGNED_BYTE, face.face);
    
    timer_transfer = t_transfer.elapsed();
            
    std::printf("%7.2f | Prepare:%4.2f Query:%7.2f Transfer:%5.2f | Count:%10d Oct:%10d Quad:%10d\n", t_global.elapsed(), timer_prepare, timer_query, timer_transfer, count, count_oct, count_quad);
}

// kate: space-indent on; indent-width 4; mixedindent off; indent-mode cstyle; 
