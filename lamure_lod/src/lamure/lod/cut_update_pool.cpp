// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/lod/cut_update_pool.h>
#include <cstring>

namespace lamure {
namespace lod {

cut_update_pool::
cut_update_pool(const context_t context_id,
  const node_t upload_budget_in_nodes,
  const node_t render_budget_in_nodes)
  : context_id_(context_id),
    locked_(false),
    num_threads_(LAMURE_CUT_UPDATE_NUM_CUT_UPDATE_THREADS),
    shutdown_(false),
    current_gpu_storage_A_(nullptr),
    current_gpu_storage_B_(nullptr),
    current_gpu_storage_(nullptr),
    current_gpu_buffer_(cut_database_record::temporary_buffer::BUFFER_A),
    upload_budget_in_nodes_(upload_budget_in_nodes),
    render_budget_in_nodes_(render_budget_in_nodes),
#ifdef LAMURE_CUT_UPDATE_ENABLE_MODEL_TIMEOUT
    cut_update_counter_(0),
#endif
    master_dispatched_(false) {

    initialize();

    for (uint32_t i = 0; i < num_threads_; ++i) {
        threads_.push_back(std::thread(&cut_update_pool::run, this));
    }
}

cut_update_pool::
~cut_update_pool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        shutdown_ = true;
        semaphore_.shutdown();
        master_semaphore_.shutdown();
    }

    for (auto& thread : threads_) {
        if (thread.joinable())
            thread.join();
    }
    threads_.clear();

    shutdown();
}

void cut_update_pool::
initialize() {
    model_database* database = model_database::get_instance();
    policy* policy = policy::get_instance();

    ASSERT(policy->render_budget_in_mb() > 0);
    ASSERT(policy->out_of_core_budget_in_mb() > 0);

    out_of_core_budget_in_nodes_ =
        (policy->out_of_core_budget_in_mb()*1024*1024) / database->get_slot_size();;

    index_ = new cut_update_index();
    index_->update_policy(0);
    gpu_cache_ = new gpu_cache(render_budget_in_nodes_);

    semaphore_.set_max_signal_count(1);
    semaphore_.set_min_signal_count(1);

#ifdef LAMURE_ENABLE_INFO
    std::cout << "lamure: num models: " << index_->num_models() << std::endl;
    std::cout << "lamure: ooc-cache size (MB): " << policy->out_of_core_budget_in_mb() << std::endl;
#endif

}

void cut_update_pool::
shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (gpu_cache_ != nullptr) {
        delete gpu_cache_;
        gpu_cache_ = nullptr;
    }

    if (index_ != nullptr) {
        delete index_;
        index_ = nullptr;
    }

    current_gpu_storage_A_ = nullptr;
    current_gpu_storage_B_ = nullptr;

}

bool cut_update_pool::
is_shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
}

const bool cut_update_pool::
is_running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return master_dispatched_;
}

void cut_update_pool::
dispatch_cut_update(char* current_gpu_storage_A, char* current_gpu_storage_B) {
    std::lock_guard<std::mutex> lock(mutex_);

    ASSERT(current_gpu_storage_A != nullptr);
    ASSERT(current_gpu_storage_B != nullptr);

#ifdef LAMURE_CUT_UPDATE_ENABLE_REPEAT_MODE
    last_frame_elapsed_ = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::high_resolution_clock::now() - last_frame_time_);
    last_frame_time_ = std::chrono::high_resolution_clock::now();
#endif

    if (!master_dispatched_) {
        current_gpu_storage_A_ = current_gpu_storage_A;
        current_gpu_storage_B_ = current_gpu_storage_B;

        master_dispatched_ = true;

        job_queue_.push_job(
            cut_update_queue::job(
                cut_update_queue::task_t::CUT_MASTER_TASK, invalid_view_t, invalid_model_t));

        semaphore_.signal(1);

    }

}


void cut_update_pool::
run() {
    while (true) {
        semaphore_.wait();

        if (is_shutdown()) break;

        //dequeue job
        cut_update_queue::job job = job_queue_.pop_front_job();

        if (job.task_ != cut_update_queue::task_t::CUT_INVALID_TASK) {
            switch (job.task_) {
                case cut_update_queue::task_t::CUT_MASTER_TASK:
                    cut_master();
                    break;

                case cut_update_queue::task_t::CUT_ANALYSIS_TASK:
                    cut_analysis(job.view_id_, job.model_id_);
                    break;

                case cut_update_queue::task_t::CUT_UPDATE_TASK:
                    cut_update();
                    break;

                default: break;

            }
        }
    }

}

const bool cut_update_pool::
prepare() {
    cut_database* cut_database = cut_database::get_instance();

    cut_database->receive_cameras(context_id_, user_cameras_);
    cut_database->receive_height_divided_by_top_minus_bottoms(context_id_, height_divided_by_top_minus_bottoms_);
    cut_database->receive_transforms(context_id_, model_transforms_);
    cut_database->receive_thresholds(context_id_, model_thresholds_);

    transfer_list_.clear();
    render_list_.clear();

    gpu_cache_->reset_transfer_list();
    gpu_cache_->set_transfer_budget(upload_budget_in_nodes_);
    gpu_cache_->set_transfer_slots_written(0);

    index_->update_policy(user_cameras_.size());

    //clamp threshold
    for (auto& threshold_it : model_thresholds_) {
        float64_t& threshold = threshold_it.second;
        threshold = threshold < LAMURE_MIN_THRESHOLD ? LAMURE_MIN_THRESHOLD : threshold;
        threshold = threshold > LAMURE_MAX_THRESHOLD ? LAMURE_MAX_THRESHOLD : threshold;

    }

#ifdef LAMURE_CUT_UPDATE_ENABLE_MODEL_TIMEOUT
    ++cut_update_counter_;
    std::set<model_t> rendered_model_ids;
    cut_database->receive_rendered(context_id_, rendered_model_ids);

    for (const auto& model_id : rendered_model_ids) {
        model_freshness_[model_id] = cut_update_counter_;

    }
#endif


    //make sure roots are resident and aquired

    bool all_roots_resident = true;

    for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
        for (view_t view_id = 0; view_id < index_->num_views(); ++view_id) {
            if (index_->get_current_cut(view_id, model_id).empty()) {
                all_roots_resident = false;
            }
        }
    }

    if (!all_roots_resident) {
        all_roots_resident = true;

        ooc_cache* ooc_cache = ooc_cache::get_instance();

        ooc_cache->lock();
        ooc_cache->refresh();

        for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
            if (!ooc_cache->is_node_resident(model_id, 0)) {
                ooc_cache->register_node(model_id, 0, 100);
                all_roots_resident = false;
            }
        }

        ooc_cache->unlock();

        if (!all_roots_resident) {
            return false;
        }
        else {
            ooc_cache->lock();
            gpu_cache_->lock();
            for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
                if (!gpu_cache_->is_node_resident(model_id, 0)) {
                    gpu_cache_->register_node(model_id, 0);
                }

                for (view_t view_id = 0; view_id < index_->num_views(); ++view_id) {
                    if (index_->get_current_cut(view_id, model_id).empty()) {
                        ASSERT(ooc_cache->is_node_resident(model_id, 0));
                        ASSERT(gpu_cache_->is_node_resident(model_id, 0));

                        ooc_cache->aquire_node(context_id_, view_id, model_id, 0);
                        gpu_cache_->aquire_node(context_id_, view_id, model_id, 0);

                        index_->push_action(cut_update_index::action(cut_update_index::queue_t::KEEP, view_id, model_id, 0, 10000.f), false);

                    }
                }
            }

            gpu_cache_->unlock();
            ooc_cache->unlock();
        }
    }

    return true;

}

void cut_update_pool::
cut_master() {
    if (!prepare()) {
        std::lock_guard<std::mutex> lock(mutex_);
        master_dispatched_ = false;
        return;
    }

#ifdef LAMURE_CUT_UPDATE_ENABLE_SHOW_GPU_CACHE_USAGE
    std::cout << "lamure: free slots gpu : " << gpu_cache_->num_free_slots() << "\t\t( " << gpu_cache_->num_slots()-gpu_cache_->num_free_slots() << " occupied)" << std::endl;
#endif

#ifdef LAMURE_CUT_UPDATE_ENABLE_SHOW_OOC_CACHE_USAGE
    std::cout << "lamure: free slots cpu: " << ooc_cache_->num_free_slots() << "\t\t( " << ooc_cache_->num_slots()-ooc_cache_->num_free_slots() << " occupied)" << std::endl << std::endl;
#endif

    //swap and use temporary buffer
    if (current_gpu_buffer_ == cut_database_record::temporary_buffer::BUFFER_A) {
        current_gpu_buffer_ = cut_database_record::temporary_buffer::BUFFER_B;
        current_gpu_storage_ = current_gpu_storage_B_;
    }
    else {
        current_gpu_buffer_ = cut_database_record::temporary_buffer::BUFFER_A;
        current_gpu_storage_ = current_gpu_storage_A_;
    }

#ifdef LAMURE_CUT_UPDATE_ENABLE_REPEAT_MODE
    std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    uint32_t num_cut_updates = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
            if ((num_cut_updates > 0 && time_span >= last_frame_elapsed_*0.5) || num_cut_updates >= LAMURE_CUT_UPDATE_MAX_NUM_UPDATES_PER_FRAME) {
                break;
            }
        }

        ++num_cut_updates;

#endif


    //swap cut index
    index_->swap_cuts();

    ASSERT(semaphore_.num_signals() == 0);
    ASSERT(master_semaphore_.num_signals() == 0);

    //re-configure semaphores
    master_semaphore_.lock();
    master_semaphore_.set_max_signal_count(index_->num_models() * index_->num_views());
    master_semaphore_.set_min_signal_count(index_->num_models() * index_->num_views());
    master_semaphore_.unlock();

    semaphore_.lock();
    semaphore_.set_max_signal_count(index_->num_models() * index_->num_views());
    semaphore_.set_min_signal_count(1);
    semaphore_.unlock();

    //launch slaves
    for (view_t view_id = 0; view_id < index_->num_views(); ++view_id) {
        for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
            job_queue_.push_job(cut_update_queue::job(cut_update_queue::task_t::CUT_ANALYSIS_TASK, view_id, model_id));
        }
    }

    semaphore_.signal(index_->num_models() * index_->num_views());

    master_semaphore_.wait();
    if (is_shutdown())
        return;

    ASSERT(semaphore_.num_signals() == 0);
    ASSERT(master_semaphore_.num_signals() == 0);

    index_->sort();

    //re-configure semaphores
    master_semaphore_.lock();
    master_semaphore_.set_max_signal_count(1);
    master_semaphore_.set_min_signal_count(1);
    master_semaphore_.unlock();

    semaphore_.lock();
    semaphore_.set_max_signal_count(1);
    semaphore_.set_min_signal_count(1);
    semaphore_.unlock();

    job_queue_.push_job(cut_update_queue::job(cut_update_queue::task_t::CUT_UPDATE_TASK, 0, 0));
    semaphore_.signal(1);

    master_semaphore_.wait();
    if (is_shutdown())
        return;

#ifdef LAMURE_CUT_UPDATE_ENABLE_REPEAT_MODE
    }
#endif

    //apply changes
    {
        //model_database* database = model_database::get_instance();
        cut_database* cuts = cut_database::get_instance();

        cuts->lock_record(context_id_);

        for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
            for (view_t view_id = 0; view_id < index_->num_views(); ++view_id) {
                cut cut(context_id_, view_id, model_id);
                cut.set_complete_set(render_list_[view_id][model_id]);

                cuts->set_cut(context_id_, view_id, model_id, cut);
            }
        }

        cuts->set_updated_set(context_id_, transfer_list_);

        cuts->set_is_front_modified(context_id_, gpu_cache_->transfer_budget() < upload_budget_in_nodes_); //...
        cuts->set_is_swap_required(context_id_, true);
        cuts->set_buffer(context_id_, current_gpu_buffer_);

        cuts->unlock_record(context_id_);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            master_dispatched_ = false;
        }
    }


}


void cut_update_pool::
cut_analysis(view_t view_id, model_t model_id) {

    ASSERT(view_id != invalid_view_t);
    ASSERT(model_id != invalid_model_t);
    ASSERT(view_id < index_->num_views());
    ASSERT(model_id < index_->num_models());

    mat4r_t model_matrix;
#ifdef LAMURE_CUT_UPDATE_ENABLE_MODEL_TIMEOUT
    size_t freshness;
#endif
    lamure::util::frustum_t frustum;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        model_matrix = model_transforms_[model_id];
#ifdef LAMURE_CUT_UPDATE_ENABLE_MODEL_TIMEOUT
        freshness = model_freshness_[model_id];
#endif
        frustum = user_cameras_[view_id].get_frustum_by_model(model_matrix);
    }

    //perform cut analysis
    std::set<node_t> old_cut = index_->get_previous_cut(view_id, model_id);

    index_->reset_cut(view_id, model_id);

    uint32_t fan_factor = index_->fan_factor(model_id);

    bool freshness_timeout = false;
#ifdef LAMURE_CUT_UPDATE_ENABLE_MODEL_TIMEOUT
    freshness_timeout = cut_update_counter_ - freshness > LAMURE_CUT_UPDATE_MAX_MODEL_TIMEOUT;
#endif

    float64_t min_error_threshold = model_thresholds_[model_id] - 0.1;
    float64_t max_error_threshold = model_thresholds_[model_id] + 0.1;

    //cut analysis
    std::set<node_t>::iterator cut_it;
    for (cut_it = old_cut.begin(); cut_it != old_cut.end(); ++cut_it) {
        node_t node_id = *cut_it;

        bool all_siblings_in_cut = false;
        bool no_sibling_in_frustum = true;

        node_t parent_id = 0;
        float64_t parent_error = 0;
        std::vector<node_t> siblings;

        ASSERT(node_id != invalid_node_t);
        ASSERT(node_id < index_->num_nodes(model_id));



        if (node_id > 0 && node_id < index_->num_nodes(model_id)) {
            parent_id = index_->get_parent_id(model_id, node_id);
            parent_error = calculate_node_error(view_id, model_id, parent_id);

            index_->get_all_siblings(model_id, node_id, siblings);

            all_siblings_in_cut = is_all_nodes_in_cut(model_id, siblings, old_cut);
            no_sibling_in_frustum = !is_node_in_frustum(view_id, model_id, parent_id, frustum);
        }

        if (!all_siblings_in_cut) {
            float64_t node_error = calculate_node_error(view_id, model_id, node_id);
            bool node_in_frustum = is_node_in_frustum(view_id, model_id, node_id, frustum);

            if (node_in_frustum && node_error > max_error_threshold) {
                //only split if the predicted error of children does not require collapsing
                bool split = true;
                std::vector<node_t> children;
                index_->get_all_children(model_id, node_id, children);
                for (const auto& child_id : children) {
                    if (child_id == invalid_node_t)
                    {
                        split = false;
                        break;
                    }

                    float64_t child_error = calculate_node_error(view_id, model_id, child_id);
                    if (child_error < min_error_threshold)
                    {
                        split = false;
                        break;
                    }
                }
                if (!split || freshness_timeout) {
                    index_->push_action(cut_update_index::action(cut_update_index::queue_t::KEEP, view_id, model_id, node_id, parent_error), false);
                }
                else {
                    index_->push_action(cut_update_index::action(cut_update_index::queue_t::MUST_SPLIT,view_id, model_id, node_id, node_error), false);
                }
            }
            else {
                index_->push_action(cut_update_index::action(cut_update_index::queue_t::KEEP, view_id, model_id, node_id, parent_error), false);
            }

        }
        else {
            if (no_sibling_in_frustum) {
#ifdef LAMURE_CUT_UPDATE_MUST_COLLAPSE_OUTSIDE_FRUSTUM
                index_->push_action(cut_update_index::action(cut_update_index::queue_t::MUST_COLLAPSE, view_id, model_id, parent_id, parent_error), false);
#else
                index_->push_action(cut_update_index::action(cut_update_index::queue_t::COLLAPSE_ON_NEED, view_id, model_id, parent_id, parent_error), false);
#endif
            }
            else {
                //the entire group of siblings is in the cut and visible

                if (freshness_timeout) {
                    index_->push_action(cut_update_index::action(cut_update_index::queue_t::COLLAPSE_ON_NEED, view_id, model_id, parent_id, parent_error), false);

                    //skip to next group of siblings
                    std::advance(cut_it, fan_factor-1);
                    continue;
                }

                bool keep_all_siblings = true;

                bool all_sibling_errors_below_min_error_threshold = true;

                std::vector<bool> keep_sibling;

                for (const auto& sibling_id : siblings) {
                    float64_t sibling_error = calculate_node_error(view_id, model_id, sibling_id);
                    bool sibling_in_frustum = is_node_in_frustum(view_id, model_id, sibling_id, frustum);

                    if (sibling_error > max_error_threshold && sibling_in_frustum) {
                        //only split if the predicted error of children does not require collapsing
                        bool split = true;
                        std::vector<node_t> children;
                        index_->get_all_children(model_id, sibling_id, children);
                        for (const auto& child_id : children) {
                            if (child_id == invalid_node_t) {
                                split = false;
                                break;
                            }

                            float64_t child_error = calculate_node_error(view_id, model_id, child_id);
                            if (child_error < min_error_threshold) {
                                split = false;
                                break;
                            }
                        }
                        if (!split) {
                            keep_sibling.push_back(true);
                        }
                        else {
                            index_->push_action(cut_update_index::action(cut_update_index::queue_t::MUST_SPLIT, view_id, model_id, sibling_id, sibling_error), false);

                            keep_all_siblings = false;
                            keep_sibling.push_back(false);
                        }
                    }
                    else keep_sibling.push_back(true);

                    if (sibling_error >= min_error_threshold) {
                        all_sibling_errors_below_min_error_threshold = false;
                    }

                }


                if (keep_all_siblings && all_sibling_errors_below_min_error_threshold) {
                    index_->push_action(cut_update_index::action(cut_update_index::queue_t::MUST_COLLAPSE, view_id, model_id, parent_id, parent_error), false);
                }
                else if (keep_all_siblings) {
                    index_->push_action(cut_update_index::action(cut_update_index::queue_t::MAYBE_COLLAPSE, view_id, model_id, parent_id, parent_error), false);
                }
                else {
                    for (uint32_t j = 0; j < fan_factor; ++j) {
                        if (keep_sibling[j]) {
                            index_->push_action(cut_update_index::action(cut_update_index::queue_t::KEEP, view_id, model_id, siblings[j], parent_error), false);
                        }
                    }
                }

            }

            //skip to next group of siblings
            std::advance(cut_it, fan_factor-1);
        }

    }
    
    master_semaphore_.signal(1);


}

void cut_update_pool::
cut_update_split_again(const cut_update_index::action& split_action) {

    std::vector<node_t> candidates;
    index_->get_all_children(split_action.model_id_, split_action.node_id_, candidates);

    mat4r_t model_matrix = model_transforms_[split_action.model_id_];
    lamure::util::frustum_t frustum = user_cameras_[split_action.view_id_].get_frustum_by_model(model_matrix);

    float64_t min_error_threshold = model_thresholds_[split_action.model_id_] - 0.1;
    float64_t max_error_threshold = model_thresholds_[split_action.model_id_] + 0.1;
    
    for (const auto& candidate_id : candidates) {
        float64_t node_error = calculate_node_error(split_action.view_id_, split_action.model_id_, candidate_id);

        if (node_error > max_error_threshold) {
            //only split if the predicted error of children does not require collapsing
            bool split = true;
            std::vector<node_t> children;
            index_->get_all_children(split_action.model_id_, candidate_id, children);
            for (const auto& child_id : children) {
                if (child_id == invalid_node_t) {
                    split = false;
                    break;
                }

                float64_t child_error = calculate_node_error(split_action.view_id_, split_action.model_id_, child_id);
                if (child_error < min_error_threshold) {
                    split = false;
                    break;
                }
            }
            if (!split) {
                index_->push_action(cut_update_index::action(cut_update_index::queue_t::KEEP, split_action.view_id_, split_action.model_id_, candidate_id, node_error), true);

            }
            else {
                index_->push_action(cut_update_index::action(cut_update_index::queue_t::MUST_SPLIT, split_action.view_id_, split_action.model_id_, candidate_id, node_error), true);

            }
        }
        else {
            index_->push_action(cut_update_index::action(cut_update_index::queue_t::KEEP, split_action.view_id_, split_action.model_id_, candidate_id, node_error), true);
        }
    }

}

void cut_update_pool::
cut_update() {
    ooc_cache* ooc_cache = ooc_cache::get_instance();
    ooc_cache->lock();
    ooc_cache->refresh();
    gpu_cache_->lock();
    
    //std::cout << ooc_cache->num_free_slots() << std::endl;

    bool check_residency = true;

    bool all_children_in_ooc_cache = true;
    bool all_children_in_gpu_cache = true;

    //cut update
    while (index_->num_actions(cut_update_index::queue_t::MUST_SPLIT) > 0) {

        cut_update_index::action must_split_action = index_->front_action(cut_update_index::queue_t::MUST_SPLIT);
        size_t fan_factor = index_->fan_factor(must_split_action.model_id_);

#if 1


        if (check_residency) {


        std::vector<node_t> child_ids;
        index_->get_all_children(must_split_action.model_id_, must_split_action.node_id_, child_ids);

        for (const auto& child_id : child_ids) {
            if (!ooc_cache->is_node_resident(must_split_action.model_id_, child_id)) {
                all_children_in_ooc_cache = false;
                if (!all_children_in_gpu_cache)
                    break;
            }
            if (!gpu_cache_->is_node_resident(must_split_action.model_id_, child_id)) {
                all_children_in_gpu_cache = false;
                if (!all_children_in_ooc_cache)
                    break;
            }

        }

        if (all_children_in_ooc_cache && all_children_in_gpu_cache) {

            index_->pop_front_action(cut_update_index::queue_t::MUST_SPLIT);

            for (const auto& child_id : child_ids) {
                gpu_cache_->aquire_node(context_id_, must_split_action.view_id_, must_split_action.model_id_, child_id);
                ooc_cache->aquire_node(context_id_, must_split_action.view_id_, must_split_action.model_id_, child_id);
            }

#ifdef LAMURE_CUT_UPDATE_ENABLE_SPLIT_AGAIN_MODE
            cut_update_split_again(must_split_action);
#else
            index_->approve_action(must_split_action);
#endif
            continue;

        }

        }

#endif
        check_residency = false;

        bool all_children_fit_in_ooc_cache = ooc_cache->num_free_slots() >= fan_factor;
        bool all_children_fit_in_gpu_cache = gpu_cache_->num_free_slots() >= fan_factor;

        if ((all_children_fit_in_ooc_cache && all_children_fit_in_gpu_cache)
            || (all_children_in_ooc_cache && all_children_fit_in_gpu_cache)) {
            cut_update_index::action msa = index_->front_action(cut_update_index::queue_t::MUST_SPLIT);
            index_->pop_front_action(cut_update_index::queue_t::MUST_SPLIT);

            split_node(msa);
            check_residency = true;
            continue;
        }

        if (index_->num_actions(cut_update_index::queue_t::MUST_COLLAPSE) > 0) {
            cut_update_index::action collapse_action = index_->front_action(cut_update_index::queue_t::MUST_COLLAPSE);
            index_->pop_front_action(cut_update_index::queue_t::MUST_COLLAPSE);

            collapse_node(collapse_action);
            continue;
        }

        if (index_->num_actions(cut_update_index::queue_t::COLLAPSE_ON_NEED) > 0) {
            cut_update_index::action collapse_on_need_action = index_->front_action(cut_update_index::queue_t::COLLAPSE_ON_NEED);
            index_->pop_front_action(cut_update_index::queue_t::COLLAPSE_ON_NEED);

            collapse_node(collapse_on_need_action);
            continue;
        }

        if (index_->num_actions(cut_update_index::queue_t::MAYBE_COLLAPSE) > 0) {
            if (must_split_action.error_
                > index_->back_action(cut_update_index::queue_t::MAYBE_COLLAPSE).error_) {
                cut_update_index::action collapse_action = index_->back_action(cut_update_index::queue_t::MAYBE_COLLAPSE);
                index_->Popback_action(cut_update_index::queue_t::MAYBE_COLLAPSE);

                collapse_node(collapse_action);
                continue;
            }
        }

#ifdef LAMURE_CUT_UPDATE_ENABLE_CUT_UPDATE_EXPERIMENTAL_MODE
        if (index_->num_actions(cut_update_index::queue_t::KEEP) > 0) {
            cut_update_index::action keep_action = index_->back_action(cut_update_index::queue_t::KEEP);
            index_->Popback_action(cut_update_index::queue_t::KEEP);

            if (must_split_action.error_ > keep_action.error_) {
                node_t keep_action_parent_id = index_->get_parent_id(keep_action.model_id_, keep_action.node_id_);

                if (keep_action.node_id_ > 0 && keep_action_parent_id > 0) {

                    std::vector<node_t> siblings;
                    index_->get_all_siblings(keep_action.model_id_, keep_action.node_id_, siblings);

                    if (is_all_nodes_in_cut(keep_action.model_id_, siblings, index_->get_previous_cut(keep_action.view_id_, keep_action.model_id_))) {
                        bool singularity = false;

                        for (const auto& sibling_id : siblings) {
                            if (singularity)
                                break;

                            if (sibling_id == must_split_action.node_id_) {
                                singularity = true;
                                break;
                            }

                            std::vector<node_t> sibling_children;
                            index_->get_all_children(keep_action.model_id_, sibling_id, sibling_children);

                            for (const auto& sibling_child_id : sibling_children) {
                                if (sibling_child_id == must_split_action.node_id_) {
                                    singularity = true;
                                    break;
                                }
                            }

                        }

                        if (!singularity) {
                            for (const auto& sibling_id : siblings) {
                                if (sibling_id != invalid_node_t) {

                                    //cancel all possible actions on sibling_id
                                    index_->cancel_action(keep_action.view_id_, keep_action.model_id_, sibling_id);

                                    if (gpu_cache_->release_node_invalidate(context_id_,
                                                                          keep_action.view_id_,
                                                                          keep_action.model_id_,
                                                                          sibling_id)) {
                                        gpu_cache_->RemoveFromTransferList(keep_action.model_id_, sibling_id);
                                    }

                                    ooc_cache->release_node(context_id_, keep_action.view_id_, keep_action.model_id_, sibling_id);

                                    //cancel a possible split action that already happened
                                    std::vector<node_t> sibling_children;
                                    index_->get_all_children(keep_action.model_id_, sibling_id, sibling_children);
                                    for (const auto& sibling_child_id : sibling_children) {
                                        if (sibling_child_id != invalid_node_t) {
                                            index_->cancel_action(keep_action.view_id_, keep_action.model_id_, sibling_child_id);

                                            if (gpu_cache_->release_node_invalidate(context_id_,
                                                                                keep_action.view_id_,
                                                                                keep_action.model_id_,
                                                                                sibling_child_id)) {
                                                gpu_cache_->RemoveFromTransferList(keep_action.model_id_, sibling_child_id);
                                            }

                                            ooc_cache->release_node(context_id_, keep_action.view_id_, keep_action.model_id_, sibling_child_id);
                                        }
                                    }
                                }
                            }

                            ASSERT(gpu_cache_->is_node_resident(keep_action.model_id_, keep_action_parent_id));
                            ASSERT(ooc_cache->is_node_resident(keep_action.model_id_, keep_action_parent_id));

                            index_->approve_action(
                                cut_update_index::action(cut_update_index::queue_t::KEEP,
                                                       keep_action.view_id_,
                                                       keep_action.model_id_,
                                                       keep_action_parent_id,
                                                       keep_action.error_));

                            continue;
                        }
                    }
                }
            }

            //approve keep action
            index_->approve_action(keep_action);

        }


        if (index_->num_actions(cut_update_index::queue_t::MUST_SPLIT) > 1) { //> 1, prevent request from canceling itself
            cut_update_index::action split_action = index_->back_action(cut_update_index::queue_t::MUST_SPLIT);
            index_->Popback_action(cut_update_index::queue_t::MUST_SPLIT);

            if (must_split_action.error_ > split_action.error_) {
                node_t split_action_parent_id = index_->get_parent_id(split_action.model_id_, split_action.node_id_);

                if (split_action.node_id_ > 0 && split_action_parent_id > 0) {

                    //only if siblings are also in cut -- why does it make sense to check this here?
                    //because we check it above in the keep-action branch anyway
                    //and there is really no reason to cancel this action if we cannot use it
                    //to cut down memory usage in the end.
                    std::vector<node_t> siblings;
                    index_->get_all_siblings(split_action.model_id_, split_action.node_id_, siblings);

                    if (is_all_nodes_in_cut(split_action.model_id_,
                                        siblings,
                                        index_->get_previous_cut(split_action.view_id_, split_action.model_id_))) {

                        bool singularity = split_action.node_id_ == must_split_action.node_id_;

                        std::vector<node_t> split_children;
                        index_->get_all_children(split_action.model_id_, split_action.node_id_, split_children);

                        //check if children of split_action are equal to the must_split_node (parent of must_split)
                        //this is important, since it would not free any memory anyways to cancel a split_action that is above
                        //the must_split action in the hierarchy
                        for (const auto& split_child_id : split_children) {
                            if (split_child_id == must_split_action.node_id_) {
                                singularity = true;
                                break;
                            }
                        }

                        if (!singularity) {
                            ASSERT(gpu_cache_->is_node_resident(split_action.model_id_, split_action.node_id_));
                            ASSERT(ooc_cache->is_node_resident(split_action.model_id_, split_action.node_id_));

                            mat4r_t model_matrix = model_transforms_[split_action.model_id_];

                            float64_t replacement_node_error = calculate_node_error(split_action.view_id_,
                                                                              split_action.model_id_,
                                                                              split_action.node_id_);
                            index_->push_action(
                                cut_update_index::action(cut_update_index::queue_t::KEEP,
                                                       split_action.view_id_,
                                                       split_action.model_id_,
                                                       split_action.node_id_,
                                                       replacement_node_error*2.75f),
                                true);

                            continue;
                        }
                    }

                }

            }

            //reject split action
            index_->reject_action(split_action);

        }

#endif

        //no success, reject must split action
        cut_update_index::action msa = index_->front_action(cut_update_index::queue_t::MUST_SPLIT);
        index_->pop_front_action(cut_update_index::queue_t::MUST_SPLIT);
        index_->reject_action(msa);
        check_residency = true;

    }

    //approve all remaining must-collapse-actions
    while (index_->num_actions(cut_update_index::queue_t::MUST_COLLAPSE) > 0) {
        cut_update_index::action collapse_action = index_->front_action(cut_update_index::queue_t::MUST_COLLAPSE);
        index_->pop_front_action(cut_update_index::queue_t::MUST_COLLAPSE);
        collapse_node(collapse_action);

    }

#ifdef LAMURE_CUT_UPDATE_ENABLE_PREFETCHING
    prefetch_routine();      
#endif
    gpu_cache_->unlock();
    ooc_cache->unlock();

    //reject remaining collapse-on-need-actions
    while (index_->num_actions(cut_update_index::queue_t::COLLAPSE_ON_NEED) > 0) {
        cut_update_index::action collapse_on_need_action = index_->front_action(cut_update_index::queue_t::COLLAPSE_ON_NEED);
        index_->pop_front_action(cut_update_index::queue_t::COLLAPSE_ON_NEED);
        index_->reject_action(collapse_on_need_action);

    }

    //reject remaining maybe-collapse-actions
    while (index_->num_actions(cut_update_index::queue_t::MAYBE_COLLAPSE) > 0) {
        cut_update_index::action maybe_collapse_action = index_->front_action(cut_update_index::queue_t::MAYBE_COLLAPSE);
        index_->pop_front_action(cut_update_index::queue_t::MAYBE_COLLAPSE);
        index_->reject_action(maybe_collapse_action);

    }

    //approve all keep-actions
    while (index_->num_actions(cut_update_index::queue_t::KEEP) > 0) {
        cut_update_index::action keep_action = index_->front_action(cut_update_index::queue_t::KEEP);
        index_->pop_front_action(cut_update_index::queue_t::KEEP);
        index_->approve_action(keep_action);

    }

    ASSERT(index_->num_actions(cut_update_index::queue_t::KEEP) == 0);
    ASSERT(index_->num_actions(cut_update_index::queue_t::MUST_SPLIT) == 0);
    ASSERT(index_->num_actions(cut_update_index::queue_t::MUST_COLLAPSE) == 0);
    ASSERT(index_->num_actions(cut_update_index::queue_t::COLLAPSE_ON_NEED) == 0);
    ASSERT(index_->num_actions(cut_update_index::queue_t::MAYBE_COLLAPSE) == 0);

    compile_render_list();
    compile_transfer_list();

    master_semaphore_.signal(1);

}

void cut_update_pool::
compile_render_list() {
    render_list_.clear();

    const std::set<view_t>& view_ids = index_->view_ids();

    for (const auto  view_id : view_ids) {
        std::vector<std::vector<cut::node_slot_aggregate>> view_render_lists;

        for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
            std::vector<cut::node_slot_aggregate> model_render_lists;

            const std::set<node_t>& current_cut = index_->get_current_cut(view_id, model_id);

            //std::cout << "cut: " << current_cut.size() << std::endl;

            for (const auto& node_id : current_cut) {
                model_render_lists.push_back(cut::node_slot_aggregate(node_id, gpu_cache_->slot_id(model_id, node_id)));
            }

            view_render_lists.push_back(model_render_lists);
        }
        render_list_.push_back(view_render_lists);
    }

}


#ifdef LAMURE_CUT_UPDATE_ENABLE_PREFETCHING
void cut_update_pool::
prefetch_routine() {

   ooc_cache* ooc_cache = ooc_cache::get_instance();

#if 0
   uint32_t num_prefetched = 0;
#endif

   std::queue<std::pair<model_t, node_t>> node_id_queue;
   for (const auto& action : pending_prefetch_set_) {
      if (action.node_id_ == invalid_node_t) {
          continue;
      }
      
      float64_t max_error_threshold = model_thresholds_[model_id] + 0.1;

      if (action.error_ > max_error_threshold * LAMURE_CUT_UPDATE_PREFETCH_FACTOR) {
 
         std::vector<node_t> child_ids;
         index_->get_all_children(action.model_id_, action.node_id_, child_ids);
         for (const auto& child_id : child_ids) {
            node_id_queue.push(std::make_pair(action.model_id_, child_id));
         }
      }
   }


   uint32_t current_prefetch_depth = 0; 
   
   while (!node_id_queue.empty()) {
 
     std::pair<model_t, node_t> model_node = node_id_queue.front();
     node_t node_id = model_node.second;
     model_t model_id = model_node.first;
     node_id_queue.pop();

     std::vector<node_t> child_ids;
     index_->get_all_children(model_id, node_id, child_ids);
            
     uint32_t fan_factor = index_->fan_factor(model_id);
     if (++current_prefetch_depth < LAMURE_CUT_UPDATE_PREFETCH_BUDGET) {
        if (ooc_cache->num_free_slots() > ooc_cache->num_slots()/4
           && gpu_cache_->num_free_slots() > gpu_cache_->num_slots()/4) {
 
           bool all_children_fit_in_ooc_cache = ooc_cache->num_free_slots() >= fan_factor;
           bool all_children_fit_in_gpu_cache = gpu_cache_->num_free_slots() >= fan_factor;     

           if (all_children_fit_in_ooc_cache && all_children_fit_in_gpu_cache) {
              for (const auto& child_id : child_ids) {

                if (child_id == invalid_node_t) 
                   continue;

                if (!ooc_cache->is_node_resident(model_id, child_id)) {
                   //load child from harddisk
                   if (ooc_cache->num_free_slots() > 0) {
                      ooc_cache->register_node(model_id, child_id, (int32_t)(-current_prefetch_depth));
                   }
                }
                    
                node_id_queue.push(std::make_pair(model_id, child_id));
#if 0
                ++num_prefetched;
#endif
              }
           } 
           else {
              break;
           }
             
           
        }
     }
  }

   pending_prefetch_set_.clear();

#if 0
   std::cout << "lamure: num prefetched: " << num_prefetched << std::endl;
#endif


}
#endif

void cut_update_pool::
compile_transfer_list() {
    model_database* database = model_database::get_instance();
    ooc_cache* ooc_cache = ooc_cache::get_instance();

    const std::vector<std::unordered_set<node_t>>& transfer_list = gpu_cache_->transfer_list();

    slot_t slot_count = gpu_cache_->transfer_slots_written();

    for (model_t model_id = 0; model_id < index_->num_models(); ++model_id) {
        for (const auto& node_id : transfer_list[model_id]) {
            slot_t slot_id = gpu_cache_->slot_id(model_id, node_id);

            ASSERT(slot_id < (slot_t)render_budget_in_nodes_);

            char* node_data = ooc_cache->node_data(model_id, node_id);

            memcpy(current_gpu_storage_ + slot_count*database->get_slot_size(), node_data, database->get_slot_size());

            transfer_list_.push_back(cut_database_record::slot_update_desc(slot_count, slot_id));

            ++slot_count;

        }

    }

    gpu_cache_->reset_transfer_list();
    gpu_cache_->set_transfer_slots_written(slot_count);


}

void cut_update_pool::
split_node(const cut_update_index::action& action) {

    std::vector<node_t> child_ids;
    index_->get_all_children(action.model_id_, action.node_id_, child_ids);

    //return if children are invalid node ids
    if (child_ids[0] == invalid_node_t || action.node_id_ == invalid_node_t) {
        index_->reject_action(action);
        return;
    }

    size_t fan_factor = index_->fan_factor(action.model_id_);

    ASSERT(child_ids[0] < index_->num_nodes(action.model_id_));

    bool all_children_available = true;

    ooc_cache* ooc_cache = ooc_cache::get_instance();
    bool all_children_fit_in_ooc_cache = ooc_cache->num_free_slots() >= fan_factor;
    bool all_children_fit_in_gpu_cache = gpu_cache_->transfer_budget() >= fan_factor && gpu_cache_->num_free_slots() >= fan_factor;

    //try to obtain children
    for (const auto& child_id : child_ids) {
        if (!ooc_cache->is_node_resident(action.model_id_, child_id)) {
            if (all_children_fit_in_ooc_cache) {
                //load child from harddisk
                if (ooc_cache->num_free_slots() > 0) {
                    ooc_cache->register_node(action.model_id_, child_id, (int32_t)action.error_);
                }
            }
            all_children_available = false;
        }
    }

    if (all_children_available) {
        for (const auto& child_id : child_ids) {
            if (!gpu_cache_->is_node_resident(action.model_id_, child_id)) {
                if (all_children_fit_in_gpu_cache) {
                    //transfer child to gpu
                    if (gpu_cache_->transfer_budget() > 0 && gpu_cache_->num_free_slots() > 0) {
                        if (gpu_cache_->register_node(action.model_id_, child_id)) {
#ifdef LAMURE_CUT_UPDATE_ENABLE_PREFETCHING
                           pending_prefetch_set_.push_back(action);
#endif
                        }

                    }
                    else {
                        all_children_available = false;
                    }
                }
                else {
                    all_children_available = false;
                }
            }
        }
    }

    if (all_children_available) {
        for (const auto& child_id : child_ids) {
            gpu_cache_->aquire_node(context_id_, action.view_id_, action.model_id_, child_id);
            ooc_cache->aquire_node(context_id_, action.view_id_, action.model_id_, child_id);
        }

#ifdef LAMURE_CUT_UPDATE_ENABLE_SPLIT_AGAIN_MODE
        cut_update_split_again(action);
#else
        index_->approve_action(action);
#endif
    }
    else {
        index_->reject_action(action);
    }

}

void cut_update_pool::
collapse_node(const cut_update_index::action& action) {

    //return if parent is invalid node id
    if (action.node_id_ < 1 || action.node_id_ == invalid_node_t) {
        index_->reject_action(action);
        return;
    }

    std::vector<node_t> child_ids;
    index_->get_all_children(action.model_id_, action.node_id_, child_ids);

    ooc_cache* ooc_cache = ooc_cache::get_instance();

    for (const auto& child_id : child_ids) {
        gpu_cache_->release_node(context_id_, action.view_id_, action.model_id_, child_id);
        ooc_cache->release_node(context_id_, action.view_id_, action.model_id_, child_id);
    }

    index_->approve_action(action);

}

const bool cut_update_pool::
is_all_nodes_in_cut(const model_t model_id, const std::vector<node_t>& node_ids, const std::set<node_t>& cut) {
    for (node_t i = 0; i < node_ids.size(); ++i) {
        node_t node_id = node_ids[i];

        if (node_id >= (node_t)index_->num_nodes(model_id))
            return false;

        if (node_id == invalid_node_t)
            return false;

        if (cut.find(node_id) == cut.end())
            return false;
    }

    return true;
}

const bool cut_update_pool::
is_node_in_frustum(const view_t view_id, const model_t model_id, const node_t node_id, const lamure::util::frustum_t& frustum) {
    model_database* database = model_database::get_instance();
    return 1 != user_cameras_[view_id].cull_against_frustum(frustum, database->get_model(model_id)->get_bvh()->get_bounding_boxes()[node_id]);
}

const bool cut_update_pool::
is_no_node_in_frustum(const view_t view_id, const model_t model_id, const std::vector<node_t>& node_ids, const lamure::util::frustum_t& frustum) {
    for (const auto& node_id : node_ids) {
        if (node_id >= (node_t)index_->num_nodes(model_id))
            return false;

        if (node_id == invalid_node_t)
            return false;

        model_database* database = model_database::get_instance();
        if (1 != user_cameras_[view_id].cull_against_frustum(frustum, database->get_model(model_id)->get_bvh()->get_bounding_boxes()[node_id]))
            return false;
    }

    return true;
}


const float64_t cut_update_pool::
calculate_node_error(const view_t view_id, const model_t model_id, const node_t node_id) {

    model_database* database = model_database::get_instance();
    auto bvh = database->get_model(model_id)->get_bvh();

    const mat4r_t& model_matrix = model_transforms_[model_id];
    const mat4r_t& view_matrix = user_cameras_[view_id].get_view_matrix();

    float64_t radius_scaling = lamure::math::length(model_matrix * vec4r_t(1.0,0.0,0.0,0.0));
    float64_t representative_radius = bvh->get_avg_primitive_extent(node_id) * radius_scaling;

#if 1
 
    //original error computation
    vec3r_t view_position = view_matrix * model_matrix * bvh->get_centroids()[node_id];
    float64_t near_plane = user_cameras_[view_id].near_plane_value();
    float64_t height_divided_by_top_minus_bottom = height_divided_by_top_minus_bottoms_[view_id];
    float64_t error = lamure::math::abs(2.0 * representative_radius * (near_plane/-view_position.z_) * height_divided_by_top_minus_bottom);

#else
    
    const mat4r_t& proj_matrix = user_cameras_[view_id].get_projection_matrix();

    mat4r_t cm = lamure::math::inverse(view_matrix);
    vec3r_t position = model_matrix * bvh->centroids()[node_id];
    vec3r_t u = vec3r_t(cm[0], cm[1], cm[2]);
    vec3r_t perimeter = position + (lamure::math::normalize(u) * representative_radius);
    
    //project the position and the perimeter point, take their distance
    mat4r_t view_projection = proj_matrix * view_matrix;
    vec4r_t view_position = view_projection * position;
    vec4r_t view_perimeter = view_projection * perimeter;
    float64_t error = lamure::math::length(view_position/view_position.w - view_perimeter/view_perimeter.w);
    //std::cout << "lamure: " << view_position << "  ----  " << view_perimeter << " ,,,, " << error << std::endl;
#endif

    return error;
}



} // namespace lod

} // namespace lamure


