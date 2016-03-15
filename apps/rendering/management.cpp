// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include "utils.h"
#include "management.h"
#include <set>
#include <ctime>
#include <algorithm>
#include <fstream>

management::
management(std::vector<std::string> const& model_filenames,
    std::vector<scm::math::mat4f> const& model_transformations,
    std::set<lamure::model_t> const& visible_set,
    std::set<lamure::model_t> const& invisible_set,
    std::vector<scm::math::mat4d> const& recorded_view_vector,
    std::string const& session_filename)
    :   num_taken_screenshots_(0),
        allow_user_input_(recorded_view_vector.size() == 0),
        screenshot_session_started_(false),
        camera_recording_enabled_(false),
        current_session_filename_(session_filename),
        current_session_file_path_(""),
        num_recorded_camera_positions_(0),
        renderer_(nullptr),
        model_filenames_(model_filenames),
        model_transformations_(model_transformations),
        recorded_view_vector_(recorded_view_vector),
#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
        active_camera_left_(nullptr),
        active_camera_right_(nullptr),
        control_left_(true),
#endif
        test_send_rendered_(true),
        active_camera_(nullptr),
        mouse_state_(),
        num_models_(0),
        num_cameras_(1),
        fast_travel_(false),
        dispatch_(true),
        trigger_one_update_(false),
        reset_matrix_(scm::math::mat4f::identity()),
        reset_diameter_(90.f),
        detail_translation_(scm::math::vec3f::zero()),
        detail_angle_(0.f),
        error_threshold_(LAMURE_DEFAULT_THRESHOLD),
        near_plane_(0.001f),
        far_plane_(100.f),
        importance_(1.f)

{

    lamure::ren::model_database* database = lamure::ren::model_database::get_instance();

    for (const auto& filename : model_filenames_)
    {
        lamure::model_t model_id = database->add_model(filename, std::to_string(num_models_));
        ++num_models_;
    }

    {

        float scene_diameter = far_plane_;
        for (lamure::model_t model_id = 0; model_id < database->num_models(); ++model_id) {
            const auto& bb = database->get_model(model_id)->get_bvh()->get_bounding_boxes()[0];
            scene_diameter = std::max(scm::math::length(bb.max_vertex()-bb.min_vertex()), scene_diameter);
            model_transformations_[model_id] = model_transformations_[model_id] * scm::math::make_translation(database->get_model(model_id)->get_bvh()->get_translation());
        }
        far_plane_ = 2.0f * scene_diameter;

        auto root_bb = database->get_model(0)->get_bvh()->get_bounding_boxes()[0];
        scm::math::vec3 center = model_transformations_[0] * root_bb.center();
        reset_matrix_ = scm::math::make_look_at_matrix(center+scm::math::vec3f(0.f, 0.1f,-0.01f), center, scm::math::vec3f(0.f, 1.f,0.f));
        reset_diameter_ = scm::math::length(root_bb.max_vertex()-root_bb.min_vertex());


        std::cout << "model center : " << center << std::endl;
        std::cout << "model size : " << reset_diameter_ << std::endl;

        for (lamure::view_t cam_id = 0; cam_id < num_cameras_; ++cam_id)
        {
            cameras_.push_back(new lamure::ren::camera(cam_id, reset_matrix_, reset_diameter_, false, false));
        }
    }

#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
    active_camera_left_ = cameras_[0];
    active_camera_right_ = cameras_[0];
#endif
    active_camera_ = cameras_[0];

#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
    renderer_ = new SplitScreenRenderer(model_transformations_);
#endif

#ifndef LAMURE_RENDERING_USE_SPLIT_SCREEN
    renderer_ = new Renderer(model_transformations_, visible_set, invisible_set);
#endif

    PrintInfo();
}

management::
~management()
{
    for (auto& cam : cameras_)
    {
        if (cam != nullptr)
        {
            delete cam;
            cam = nullptr;
        }
    }
    if (renderer_ != nullptr)
    {
        delete renderer_;
        renderer_ = nullptr;
    }


}

bool management::
MainLoop()
{
    lamure::ren::model_database* database = lamure::ren::model_database::get_instance();
    lamure::ren::controller* controller = lamure::ren::controller::get_instance();
    lamure::ren::cut_database* cuts = lamure::ren::cut_database::get_instance();
    lamure::ren::policy* policy = lamure::ren::policy::get_instance();

    controller->reset_system();

    lamure::context_t context_id = controller->deduce_context_id(0);
    
    controller->dispatch(context_id, renderer_->device());


#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
    lamure::view_t view_id_left = controller->deduce_view_id(context_id, active_camera_left_->view_id());
    lamure::view_t view_id_right = controller->deduce_view_id(context_id, active_camera_right_->view_id());
#else

    lamure::view_t view_id = controller->deduce_view_id(context_id, active_camera_->view_id());

#endif    
    

#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
    renderer_->render(context_id, *active_camera_left_, view_id_left, 0, num_recorded_camera_positions_);
    renderer_->render(context_id, *active_camera_right_, view_id_right, 1, num_recorded_camera_positions_);
#else
    renderer_->set_radius_scale(importance_);
    renderer_->render(context_id, *active_camera_, view_id, num_recorded_camera_positions_);
#endif

    renderer_->display_status("Current_camera_Session");

    if (! allow_user_input_) {
        if ( controller->ms_since_last_node_upload() > 3000) {
            if ( screenshot_session_started_ )
                ++num_taken_screenshots_;
                renderer_->take_screenshot("../quality_measurement/session_screenshots/" + current_session_filename_+ "/" , std::to_string(num_taken_screenshots_+1));

            if(! recorded_view_vector_.empty() ) {
                if (! screenshot_session_started_ ) {
                    screenshot_session_started_ = true;
                }
                active_camera_->set_view_matrix(recorded_view_vector_.back());
                recorded_view_vector_.pop_back();
            } else {
                // leave the main loop
                return true;
            }

            controller->reset_ms_since_last_node_upload();
        }
    }

    if (dispatch_ || trigger_one_update_)
    {
        if (trigger_one_update_)
        {
            trigger_one_update_ = false;
        }

        for (lamure::model_t model_id = 0; model_id < num_models_; ++model_id)
        {
            lamure::model_t m_id = controller->deduce_model_id(std::to_string(model_id));

            cuts->send_transform(context_id, m_id, model_transformations_[m_id]);
            cuts->send_threshold(context_id, m_id, error_threshold_ / importance_);
            
            //if (visible_set_.find(model_id) != visible_set_.end())
            if (!test_send_rendered_) {
               if (model_id > num_models_/2) {
                  cuts->send_rendered(context_id, m_id);
               }
            }
            else {
              cuts->send_rendered(context_id, m_id);
            }

            database->get_model(m_id)->set_transform(model_transformations_[m_id]);
        }

        for (auto& cam : cameras_)
        {
            lamure::view_t cam_id = controller->deduce_view_id(context_id, cam->view_id());
            cuts->send_camera(context_id, cam_id, *cam);

            std::vector<scm::math::vec3d> corner_values = cam->get_frustum_corners();
            double top_minus_bottom = scm::math::length((corner_values[2]) - (corner_values[0]));
            float height_divided_by_top_minus_bottom = policy->window_height() / top_minus_bottom;

            cuts->send_height_divided_by_top_minus_bottom(context_id, cam_id, height_divided_by_top_minus_bottom);
        }

        //controller->dispatch(context_id, renderer_->device());


    }

#ifdef LAMURE_CUT_UPDATE_ENABLE_MEASURE_SYSTEM_PERFORMANCE
    system_performance_timer_.stop();
    boost::timer::cpu_times const elapsed_times(system_performance_timer_.elapsed());
    boost::timer::nanosecond_type const elapsed(elapsed_times.system + elapsed_times.user);

    if (elapsed >= boost::timer::nanosecond_type(1.0f * 1000 * 1000 * 1000)) //1 second
    {
       boost::timer::cpu_times const result_elapsed_times(system_result_timer_.elapsed());
       boost::timer::nanosecond_type const result_elapsed(result_elapsed_times.system + result_elapsed_times.user);


       std::cout << "no cut update after " << result_elapsed/(1000 * 1000 * 1000) << " seconds" << std::endl;
       system_performance_timer_.start();
    }
    else
    {
       system_performance_timer_.resume();
    }

#endif

    return false;
}


void management::
update_trackball(int x, int y)
{
    active_camera_->update_trackball(x,y, width_, height_, mouse_state_);
}



void management::
RegisterMousePresses(int button, int state, int x, int y)
{
    if(! allow_user_input_) {
        return;
    }

    switch (button) {
        case GLUT_LEFT_BUTTON:
            {
                mouse_state_.lb_down_ = (state == GLUT_DOWN) ? true : false;
            }break;
        case GLUT_MIDDLE_BUTTON:
            {
                mouse_state_.mb_down_ = (state == GLUT_DOWN) ? true : false;
            }break;
        case GLUT_RIGHT_BUTTON:
            {
                mouse_state_.rb_down_ = (state == GLUT_DOWN) ? true : false;
            }break;
    }



    float trackball_init_x = 2.f * float(x - (width_/2))/float(width_) ;
    float trackball_init_y = 2.f * float(height_ - y - (height_/2))/float(height_);

    active_camera_->update_trackball_mouse_pos(trackball_init_x, trackball_init_y);

}


void management::
dispatchKeyboardInput(unsigned char key)
{

    if(! allow_user_input_) {
        return;
    }

    bool override_center_of_rotation = false;

    switch (key)
    {
    case '+':
        importance_ += 0.1f;
        importance_ = std::min(importance_, 1.0f);
        std::cout << "importance: " << importance_ << std::endl;
        break;

    case '-':
        importance_ -= 0.1f;
        importance_ = std::max(0.1f, importance_);
        std::cout << "importance: " << importance_ << std::endl;
        break;

    case 'y':
        test_send_rendered_ = !test_send_rendered_;
        std::cout << "send rendered: " << test_send_rendered_ << std::endl;
        break;
    case 'w':
        renderer_->toggle_bounding_box_rendering();
        break;
    case 'U':
        renderer_->change_point_size(1.0f);
        break;
    case 'u':
        renderer_->change_point_size(0.1f);
        break;
    case 'J':
        renderer_->change_point_size(-1.0f);
        break;
    case 'j':
        renderer_->change_point_size(-0.1f);
        break;
    case 't':
#ifndef LAMURE_RENDERING_USE_SPLIT_SCREEN
        renderer_->toggle_visible_set();
#endif
        break;

    case '1':
#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
        control_left_ = !control_left_;
        if (control_left_)
            active_camera_ = active_camera_left_;
        else
            active_camera_ = active_camera_right_;
#endif
        break;

    case '2':
#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
        control_left_ = !control_left_;
        if (control_left_)
            active_camera_ = active_camera_left_;
        else
            active_camera_ = active_camera_right_;
#endif
        break;

    case ' ':
#ifdef LAMURE_RENDERING_ENABLE_MULTI_VIEW_TEST
#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
        {
            if (control_left_)
            {
                lamure::view_t current_camera_id = active_camera_left_->view_id();
                active_camera_left_ = cameras_[(++current_camera_id) % num_cameras_];
                active_camera_ = active_camera_left_;
            }
            else
            {
                lamure::view_t current_camera_id = active_camera_right_->view_id();
                active_camera_right_ = cameras_[(++current_camera_id) % num_cameras_];
                active_camera_ = active_camera_right_;
            }

            renderer_->toggle_camera_info(active_camera_left_->view_id(), active_camera_right_->view_id());
        }
#else
        {
            lamure::view_t current_camera_id = active_camera_->view_id();
            active_camera_ = cameras_[(++current_camera_id) % num_cameras_];
            renderer_->toggle_camera_info(active_camera_->view_id());
        }
#endif
#endif
        break;

    case 'd':
        this->Toggledispatching();
        renderer_->toggle_cut_update_info();
        break;

    case 'z':
        {
        lamure::ren::ooc_cache* ooc_cache = lamure::ren::ooc_cache::get_instance();
        ooc_cache->begin_measure();
        }
        break;

    case 'Z':
        {
        lamure::ren::ooc_cache* ooc_cache = lamure::ren::ooc_cache::get_instance();
        ooc_cache->end_measure();
        }
        break;

    case 'e':
        trigger_one_update_ = true;
        break;

    case 'x':
        {
#ifdef LAMURE_RENDERING_ENABLE_MULTI_VIEW_TEST
        cameras_.push_back(new lamure::ren::camera(num_cameras_, reset_matrix_, reset_diameter_, fast_travel_));
        int w = width_;
        int h = height_;
#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
        w/=2;
#endif

        cameras_.back()->set_projection_matrix(30.0f, float(w)/float(h),  near_plane_, far_plane_);

        ++num_cameras_;
#endif
        }
        break;


    case 'f':
        std::cout<<"fast travel: ";
        if(fast_travel_)
        {
            for (auto& cam : cameras_)
            {
                cam->set_dolly_sens_(0.5f);
            }
            std::cout<<"OFF\n\n";
        }
        else
        {
            for (auto& cam : cameras_)
            {
                cam->set_dolly_sens_(20.5f);
            }
            std::cout<<"ON\n\n";
        }

        fast_travel_ = ! fast_travel_;

        break;
#ifndef LAMURE_RENDERING_USE_SPLIT_SCREEN
    
 
    case 'V':
        {
           override_center_of_rotation = true;

        }
#if 0
    case 'v':
        {
            scm::math::mat4f cm = scm::math::inverse(scm::math::mat4f(active_camera_->trackball_matrix()));
            scm::math::vec3f cam_pos = scm::math::vec3f(cm[12], cm[13], cm[14]);
            scm::math::vec3f cam_fwd = -scm::math::normalize(scm::math::vec3f(cm[8], cm[9], cm[10]));
            scm::math::vec3f cam_right = scm::math::normalize(scm::math::vec3f(cm[4], cm[5], cm[6]));
            scm::math::vec3f cam_up = scm::math::normalize(scm::math::vec3f(cm[0], cm[1], cm[2]));

            float max_distance = 100000.0f;
 
            lamure::ren::ray::intersection intersection;
            std::vector<lamure::ren::ray::intersection> dbg_intersections;
            lamure::ren::ray ray(cam_pos, cam_fwd, max_distance);
          
            //sample params for single pick (wysiwg)
            unsigned int max_depth = 255;
            unsigned int surfel_skip = 1;
            float plane_dim = 0.11f;//e.g. 5.0 for valley, 0.05 for seradina rock


#if 1 /*INTERPOLATION PICK*/
                if (ray.intersect(1.0f, cam_up, plane_dim, max_depth, surfel_skip, intersection)) {
#ifdef LAMURE_ENABLE_INFO
                    std::cout << "intersection distance: " << intersection.distance_ << std::endl;
                    std::cout << "intersection position: " << intersection.position_ << std::endl;
#endif
#ifndef LAMURE_RENDERING_USE_SPLIT_SCREEN
                    renderer_->clear_line_begin();
                    renderer_->clear_line_end();
                    scm::math::vec3f intersection_position = cam_pos + cam_fwd * intersection.distance_;
                    renderer_->add_line_begin(intersection_position);
                    renderer_->add_line_end(intersection_position + intersection.normal_ * 5.f);
                    //std::cout << "num debug intersections " << dbg_intersections.size() << std::endl;
                    for (const auto& dbg : dbg_intersections) {
                       renderer_->add_line_begin(dbg.position_);
                       renderer_->add_line_end(dbg.position_ - (cam_fwd) * 5.f);
                       //renderer_->add_line_end(dbg.position_ - dbg.normal_ * 5.f);

                    }
#endif
                }

#elif 0 /*SINGLE PICK SPLAT-BASED*/
            lamure::ren::model_database* database = lamure::ren::model_database::get_instance();
            for (lamure::model_t model_id = 0; model_id < database->num_models(); ++model_id) {
               scm::math::mat4f model_transform = database->get_model(model_id)->transform();
               lamure::ren::ray::Intersection temp;
               if (ray.intersect_model(model_id, model_transform, 1.0f, max_depth, surfel_skip, true, temp)) {
                  intersection = temp;
               }
            }

#elif 0 /*SINGLE PICK BVH-BASED*/

            lamure::ren::model_database* database = lamure::ren::model_database::get_instance();
            for (lamure::model_t model_id = 0; model_id < database->num_models(); ++model_id) {
               scm::math::mat4f model_transform = database->get_model(model_id)->transform();
               lamure::ren::ray::Intersectionbvh temp;
               if (ray.intersect_model_bvh(model_id, model_transform, 1.0f, temp)) {
                  //std::cout << "hit i model id " << model_id << " distance: " << temp.tmin_ << std::endl;
                  intersection.position_ = temp.position_;
                  intersection.normal_ = scm::math::vec3f(0.0f, 1.0f, 0.f);
                  intersection.error_ = 0.f;
                  intersection.distance_ = temp.tmin_;   
               }

            }
            
#else /*DISAMBIGUATION SINGLE PICK BVH-BASED*/

      //compile list of model kdn filenames
      lamure::ren::model_database* database = lamure::ren::model_database::get_instance();
      std::set<std::string> bvh_filenames;
      for (lamure::model_t model_id = 0; model_id < database->num_models(); ++model_id) {
         const std::string& bvh_filename = database->get_model(model_id)->bvh()->filename();
         bvh_filenames.insert(bvh_filename);
      }

      //now test the list of files
      lamure::ren::ray::Intersectionbvh temp;
      if (ray.Intersectbvh(bvh_filenames, 1.0f, temp)) {
         intersection.position_ = temp.position_;
         intersection.normal_ = scm::math::vec3f(0.f, 1.0f, 1.0f);
         intersection.error_ = 0.f;
         intersection.distance_ = temp.tmin_;
         std::cout << temp.bvh_filename_ << std::endl;
      }
    

#endif

#ifdef LAMURE_ENABLE_INFO
               // std::cout << "intersection distance: " << intersection.distance_ << std::endl;
               // std::cout << "intersection position: " << intersection.position_ << std::endl;
#endif

                if (intersection.error_ < std::numeric_limits<float>::max()) {
                  renderer_->clear_line_begin();
                  renderer_->clear_line_end();
                  renderer_->add_line_begin(intersection.position_);
                  renderer_->add_line_end(intersection.position_ + intersection.normal_ * 5.f);
                 // std::cout << "num debug intersections " << dbg_intersections.size() << std::endl;
                  for (const auto& dbg : dbg_intersections) {
                    renderer_->add_line_begin(dbg.position_);
                    //renderer_->add_line_end(dbg.position_ - (cam_fwd) * 5.f);
                    renderer_->add_line_end(dbg.position_ - dbg.normal_ * 5.f);

                  }
                }
           

        

#if 1
             if (override_center_of_rotation) {
               //move center of rotation to intersection
               if (intersection.error_ < std::numeric_limits<float>::max()) {
                   active_camera_->set_trackball_center_of_rotation(intersection.position_);

               }
             }
#endif
        }

        break;

#endif

#endif

    case 'r':
    case 'R':
        toggle_camera_session();
        break;

    case 'a':
        record_next_camera_position();
        break;

    case '0':
        active_camera_->set_trackball_matrix(scm::math::mat4d(reset_matrix_));
        break;

    case '9':
        renderer_->toggle_display_info();
        break;

    case 'k':
        DecreaseErrorThreshold();
        std::cout << "error threshold: " << error_threshold_ << std::endl;
        break;
    case 'i':
        IncreaseErrorThreshold();
        std::cout << "error threshold: " << error_threshold_ << std::endl;
        break;
    }
}

void management::
dispatchResize(int w, int h)
{
    width_ = w;
    height_ = h;

#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
    w/=2;
#endif

    renderer_->reset_viewport(w,h);
    lamure::ren::policy* policy = lamure::ren::policy::get_instance();
    policy->set_window_width(w);
    policy->set_window_height(h);


    for (auto& cam : cameras_) {
        cam->set_projection_matrix(30.0f, float(w)/float(h),  near_plane_, far_plane_);
    }

}




void management::
Toggledispatching()
{
    dispatch_ = ! dispatch_;
};


void management::
DecreaseErrorThreshold() {
    error_threshold_ -= 0.1f;
    if (error_threshold_ < LAMURE_MIN_THRESHOLD)
        error_threshold_ = LAMURE_MIN_THRESHOLD;

}

void management::
IncreaseErrorThreshold() {
    error_threshold_ += 0.1f;
    if (error_threshold_ > LAMURE_MAX_THRESHOLD)
        error_threshold_ = LAMURE_MAX_THRESHOLD;

}

void management::
PrintInfo()
{
    std::cout<<"\n"<<
               "Controls: w - enable/disable bounding box rendering\n"<<
               "\n"<<
               "          U/u - increase point size by 1.0/0.1\n"<<
               "          J/j - decrease point size by 1.0/0.1\n"<<
               "\n"<<
               "          o - switch to circle/ellipse rendering\n"<<
               "          c - toggle normal clamping\n"<<
               "\n"<<
               "          A/a - increase clamping ratio by 0.1/0.01f\n"<<
               "          S/s - decrease clamping ratio by 0.1/0.01f\n"<<
               "\n"<<
               "          d - toggle dispatching\n"<<
               "          e - trigger 1 dispatch, if dispatch is frozen\n"<<
               "          f - toggle fast travel\n"<<
               "          . - toggle fullscreen\n"<<
               "\n"<< 
               "          i - increase error threshold\n" <<
               "          k - decrease error threshold\n" <<
               "\n"<< 
               "          + (NUMPAD) - increase importance\n" <<
               "          - (NUMPAD) - decrease importance\n" <<


#ifdef LAMURE_RENDERING_ENABLE_MULTI_VIEW_TEST
               "\n"<<
               "          x - add camera\n"<<
               "          Space - switch to next camera\n" <<
#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
               "\n"<<
               "          1 - control left screen"<<
               "\n"<<
               "          2 - control right screen"<<
#endif
#endif

               "\n";


}

void management::
toggle_camera_session() {
    camera_recording_enabled_ = !camera_recording_enabled_;
}

void management::
record_next_camera_position() {
    if (camera_recording_enabled_) {
        create_quality_measurement_resources();
    }
}

void management::
create_quality_measurement_resources() {

    std::string base_quality_measurement_path = "../quality_measurement/";
    std::string session_file_prefix = "session_";

    if(! boost::filesystem::exists(base_quality_measurement_path)) {
        std::cout<<"Creating Folder.\n\n";
        boost::filesystem::create_directories(base_quality_measurement_path);
    }

    if( current_session_file_path_.empty() ) {

        boost::filesystem::directory_iterator begin(base_quality_measurement_path), end;
        
        int num_existing_sessions = std::count_if(begin, end,
            [](const boost::filesystem::directory_entry & d) {
                return !boost::filesystem::is_directory(d.path());
            });

        current_session_file_path_ = base_quality_measurement_path+session_file_prefix+std::to_string(num_existing_sessions+1)+".csn";
        

    }

    std::ofstream camera_session_file(current_session_file_path_, std::ios_base::out | std::ios_base::app);
    active_camera_->write_view_matrix(camera_session_file);
    camera_session_file.close();


}
