// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <GL/glew.h>
#include <GL/freeglut.h>

#include "utils.h"
#include <lamure/lod/camera.h>

#include <memory>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>

#include "management.h"

#include <GL/freeglut.h>

#include <lamure/types.h>
#include <lamure/lod/config.h>
#include <lamure/lod/model_database.h>
#include <lamure/lod/cut_database.h>
#include <lamure/lod/dataset.h>
#include <lamure/lod/policy.h>

#include <lamure/types.h>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>


void initialize_glut();

void glut_display();
void glut_resize(int w, int h);
void glut_mousefunc(int button, int state, int x, int y);
void glut_mousemotion(int x, int y);
void glut_idle();
void glut_keyboard(unsigned char key, int x, int y);
void glut_keyboard_release(unsigned char key, int x, int y);
void glut_timer(int value);
void glut_close();

std::vector<lamure::mat4r_t> const parse_camera_session_file( std::string const& session_file_path ) {

  std::ifstream camera_session_file(session_file_path);

  std::string view_matrix_as_string;

  std::vector<lamure::mat4r_t> read_view_matrices;

  while( std::getline(camera_session_file, view_matrix_as_string) ) {
      lamure::mat4r_t curr_view_matrix;
      std::istringstream view_matrix_as_strstream(view_matrix_as_string);

      for(int matrix_element_idx = 0; 
              matrix_element_idx < 16; 
              ++matrix_element_idx) {
          view_matrix_as_strstream >> curr_view_matrix[matrix_element_idx];
      }

      read_view_matrices.push_back(curr_view_matrix);

  }

  //reverse vector in order to pop_back elements in correct order
  std::reverse(read_view_matrices.begin(), read_view_matrices.end());
  return read_view_matrices;
}

void initialize_glut(int argc, char** argv, uint32_t width, uint32_t height)
{
  glutInit(&argc, argv);
  glutInitContextVersion(3, 1);
  glutInitContextProfile(GLUT_CORE_PROFILE);

  glutSetOption(
    GLUT_ACTION_ON_WINDOW_CLOSE,
    GLUT_ACTION_GLUTMAINLOOP_RETURNS
  );

  glutInitDisplayMode(GLUT_DOUBLE | GLUT_DEPTH | GLUT_RGBA | GLUT_ALPHA | GLUT_MULTISAMPLE);

  glutInitWindowPosition(400,300);
  glutInitWindowSize(width, height);

  int wh1 = glutCreateWindow("Point Renderer");

  glutSetWindow(wh1);

  glutReshapeFunc(glut_resize);
  glutDisplayFunc(glut_display);
  glutKeyboardFunc(glut_keyboard);
  glutKeyboardUpFunc(glut_keyboard_release);
  glutMouseFunc(glut_mousefunc);
  glutMotionFunc(glut_mousemotion);
  glutIdleFunc(glut_idle);
  glewExperimental = GL_TRUE;
  auto err = glewInit();
  
  
  if (GLEW_OK != err) {
    fprintf(stderr, "Error: %s\n", glewGetErrorString(err));
  }
    
}

management* management_ = nullptr;
bool quality_measurement_mode_enabled_ = false;

int main(int argc, char** argv)
{
  namespace po = boost::program_options;
  namespace fs = boost::filesystem;

  const std::string exec_name = (argc > 0) ? fs::basename(argv[0]) : "";

  putenv((char *)"__GL_SYNC_TO_VBLANK=0");

  int window_width;
  int window_height;
  unsigned int main_memory_budget;
  unsigned int video_memory_budget ;
  unsigned int max_upload_budget;

  std::string resource_file_path = "";
  std::string measurement_file_path = "";

  po::options_description desc("Usage: " + exec_name + " [OPTION]... INPUT\n\n"
                             "Allowed Options");
  desc.add_options()
    ("help", "print help message")
    ("width,w", po::value<int>(&window_width)->default_value(1920), "specify window width (default=1920)")
    ("height,h", po::value<int>(&window_height)->default_value(1080), "specify window height (default=1080)")
    ("resource-file,f", po::value<std::string>(&resource_file_path), "specify resource input-file")
    ("vram,v", po::value<unsigned>(&video_memory_budget)->default_value(4096), "specify graphics memory budget in MB (default=2048)")
    ("mem,m", po::value<unsigned>(&main_memory_budget)->default_value(22000), "specify main memory budget in MB (default=4096)")
    ("upload,u", po::value<unsigned>(&max_upload_budget)->default_value(100), "specify maximum video memory upload budget per frame in MB (default=64)")
    ("measurement-file", po::value<std::string>(&measurement_file_path)->default_value(""), "specify camera session for quality measurement_file (default = \"\")");
    ;

  po::positional_options_description p;
  po::variables_map vm;

  try {    
    auto parsed_options = po::command_line_parser(argc, argv).options(desc).allow_unregistered().run();
    po::store(parsed_options, vm);
    po::notify(vm);

    std::vector<std::string> to_pass_further = po::collect_unrecognized(parsed_options.options, po::include_positional);
    bool no_input = !vm.count("input") && to_pass_further.empty();

    if (resource_file_path == "") {
      if (vm.count("help") || no_input)
      {
        std::cout << desc;
        return 0;
      }
    }

    // no explicit input -> use unknown options
    if (!vm.count("input") && resource_file_path == "") 
    {
      resource_file_path = "auto_generated.rsc";
      std::fstream ofstr(resource_file_path, std::ios::out);
      if (ofstr.good()) 
      {
        for (auto argument : to_pass_further)
        {
          ofstr << argument << std::endl;
        }
      } else {
        throw std::runtime_error("Cannot open file");
      }
      ofstr.close();
    }


  } catch (std::exception& e) {
    std::cout << "Warning: No input file specified. \n" << desc;
    return 0;
  }

  // set min and max
  window_width        = std::max(std::min(window_width, 4096), 1);
  window_height       = std::max(std::min(window_height, 2160), 1);
  main_memory_budget  = std::max(int(main_memory_budget), 1);
  video_memory_budget = std::max(int(video_memory_budget), 1);
  max_upload_budget   = std::max(int(max_upload_budget), 64);

  initialize_glut(argc, argv, window_width, window_height);

  std::pair< std::vector<std::string>, std::vector<lamure::mat4r_t> > model_attributes;
  std::set<lamure::model_t> visible_set;
  std::set<lamure::model_t> invisible_set;
  model_attributes = read_model_string(resource_file_path, &visible_set, &invisible_set);

  //std::string scene_name;
  //create_scene_name_from_vector(model_attributes.first, scene_name);
  std::vector<lamure::mat4r_t> & model_transformations = model_attributes.second;
  std::vector<std::string> const& model_filenames = model_attributes.first;

  lamure::lod::policy* policy = lamure::lod::policy::get_instance();
  policy->set_max_upload_budget_in_mb(max_upload_budget); //8
  policy->set_render_budget_in_mb(video_memory_budget); //2048
  policy->set_out_of_core_budget_in_mb(main_memory_budget); //4096, 8192
  policy->set_window_width(window_width);
  policy->set_window_height(window_height);

  lamure::lod::model_database* database = lamure::lod::model_database::get_instance();

  std::vector<lamure::mat4r_t> parsed_views = std::vector<lamure::math::mat4d_t>();

  std::string measurement_filename = "";

  snapshot_session_descriptor measurement_descriptor;

  if( ! measurement_file_path.empty() ) {
    measurement_descriptor.recorded_view_vector_ = parse_camera_session_file(measurement_file_path);
    measurement_descriptor.snapshot_resolution_ = lamure::math::vector_t<uint32_t, 2>(window_width, window_height);
    size_t last_dot_in_filename_pos = measurement_file_path.find_last_of('.');
    size_t first_slash_before_filename_pos = measurement_file_path.find_last_of("/\\", last_dot_in_filename_pos);

    measurement_descriptor.session_filename_ = measurement_file_path.substr(first_slash_before_filename_pos+1, last_dot_in_filename_pos);
    quality_measurement_mode_enabled_ = true;
    measurement_descriptor.snapshot_session_enabled_ = true;
    glutFullScreenToggle();
  }

  management_ = new management(model_filenames, model_transformations, visible_set, invisible_set, measurement_descriptor);

  glutMainLoop();

  return 0;
}



void glut_display()
{
  bool signaled_shutdown = false;
  if (management_ != nullptr)
  {
      signaled_shutdown = management_->MainLoop(); 

      glutSwapBuffers();
  }

  if(signaled_shutdown) {
      glutExit();
      exit(0);
  }
}


void glut_resize(int w, int h)
{
  if (management_ != nullptr)
  {
      management_->dispatchResize(w, h);
  }
}

void glut_mousefunc(int button, int state, int x, int y)
{
  if (management_ != nullptr)
  {
      management_->RegisterMousePresses(button, state, x, y);
  }
}

void glut_mousemotion(int x, int y)
{
  if (management_ != nullptr)
  {
      management_->update_trackball(x, y);
  }
}

void glut_idle()
{
  glutPostRedisplay();
}

void Cleanup()
{

  if (management_ != nullptr)
  {
    delete management_;
    management_ = nullptr;
    delete lamure::lod::cut_database::get_instance();
    delete lamure::lod::controller::get_instance();
    delete lamure::lod::model_database::get_instance();
    delete lamure::lod::policy::get_instance();
    delete lamure::lod::ooc_cache::get_instance();
  }

}

void glut_close()
{

  if (management_ != nullptr)
  {
    delete management_;
    management_ = nullptr;
    delete lamure::lod::cut_database::get_instance();
    delete lamure::lod::controller::get_instance();
    delete lamure::lod::model_database::get_instance();
    delete lamure::lod::policy::get_instance();
    delete lamure::lod::ooc_cache::get_instance();
  }
}


void glut_keyboard(unsigned char key, int x, int y)
{
  switch(key)
  {
    case 27:
        //Cleanup();
        glutExit();
        exit(0);
        break;
    case '.':
        if(!quality_measurement_mode_enabled_)
            glutFullScreenToggle();
        break;

    default:
        if (management_ != nullptr)
        {
            management_->dispatchKeyboardInput(key);
        }
        break;

  }
}

void glut_keyboard_release(unsigned char key, int x, int y)
{

}



