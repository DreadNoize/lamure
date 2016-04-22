// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/xyz/surfel_array_abstract.h>

namespace lamure {
namespace xyz {

surfel_array_abstract::
~surfel_array_abstract() 
{
    try {
        reset();
    }
    catch (...) {}
}


} } // namespace lamure

