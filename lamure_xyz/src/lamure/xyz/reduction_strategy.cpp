// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/xyz/bvh.h>
#include <lamure/xyz/reduction_strategy.h>

namespace lamure {
namespace xyz {

void reduction_strategy::
interpolate_approx_natural_neighbours(surfel& surfel_to_update,
    					              std::vector<surfel> const& input_surfels,
                                      const bvh& tree,
    						          size_t const num_nearest_neighbours) const {

    auto nn_pairs = tree.get_locally_natural_neighbours(input_surfels, 
    													surfel_to_update.pos(), 
    													num_nearest_neighbours);

    real_t accumulated_nn_weights(0.0);
    vec3r_t accumulated_color(0.0, 0.0, 0.0);
    vec3r_t accumulated_normal(0.0, 0.0, 0.0);

    for (auto const& nn_pair : nn_pairs ) {
	real_t nn_weight = nn_pair.second;
        accumulated_color  += vec3r_t(nn_pair.first.color()) * nn_weight;
        accumulated_normal += nn_pair.first.normal() * nn_weight;

        accumulated_nn_weights += nn_weight;
    }

    if (accumulated_nn_weights != 0.0) {
        accumulated_color /= accumulated_nn_weights;
        accumulated_normal /= accumulated_nn_weights;

        surfel_to_update.color() = accumulated_color;
        surfel_to_update.normal() = accumulated_normal;
     }

};

} //namespace xyz
} //namespace lamure
