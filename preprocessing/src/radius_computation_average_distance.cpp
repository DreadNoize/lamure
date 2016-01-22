// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <iostream>

#include <lamure/pre/bvh.h>
#include <lamure/pre/radius_computation_average_distance.h>

namespace lamure {
namespace pre{
	

real radius_computation_average_distance::
compute_radius(const bvh& tree,
			   const surfel_id_t target_surfel) const {
	
	const uint16_t num = number_of_neighbours_;

	// find nearest neighbours
	std::vector<std::pair<surfel_id_t, real>> const& neighbours = tree.get_nearest_neighbours(target_surfel, num);
    
    real avg_distance = 0.0;

    // compute radius
    for (auto const& neighbour : neighbours) {
    	avg_distance += sqrt(neighbour.second);
    }

    avg_distance /= neighbours.size();

	return avg_distance;
	};

}// namespace pre
}// namespace lamure