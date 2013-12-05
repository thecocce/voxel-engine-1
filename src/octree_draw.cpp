/*
    Voxel-Engine - A CPU based sparse octree renderer.
    Copyright (C) 2013  B.J. Conijn <bcmpinc@users.sourceforge.net>

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
#include <algorithm>
//#include <GL/gl.h>

#include "art.h"
#include "events.h"
#include "quadtree.h"
#include "timing.h"
#include "octree.h"

#define static_assert(test, message) typedef char static_assert__##message[(test)?1:-1]

using std::max;
using std::min;

namespace {
    quadtree face;
    octree * root;
    int32_t C;
}

static_assert(quadtree::SIZE >= SCREEN_HEIGHT, quadtree_height_too_small);
static_assert(quadtree::SIZE >= SCREEN_WIDTH,  quadtree_width_too_small);

// Array with x1, x2, y1, y2. Note that x2-x1 = y2-y1.
typedef int32_t v4si __attribute__ ((vector_size (16)));

const v4si quad_permutation[8] = {
    {},{},{},{},
    {0,0,3,3},{1,1,3,3},{0,0,2,2},{1,1,2,2},
};

const v4si nil = {};

/** Returns true if quadtree node is rendered 
 * Function is assumed to be called only if quadtree node is not yet fully rendered.
 * The bounds array is ordered as DELTA.
 * C is the corner that is furthest away from the camera.
 */
static bool traverse(
    const int32_t quadnode, const uint32_t octnode, const uint32_t octcolor, const v4si bounds[8]
){    
    v4si ltz;
    v4si gtz;
    v4si new_bounds[8];
    
    // Recursion
    if (bounds[C][1] - bounds[C][0] <= (2<<26)) {
        // Traverse octree
        octree &s = root[octnode];
        for (int k = 7; k>=0; k--) {
            int i = k^C;
            if (~octnode && s.avgcolor[i]<0) continue;
            ltz = gtz = nil;
            for (int j = 0; j<8; j++) {
                new_bounds[j] = (bounds[i] + bounds[j]);
                ltz |= new_bounds[j]<0;
                gtz |= new_bounds[j]>0;
            }
            if ((ltz[0] & gtz[1] & ltz[2] & gtz[3]) == 0) continue; // frustum occlusion
            if (new_bounds[C][1] - new_bounds[C][0]<=0) continue; // behind camera occlusion
            if (~octnode) {
                if (traverse(quadnode, s.child[i], s.avgcolor[i], new_bounds)) return true;
            } else {
                if (traverse(quadnode, ~0u, octcolor, new_bounds)) return true;
            }
        }
        return false;
    } else {
        // Traverse quadtree 
        assert(quadnode<(int)quadtree::M);
        for (int i = 4; i<8; i++) {
            if (!face.map[quadnode*4+i]) continue;
            ltz = gtz = nil;
            for (int j = 0; j<8; j++) {
                new_bounds[j] = (bounds[j] + __builtin_shuffle(bounds[j],quad_permutation[i])) / 2;
                ltz |= new_bounds[j]<0;
                gtz |= new_bounds[j]>0;
            }
            if ((ltz[0] & gtz[1] & ltz[2] & gtz[3]) == 0) continue; // frustum occlusion
            if (new_bounds[C][1] - new_bounds[C][0]<=0) continue; // behind camera occlusion
            if (quadnode<(int)quadtree::L)
                traverse(quadnode*4+i, octnode, octcolor, new_bounds); 
            else
                face.set_face(quadnode*4+i, octcolor); // Rendering
        }
        if (quadnode>=0) {
            face.compute(quadnode);
            return !face.map[quadnode];
        } else {
            return face.children[0]==0;
        }
    }
}
    
static const int32_t SCENE_DEPTH = 26;
static const double SCENE_SIZE = 1<<SCENE_DEPTH;

static const glm::dvec3 DELTA[8]={
    glm::dvec3(-1,-1,-1) * SCENE_SIZE,
    glm::dvec3(-1,-1, 1) * SCENE_SIZE,
    glm::dvec3(-1, 1,-1) * SCENE_SIZE,
    glm::dvec3(-1, 1, 1) * SCENE_SIZE,
    glm::dvec3( 1,-1,-1) * SCENE_SIZE,
    glm::dvec3( 1,-1, 1) * SCENE_SIZE,
    glm::dvec3( 1, 1,-1) * SCENE_SIZE,
    glm::dvec3( 1, 1, 1) * SCENE_SIZE,
};

static const double quadtree_bounds[] = {
    frustum::left  /(double)frustum::near,
   (frustum::left + (frustum::right -frustum::left)*(double)quadtree::SIZE/SCREEN_WIDTH )/frustum::near,
   (frustum::top  + (frustum::bottom-frustum::top )*(double)quadtree::SIZE/SCREEN_HEIGHT)/frustum::near,
    frustum::top   /(double)frustum::near,
};

/** Render the octree to the OpenGL cubemap texture. 
 */
void octree_draw(octree_file * file) {
    Timer t_global;
    
    double timer_prepare;
    double timer_query;
    double timer_transfer;
    
    root = file->root;
    
    Timer t_prepare;
        
    // Prepare the occlusion quadtree
    face.build(SCREEN_WIDTH, SCREEN_HEIGHT);
    
    timer_prepare = t_prepare.elapsed();
        
    Timer t_query;
    
    // Do the actual rendering of the scene (i.e. execute the query).
    v4si bounds[8];
    int max_z=-(1<<SCENE_DEPTH), max_z_i=0;
    for (int i=0; i<8; i++) {
        // Compute position of octree corners in camera-space
        glm::dvec3 coord = orientation * (DELTA[i] - position);
        v4si b = {
            (int)(coord.z*quadtree_bounds[0] - coord.x),
            (int)(coord.z*quadtree_bounds[1] - coord.x),
            (int)(coord.z*quadtree_bounds[2] - coord.y),
            (int)(coord.z*quadtree_bounds[3] - coord.y),
        };
        bounds[i] = b;
        if (max_z < coord.z) {
            max_z = coord.z;
            max_z_i = i;
        }
    }
    C = max_z_i;
    traverse(-1, 0, 0, bounds);
    
    
    timer_query = t_query.elapsed();

    Timer t_transfer;
    
    // Send the image data to OpenGL.
    // glTexImage2D( cubetargets[i], 0, 4, quadtree::SIZE, quadtree::SIZE, 0, GL_BGRA, GL_UNSIGNED_BYTE, face.face);
    
    timer_transfer = t_transfer.elapsed();
            
    std::printf("%7.2f | Prepare:%4.2f Query:%7.2f Transfer:%5.2f \n", t_global.elapsed(), timer_prepare, timer_query, timer_transfer);
}

// kate: space-indent on; indent-width 4; mixedindent off; indent-mode cstyle; 
