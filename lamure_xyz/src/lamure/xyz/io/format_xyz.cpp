// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/xyz/io/format_xyz.h>

#include <iomanip>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <iostream>

#define DEFAULT_PRECISION 15

namespace lamure {
namespace xyz {

void format_xyz::
read(const std::string& filename, surfel_callback_funtion callback)
{
    std::ifstream xyz_file_stream(filename);

    if (!xyz_file_stream.is_open())
        throw std::runtime_error("Unable to open file: " +
                                 filename);

    std::string line;

    real_t pos[3];
    unsigned int color[3];

    xyz_file_stream.seekg (0, std::ios::end);
    std::streampos end_pos = xyz_file_stream.tellg();
    xyz_file_stream.seekg (0, std::ios::beg);
    uint8_t percent_processed = 0;

    while (getline(xyz_file_stream, line)) {

        uint8_t new_percent_processed = (xyz_file_stream.tellg()/float(end_pos)) * 100;
        if (percent_processed + 1 == new_percent_processed) {
            percent_processed = new_percent_processed;
            std::cout << "\r" << (int)percent_processed << "% processed" << std::flush;
        }

        std::stringstream sstream;

        sstream << line;

        sstream >> pos[0];
        sstream >> pos[1];
        sstream >> pos[2];
        sstream >> color[0];
        sstream >> color[1];
        sstream >> color[2];

        callback(surfel(vec3r_t(pos[0], pos[1], pos[2]),
                        vec3b_t(color[0], color[1], color[2])));
    }

    xyz_file_stream.close();
}

void format_xyz::
write(const std::string& filename, buffer_callback_function callback)
{
    std::ofstream xyz_file_stream(filename);

    if (!xyz_file_stream.is_open())
        throw std::runtime_error("Unable to open file: " +
                                 filename);

    surfel_vector buffer;
    size_t count = 0;

    while (true) {
        bool ret = callback(buffer);
        if (!ret)
            break;

        for (const auto s: buffer) {
            xyz_file_stream << std::setprecision(DEFAULT_PRECISION) << s.pos().x_ << " " << s.pos().y_ << " " << s.pos().z_ << " "
                            << int(s.color().x_) << " " << int(s.color().y_) << " " << int(s.color().z_) << "\r\n";
        }

        count += buffer.size();
    }
    xyz_file_stream.close();
    LAMURE_LOG_INFO("Output surfels: " << count);
}

} // namespace xyz
} // namespace lamure
