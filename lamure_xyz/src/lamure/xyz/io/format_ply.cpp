// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/xyz/io/format_ply.h>

#include <lamure/xyz/io/ply/ply.h>
#include <lamure/xyz/io/ply/ply_parser.h>

#include <boost/filesystem.hpp>
#include <stdexcept>
#include <tuple>
#include <memory>

namespace lamure {
namespace xyz {

void format_ply::
read(const std::string& filename, surfel_callback_funtion callback)
{
    using namespace std::placeholders;
    typedef std::tuple<std::function<void()>, std::function<void()>> FuncTuple;

    const std::string basename = boost::filesystem::path(filename).stem().string();
    auto begin_point = [&](){ current_surfel_ = surfel(); };
    auto end_point   = [&](){ 
        callback(current_surfel_); 
    };

    io::ply::ply_parser ply_parser;

    // define scalar property definition callbacks
    io::ply::ply_parser::scalar_property_definition_callbacks_type scalar_callbacks;

    using namespace io::ply;

    at<io::ply::float32>(scalar_callbacks) = std::bind(&format_ply::scalar_callback<io::ply::float32>, this, _1, _2);
    at<io::ply::uint8>(scalar_callbacks) = std::bind(&format_ply::scalar_callback<io::ply::uint8>, this, _1, _2);

    // set callbacks
    ply_parser.scalar_property_definition_callbacks(scalar_callbacks);
    ply_parser.element_definition_callback(
        [&](const std::string& element_name, std::size_t count) {
            if (element_name == "vertex")
                return FuncTuple(begin_point, end_point);
            else if(element_name == "face")
	      return FuncTuple(nullptr,nullptr);
            else 
                throw std::runtime_error("format_ply::read() : Invalid element_name!");
    });

    ply_parser.info_callback([&](std::size_t line, const std::string& message) {
			       LAMURE_LOG_INFO(basename << " (" << line << "): " << message);
			     });
    ply_parser.warning_callback([&](std::size_t line, const std::string& message) {
				  LAMURE_LOG_WARN(basename << " (" << line << "): " << message);
				});
    ply_parser.error_callback([&](std::size_t line, const std::string& message) {
				LAMURE_LOG_ERROR(basename << " (" << line << "): " << message);
				throw std::runtime_error("Failed to parse PLY file");
			      });

    // convert
    ply_parser.parse(filename);
}

void format_ply::
write(const std::string& filename, buffer_callback_function callback)
{
    throw std::runtime_error("Not implemented yet!");
}

template <>
std::function<void(float)> format_ply::
scalar_callback(const std::string& element_name, const std::string& property_name)
{
    if (element_name == "vertex") {
        if (property_name == "x")
            return [this](float value) { current_surfel_.pos().x_ = value; };
        else if (property_name == "y")
            return [this](float value) { current_surfel_.pos().y_ = value; };
        else if (property_name == "z")
            return [this](float value) { current_surfel_.pos().z_ = value; };
        else if (property_name == "nx")
            return [this](float value) { current_surfel_.normal().x_ = value; };
        else if (property_name == "ny")
            return [this](float value) { current_surfel_.normal().y_ = value; };
        else if (property_name == "nz")
            return [this](float value) { current_surfel_.normal().z_ = value; };
	else
          throw std::runtime_error("format_ply::scalar_callback(): Invalid property_name!");
    }
    else
      throw std::runtime_error("format_ply::scalar_callback(): Invalid element_name!");
}

template <>
std::function<void(uint8_t)> format_ply::
scalar_callback(const std::string& element_name, const std::string& property_name)
{
    if (element_name == "vertex") {
        if (property_name == "red")
            return [this](uint8_t value) { current_surfel_.color().x_ = value; };
        else if (property_name == "green")
            return [this](uint8_t value) { current_surfel_.color().y_ = value; };
        else if (property_name == "blue")
            return [this](uint8_t value) { current_surfel_.color().z_ = value; };
        else if (property_name == "alpha")
	  return [this](uint8_t value) { /*current_surfel_.color().z_ = value*/; };
        else
          throw std::runtime_error("format_ply::scalar_callback(): Invalid property_name!");
    }
    else
      throw std::runtime_error("format_ply::scalar_callback(): Invalid element_name!");
}

} // namespace xyz
} // namespace lamure
