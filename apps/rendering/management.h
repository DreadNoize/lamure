// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#ifndef REN_APP_MANAGEMENT_H_
#define REN_APP_MANAGEMENT_H_

#include <lamure/utils.h>
#include <lamure/types.h>

#include "split_screen_renderer.h"
#include "renderer.h"
#include <lamure/ren/config.h>

#include <lamure/ren/model_database.h>
#include <lamure/ren/controller.h>
#include <lamure/ren/cut.h>
#include <lamure/ren/cut_update_pool.h>

#include <GL/freeglut.h>

#include <FreeImagePlus.h>

#include <lamure/ren/ray.h>

class management
{
public:
                        management(std::vector<std::string> const& model_filenames,
                            std::vector<scm::math::mat4f> const& model_transformations,
                            const std::set<lamure::model_t>& visible_set,
                            const std::set<lamure::model_t>& invisible_set,
                            std::vector<scm::math::mat4d> const& recorded_view_vector = std::vector<scm::math::mat4d>(),
                            std::string const& session_filename = "");
    virtual             ~management();

                        management(const management&) = delete;
                        management& operator=(const management&) = delete;

    bool                MainLoop();
    void                update_trackball(int x, int y);
    void                RegisterMousePresses(int button, int state, int x, int y);
    void                dispatchKeyboardInput(unsigned char key);
    void                dispatchResize(int w, int h);

    void                PrintInfo();
    void                SetSceneName();

    float               error_threshold_;
    void                IncreaseErrorThreshold();
    void                DecreaseErrorThreshold();

protected:

    void                Toggledispatching();
    void                toggle_camera_session();
    void                record_next_camera_position();
    void                create_quality_measurement_resources();

private:
    
    size_t              num_taken_screenshots_;
    bool const          allow_user_input_;
    bool                screenshot_session_started_;
    bool                camera_recording_enabled_;
    std::string const   current_session_filename_;
    std::string         current_session_file_path_;
    unsigned            num_recorded_camera_positions_;

#ifdef LAMURE_RENDERING_USE_SPLIT_SCREEN
    split_screen_renderer* renderer_;
    lamure::ren::camera*   active_camera_left_;
    lamure::ren::camera*   active_camera_right_;
    bool                control_left_;
#endif

#ifndef LAMURE_RENDERING_USE_SPLIT_SCREEN
    Renderer* renderer_;
#endif

    lamure::ren::camera*   active_camera_;

    int32_t             width_;
    int32_t             height_;

    float importance_;

    bool test_send_rendered_;

    lamure::view_t         num_cameras_;
    std::vector<lamure::ren::camera*> cameras_;

    lamure::ren::camera::mouse_state mouse_state_;

    bool                fast_travel_;

    bool                dispatch_;
    bool                trigger_one_update_;

    scm::math::mat4f    reset_matrix_;
    float               reset_diameter_;

    lamure::model_t     num_models_;

    scm::math::vec3f    detail_translation_;
    float               detail_angle_;
    float               near_plane_;
    float               far_plane_;

    std::vector<scm::math::mat4f> model_transformations_;
    std::vector<std::string> model_filenames_;

    std::vector<scm::math::mat4d> recorded_view_vector_; 

#ifdef LAMURE_CUT_UPDATE_ENABLE_MEASURE_SYSTEM_PERFORMANCE
    boost::timer::cpu_timer system_performance_timer_;
    boost::timer::cpu_timer system_result_timer_;
#endif
};


#endif // REN_MANAGEMENT_H_

