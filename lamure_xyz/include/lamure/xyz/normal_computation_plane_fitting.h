// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr


#ifndef  NORMAL_COMPUTATION_PLANE_FITTING_H_
#define  NORMAL_COMPUTATION_PLANE_FITTING_H_

#include <lamure/xyz/normal_computation_strategy.h>
#include <lamure/types.h>
#include <vector>

namespace lamure {
namespace xyz {

class bvh;

class normal_computation_plane_fitting: public normal_computation_strategy
{
public:
	explicit normal_computation_plane_fitting(const uint16_t number_of_neighbours) {
			// base class attribute
			number_of_neighbours_ = number_of_neighbours;
		}

	void eigsrt_jacobi(
	    int dim,
	    double* eigenvalues, 
	    double** eigenvectors) const;

	void jacobi_rotation(const mat3d_t& _matrix,
	                     double* eigenvalues,
	                     double** eigenvectors) const;

	vec3f_t  compute_normal(const bvh& tree,
                              const surfel_id_t surfel,
                              std::vector<std::pair<surfel_id_t, real_t>> const& nearest_neighbours) const override;
};

}// namespace pre
}// namespace lamure

#endif // NORMAL_COMPUTATION_PLANE_FITTING_COVAR_H_
