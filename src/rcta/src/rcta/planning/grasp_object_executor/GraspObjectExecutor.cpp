#include "GraspObjectExecutor.h"

// standard includes
#include <cmath>

// system includes
#include <Eigen/Dense>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <eigen_conversions/eigen_msg.h>
#include <sbpl_geometry_utils/utils.h>
#include <spellbook/geometry_msgs/geometry_msgs.h>
#include <spellbook/msg_utils/msg_utils.h>
#include <spellbook/random/gaussian.h>
#include <spellbook/stringifier/stringifier.h>
#include <spellbook/utils/RunUponDestruction.h>
#include <spellbook/utils/utils.h>

// project includes
#include <rcta/control/robotiq_controllers/gripper_model.h>

namespace GraspObjectExecutionStatus
{

std::string to_string(Status status)
{
    switch (status) {
    case IDLE:
        return "Idle";
    case FAULT:
        return "Fault";
    case GENERATING_GRASPS:
        return "GeneratingGrasps";
    case PLANNING_ARM_MOTION_TO_PREGRASP:
        return "PlanningArmMotionToPregrasp";
    case EXECUTING_ARM_MOTION_TO_PREGRASP:
        return "ExecutingArmMotionToPregrasp";
    case OPENING_GRIPPER:
        return "OpeningGripper";
    case EXECUTING_VISUAL_SERVO_MOTION_TO_PREGRASP:
        return "ExecutingVisualServoMotionToPregrasp";
    case EXECUTING_VISUAL_SERVO_MOTION_TO_GRASP:
        return "ExecutingVisualServoMotionToGrasp";
    case GRASPING_OBJECT:
        return "GraspingObject";
    case RETRACTING_GRIPPER:
        return "RetractingGripper";
    case PLANNING_ARM_MOTION_TO_STOW_POSITION:
        return "PlanningArmMotionToStowPosition";
    case EXECUTING_ARM_MOTION_TO_STOW_POSITION:
        return "ExecutingArmMotionToStowPosition";
    case COMPLETING_GOAL:
        return "CompletingGoal";
    default:
        return "Invalid";
    }
}

} // namespace GraspObjectExecutionStatus

bool extract_xml_value(
    XmlRpc::XmlRpcValue& value,
    GraspObjectExecutor::StowPosition& stow_position)
{
    if (!value.hasMember("name") || !value.hasMember("joint_vector_degs")) {
        return false;
    }

    GraspObjectExecutor::StowPosition tmp;
    bool success =
            msg_utils::extract_xml_value(value["name"], tmp.name) &&
            msg_utils::extract_xml_value(value["joint_vector_degs"], tmp.joint_positions);

    if (success) {
        stow_position = tmp;
    }
    return success;
}

GraspObjectExecutor::GraspObjectExecutor() :
    nh_(),
    ph_("~"),
    costmap_sub_(),
    m_viz(),
    last_occupancy_grid_(),
    current_occupancy_grid_(),
    wait_for_after_grasp_grid_(false),
    occupancy_grid_after_grasp_(),
    use_extrusion_octomap_(false),
    current_octomap_(),
    extruder_(100, false),
    extrusion_octomap_pub_(),
    action_name_("grasp_object_command"),
    as_(),

    move_arm_command_action_name_("move_arm_command"),
    move_arm_command_client_(),
    sent_move_arm_goal_(false),
    pending_move_arm_command_(false),
    move_arm_command_goal_state_(actionlib::SimpleClientGoalState::SUCCEEDED),
    move_arm_command_result_(),

    last_move_arm_pregrasp_goal_(),

    viservo_command_action_name_("viservo_command"),
    viservo_command_client_(),
    sent_viservo_command_(false),
    pending_viservo_command_(false),
    viservo_command_goal_state_(actionlib::SimpleClientGoalState::SUCCEEDED),
    viservo_command_result_(),

    gripper_command_action_name_("gripper_controller/gripper_command_action"),
    gripper_command_client_(),
    sent_gripper_command_(false),
    pending_gripper_command_(false),
    gripper_command_goal_state_(actionlib::SimpleClientGoalState::SUCCEEDED),
    gripper_command_result_(),

    current_goal_(),
    status_(GraspObjectExecutionStatus::INVALID),
    last_status_(GraspObjectExecutionStatus::INVALID),
    listener_()
{
    sbpl::viz::set_visualizer(&m_viz);
}

GraspObjectExecutor::~GraspObjectExecutor()
{
    if (sbpl::viz::visualizer() == &m_viz) {
        sbpl::viz::unset_visualizer();
    }
}

bool GraspObjectExecutor::initialize()
{
    m_rml.reset(new robot_model_loader::RobotModelLoader);
    m_robot_model = m_rml->getModel();
    if (!m_robot_model) {
        ROS_ERROR("Failed to load Robot Model");
        return false;
    }

    ros::NodeHandle grasp_nh(ph_, "grasping");
    if (!m_grasp_planner.init(grasp_nh)) {
        ROS_ERROR("Failed to initialize Grasp Planner");
        return false;
    }

    // read in stow positions
    if (!msg_utils::download_param(ph_, "stow_positions", stow_positions_)) {
        ROS_ERROR("Failed to retrieve 'stow_positions' from the param server");
        return false;
    }

    ROS_INFO("Stow Positions:");
    for (StowPosition& position : stow_positions_) {
        ROS_INFO("    %s: %s", position.name.c_str(), to_string(position.joint_positions).c_str());
        position.joint_positions = msg_utils::to_radians(position.joint_positions);
    }

    // read in max grasps
    if (!msg_utils::download_param(ph_, "max_grasp_candidates", max_grasp_candidates_) ||
        max_grasp_candidates_ < 0)
    {
        ROS_ERROR("Failed to retrieve 'max_grasp_candidates' from the param server or 'max_grasp_candidates' is negative");
        return false;
    }

    if (!msg_utils::download_param(ph_, "use_extrusion_octomap", use_extrusion_octomap_)) {
        ROS_ERROR("Failed to retrieve 'use_extrusion_octomap' from the param server");
        return false;
    }

    if (!msg_utils::download_param(ph_, "object_filter_radius_m", object_filter_radius_m_)) {
        ROS_ERROR("Failed to retrieve 'object_filter_radius_m' from the param server");
        return false;
    }

    if (!msg_utils::download_param(ph_, "gas_can_detection_threshold", gas_can_detection_threshold_)) {
        return false;
    }

    if (!download_marker_params()) {
        return false; // errors printed within
    }

    // subscribers
    costmap_sub_ = nh_.subscribe<nav_msgs::OccupancyGrid>("costmap", 1, &GraspObjectExecutor::occupancy_grid_cb, this);

    // publishers
    extrusion_octomap_pub_ = nh_.advertise<octomap_msgs::Octomap>("extrusion_octomap", 1);
    marker_arr_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("visualization_marker_array", 5);
    filtered_costmap_pub_ = nh_.advertise<nav_msgs::OccupancyGrid>("costmap_filtered", 1);

    move_arm_command_client_.reset(new MoveArmActionClient(move_arm_command_action_name_, false));
    if (!move_arm_command_client_) {
        ROS_ERROR("Failed to instantiate Move Arm Command Client");
        return false;
    }

    viservo_command_client_.reset(new ViservoCommandActionClient(viservo_command_action_name_, false));
    if (!viservo_command_client_) {
        ROS_ERROR("Failed to instantiate Viservo Command Client");
        return false;
    }

    gripper_command_client_.reset(new GripperCommandActionClient(gripper_command_action_name_, false));
    if (!gripper_command_client_) {
        ROS_ERROR("Failed to instantiate Gripper Command Client");
        return false;
    }

    as_.reset(new GraspObjectCommandActionServer(action_name_, false));
    if (!as_) {
        ROS_ERROR("Failed to instantiate Grasp Object Action Server");
        return false;
    }

    as_->registerGoalCallback(boost::bind(&GraspObjectExecutor::goal_callback, this));
    as_->registerPreemptCallback(boost::bind(&GraspObjectExecutor::preempt_callback, this));

    ROS_INFO("Starting action server '%s'...", action_name_.c_str());
    as_->start();
    ROS_INFO("Action server started");

    return true;
}

int GraspObjectExecutor::run()
{
    if (!initialize()) {
        return FAILED_TO_INITIALIZE;
    }

    status_ = GraspObjectExecutionStatus::IDLE;

    ros::Rate loop_rate(1);
    while (ros::ok()) {
        RunUponDestruction rod([&](){ loop_rate.sleep(); });

        ros::spinOnce();

        if (status_ != last_status_) {
            ROS_INFO("Grasp Job Executor Transitioning: %s -> %s", to_string(last_status_).c_str(), to_string(status_).c_str());
            last_status_ = status_;
        }

        switch (status_) {
        case GraspObjectExecutionStatus::IDLE: {
            status_ = onIdle();
        }   break;
        case GraspObjectExecutionStatus::FAULT: {
            status_ = onFault();
        }   break;
        case GraspObjectExecutionStatus::GENERATING_GRASPS:
        {
            status_ = onGeneratingGrasps();
        }   break;
        case GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP:
        {
            status_ = onPlanningArmMotionToPregrasp();
        }   break;
        case GraspObjectExecutionStatus::EXECUTING_ARM_MOTION_TO_PREGRASP:
        {
            status_ = onExecutingArmMotionToPregrasp();
        }   break;
        case GraspObjectExecutionStatus::OPENING_GRIPPER:
        {
            status_ = onOpeningGripper();
        }   break;
        case GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_PREGRASP:
        {
            status_ = onExecutingVisualServoMotionToPregrasp();
        }   break;
        case GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_GRASP:
        {
            status_ = onExecutingVisualServoMotionToGrasp();
        }   break;
        case GraspObjectExecutionStatus::GRASPING_OBJECT:
        {
            status_ = onGraspingObject();
        }   break;
        case GraspObjectExecutionStatus::RETRACTING_GRIPPER:
        {
            status_ = onRetractingGripper();
        }   break;
        case GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_STOW_POSITION:
        {
            status_ = onPlanningArmMotionToStowPosition();
        }   break;
        case GraspObjectExecutionStatus::EXECUTING_ARM_MOTION_TO_STOW_POSITION:
        {
            status_ = onExecutingArmMotionToStowPosition();
        }   break;
        case GraspObjectExecutionStatus::COMPLETING_GOAL:
        {
            status_ = onCompletingGoal();
        }   break;
        default:
            break;
        }
    }

    return SUCCESS;
}

void GraspObjectExecutor::goal_callback()
{
    ROS_INFO("Received a new goal");
    current_goal_ = as_->acceptNewGoal();
    ROS_INFO("  Goal ID: %u", current_goal_->id);
    ROS_INFO("  Retry Count: %d", current_goal_->retry_count);
    ROS_INFO("  Gas Can Pose [world frame: %s]: %s", current_goal_->gas_can_in_map.header.frame_id.c_str(), to_string(current_goal_->gas_can_in_map.pose).c_str());
    ROS_INFO("  Gas Can Pose [robot frame: %s]: %s", current_goal_->gas_can_in_base_link.header.frame_id.c_str(), to_string(current_goal_->gas_can_in_base_link.pose).c_str());
    ROS_INFO("  Octomap ID: %s", current_goal_->octomap.id.c_str());

    sent_move_arm_goal_ = false;
    pending_move_arm_command_ = false;

    sent_viservo_command_ = false;
    pending_viservo_command_ = false;

    sent_gripper_command_ = false;
    pending_gripper_command_ = false;

    next_stow_position_to_attempt_ = 0;

    // make a hard copy of the grid
    if (last_occupancy_grid_) {
        current_occupancy_grid_.reset(new nav_msgs::OccupancyGrid);
        *current_occupancy_grid_ = *last_occupancy_grid_;
    }

    // get the gas can pose in the frame of the occupancy grid
    if (current_occupancy_grid_) {
        const std::string& grid_frame = current_occupancy_grid_->header.frame_id;
        const std::string& world_frame = current_goal_->gas_can_in_map.header.frame_id;

        // clear gas_can_in_grid_frame_
        gas_can_in_grid_frame_.header.frame_id = "";
        gas_can_in_grid_frame_.header.stamp = ros::Time(0);
        gas_can_in_grid_frame_.header.seq = 0;
        gas_can_in_grid_frame_.pose = geometry_msgs::IdentityPose();

        if (grid_frame != world_frame) {
            ROS_INFO("Transforming gas can into frame '%s' to clear from the occupancy grid", grid_frame.c_str());
            try {
                geometry_msgs::PoseStamped gas_can_in_world_frame = current_goal_->gas_can_in_map;
                gas_can_in_world_frame.header.stamp = ros::Time(0);

                gas_can_in_grid_frame_.header.frame_id = current_occupancy_grid_->header.frame_id;
                gas_can_in_grid_frame_.header.stamp = ros::Time(0);

                listener_.transformPose(grid_frame, gas_can_in_world_frame, gas_can_in_grid_frame_);
            }
            catch (const tf::TransformException& ex) {
                ROS_ERROR("Failed to lookup transform gas can into the grid frame. Assuming Identity");
                ROS_ERROR("%s", ex.what());
                gas_can_in_grid_frame_ = current_goal_->gas_can_in_map;
            }
        }
        else {
            ROS_INFO("Gas can already exists in grid frame '%s'", grid_frame.c_str());
            gas_can_in_grid_frame_ = current_goal_->gas_can_in_map;
            gas_can_in_grid_frame_.header.stamp = ros::Time(0);
        }

        ROS_INFO("Gas Can Pose [grid frame: %s]: %s", grid_frame.c_str(), to_string(gas_can_in_grid_frame_.pose).c_str());

        // todo: save all the cells that didn't need to be cleared, so that we can check for their existence later

        clear_circle_from_grid(
                *current_occupancy_grid_,
                gas_can_in_grid_frame_.pose.position.x,
                gas_can_in_grid_frame_.pose.position.y,
                object_filter_radius_m_);

        filtered_costmap_pub_.publish(current_occupancy_grid_);
    }


    if (use_extrusion_octomap_ && current_occupancy_grid_) {
        const double HARDCODED_EXTRUSION = 2.0; // Extrude the occupancy grid from 0m to 2m
//        current_octomap_ = extruder_.extrude(*current_occupancy_grid_, HARDCODED_EXTRUSION);
        extruder_.extrude(*current_occupancy_grid_, HARDCODED_EXTRUSION, *current_octomap_);
        if (current_octomap_) {
            extrusion_octomap_pub_.publish(current_octomap_);
        }
    }
}

void GraspObjectExecutor::preempt_callback()
{

}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onIdle()
{
    if (as_->isActive()) {
        if (use_extrusion_octomap_ &&
            (!current_occupancy_grid_ || !current_octomap_))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
            std::string msg = (bool)current_occupancy_grid_ ?
                    "Failed to extrude Occupancy Grid" : "Have yet to receive Occupancy Grid";
            ROS_WARN("%s", msg.c_str());
            as_->setAborted(result, msg.c_str());
            return GraspObjectExecutionStatus::FAULT;
        }
        else {
            return GraspObjectExecutionStatus::GENERATING_GRASPS;
        }
    }

    return GraspObjectExecutionStatus::IDLE;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onFault()
{
    if (as_->isActive()) {
        if (use_extrusion_octomap_ &&
            (!current_occupancy_grid_ || !current_octomap_))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
            std::string msg = (bool)current_occupancy_grid_ ?
                    "Failed to extrude Occupancy Grid" : "Have yet to receive Occupancy Grid";
            ROS_WARN("%s", msg.c_str());
            as_->setAborted(result, msg.c_str());
            return GraspObjectExecutionStatus::FAULT;
        } else {
            return GraspObjectExecutionStatus::GENERATING_GRASPS;
        }
    }

    return GraspObjectExecutionStatus::FAULT;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onGeneratingGrasps()
{
    Eigen::Affine3d base_link_to_gas_canister;
    tf::poseMsgToEigen(current_goal_->gas_can_in_base_link.pose, base_link_to_gas_canister);

    // 1. generate grasp candidates (poses of the wrist in the robot frame) from the object pose
    int max_num_candidates = 100;
    std::vector<rcta::GraspCandidate> grasp_candidates;
    if (!m_grasp_planner.sampleGrasps(
            base_link_to_gas_canister, max_num_candidates, grasp_candidates))
    {
        // TODO: hmm
    }
    ROS_INFO("Sampled %zd grasp poses", grasp_candidates.size());

    SV_SHOW_INFO(getGraspCandidatesVisualization(grasp_candidates, "candidate_grasp"));

    // mount -> wrist = mount -> robot * robot -> wrist

    const std::string robot_frame = current_goal_->gas_can_in_base_link.header.frame_id;
    const std::string kinematics_frame = "arm_mount_panel_dummy";
    const std::string camera_view_frame = "camera_rgb_optical_frame";
    tf::StampedTransform tf_transform;
    tf::StampedTransform T_robot_camera_tf;
    try {
        listener_.lookupTransform(robot_frame, kinematics_frame, ros::Time(0), tf_transform);
        listener_.lookupTransform(robot_frame, camera_view_frame, ros::Time(0), T_robot_camera_tf);
    }
    catch (const tf::TransformException& ex) {
        rcta_msgs::GraspObjectCommandResult result;
        result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
        std::stringstream ss;
        ss << "Failed to lookup transform " << robot_frame << " -> " << kinematics_frame << "; Unable to determine grasp reachability";
        ROS_WARN("%s", ss.str().c_str());
        as_->setAborted(result, ss.str());
        return GraspObjectExecutionStatus::FAULT;
    }

    Eigen::Affine3d robot_to_kinematics;
    Eigen::Affine3d robot_to_camera;
    msg_utils::convert(tf_transform, robot_to_kinematics);
    msg_utils::convert(T_robot_camera_tf, robot_to_camera);

    // filter unreachable grasp candidates that we know we can't grasp
    std::swap(grasp_candidates, reachable_grasp_candidates_);
    filterGraspCandidates(
            reachable_grasp_candidates_,
            robot_to_kinematics.inverse(),
            robot_to_camera.inverse(),
            sbpl::utils::ToRadians(45.0));

    ROS_INFO("Produced %zd reachable grasp poses", reachable_grasp_candidates_.size());

    if (reachable_grasp_candidates_.empty()) {
        ROS_WARN("No reachable grasp candidates available");
        rcta_msgs::GraspObjectCommandResult result;
        result.result = rcta_msgs::GraspObjectCommandResult::OBJECT_OUT_OF_REACH;
        as_->setAborted(result, "No reachable grasp candidates available");
        return GraspObjectExecutionStatus::FAULT;
    }

    rcta::RankGrasps(reachable_grasp_candidates_); // rank grasp attempts by 'graspability'
    cull_grasp_candidates(reachable_grasp_candidates_, max_grasp_candidates_); // limit the number of grasp attempts by the configured amount

    ROS_INFO("Attempting %zd grasps", reachable_grasp_candidates_.size());

    return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onPlanningArmMotionToPregrasp()
{
    ////////////////////////////////////////////////////////////////////////////////////////
    // Mini state machine to monitor move arm command status
    ////////////////////////////////////////////////////////////////////////////////////////

    if (!sent_move_arm_goal_) {
        rcta_msgs::GraspObjectCommandFeedback feedback;
        feedback.status = execution_status_to_feedback_status(status_);
        as_->publishFeedback(feedback);

        if (reachable_grasp_candidates_.empty()) {
            ROS_WARN("Failed to plan to all reachable grasps");
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
            as_->setAborted(result, "Failed on all reachable grasps");
            return GraspObjectExecutionStatus::FAULT;
        }

        ROS_WARN("Sending Move Arm Goal to pregrasp pose");
        if (!wait_for_action_server(
                move_arm_command_client_,
                move_arm_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            std::stringstream ss; ss << "Failed to connect to '" << move_arm_command_action_name_ << "' action server";
            ROS_ERROR("%s", ss.str().c_str());
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
            as_->setAborted(result, ss.str());
            return GraspObjectExecutionStatus::FAULT;
        }

        const rcta::GraspCandidate& next_best_grasp = reachable_grasp_candidates_.back();

        // 4. send a move arm goal for the best grasp
        last_move_arm_pregrasp_goal_.type = rcta::MoveArmGoal::EndEffectorGoal;
        tf::poseEigenToMsg(next_best_grasp.pose, last_move_arm_pregrasp_goal_.goal_pose);

        last_move_arm_pregrasp_goal_.octomap = use_extrusion_octomap_ ?
                *current_octomap_ : current_goal_->octomap;

        last_move_arm_pregrasp_goal_.execute_path = true;

        auto result_cb = boost::bind(&GraspObjectExecutor::move_arm_command_result_cb, this, _1, _2);
        move_arm_command_client_->sendGoal(last_move_arm_pregrasp_goal_, result_cb);

        pending_move_arm_command_ = true;
        sent_move_arm_goal_ = true;

        reachable_grasp_candidates_.pop_back();
        last_successful_grasp_ = next_best_grasp;
    } else if (!pending_move_arm_command_) {
        // NOTE: short-circuiting "EXECUTING_ARM_MOTION_TO_PREGRASP" for
        // now since the move_arm action handles execution and there is
        // presently no feedback to distinguish planning vs. execution

        ROS_INFO("Move Arm Goal is no longer pending");
        if (move_arm_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED &&
            move_arm_command_result_ && move_arm_command_result_->success)
        {
            ROS_INFO("Move Arm Command succeeded");
            return GraspObjectExecutionStatus::OPENING_GRIPPER;
        }
        else {
            ROS_INFO("Move Arm Command failed");
            ROS_INFO("    Simple Client Goal State: %s", move_arm_command_goal_state_.toString().c_str());
            ROS_INFO("    Error Text: %s", move_arm_command_goal_state_.getText().c_str());
            ROS_INFO("    result.success = %s", move_arm_command_result_ ? (move_arm_command_result_->success ? "TRUE" : "FALSE") : "null");
            // stay in PLANNING_ARM_MOTION_TO_PREGRASP until there are no more grasps
            // TODO: consider moving back to the stow position
        }

        sent_move_arm_goal_ = false; // reset for future move arm goals
    }

    return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onExecutingArmMotionToPregrasp()
{
    return GraspObjectExecutionStatus::EXECUTING_ARM_MOTION_TO_PREGRASP;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onOpeningGripper()
{
    ////////////////////////////////////////////////////////////////////////////////////////
    // Mini state machine to monitor gripper command status
    ////////////////////////////////////////////////////////////////////////////////////////

    // send gripper command upon entering
    if (!sent_gripper_command_) {
        ROS_WARN("Sending Gripper Goal to open gripper");
        if (!wait_for_action_server(
                gripper_command_client_,
                gripper_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            std::stringstream ss; ss << "Failed to connect to '" << gripper_command_action_name_ << "' action server";
            as_->setAborted(result, ss.str());
            ROS_ERROR("%s", ss.str().c_str());
            return GraspObjectExecutionStatus::FAULT;
        }

        control_msgs::GripperCommandGoal gripper_goal;
        gripper_goal.command.position = GripperModel().maximum_width();
        gripper_goal.command.max_effort = GripperModel().maximum_force();

        auto result_cb = boost::bind(&GraspObjectExecutor::gripper_command_result_cb, this, _1, _2);
        gripper_command_client_->sendGoal(gripper_goal, result_cb);

        pending_gripper_command_ = true;
        sent_gripper_command_ = true;
    }

    // wait for gripper command to finish
    if (!pending_gripper_command_) {
        ROS_INFO("Gripper Goal to open gripper is no longer pending");

        sent_gripper_command_ = false; // reset for future gripper goals upon exiting
        if (gripper_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED &&
            (gripper_command_result_->reached_goal || gripper_command_result_->stalled))
        {
            ROS_INFO("Gripper Command Succeeded");
            if (gripper_command_result_->stalled) {
                ROS_WARN("    Open Gripper Command Succeeded but Stalled During Execution");
            }
            return GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_PREGRASP;
        }
        else {
            ROS_WARN("Open Gripper Command failed");
            ROS_WARN("    Simple Client Goal State: %s", gripper_command_goal_state_.toString().c_str());
            ROS_WARN("    Error Text: %s", gripper_command_goal_state_.getText().c_str());
            ROS_WARN("    result.reached_goal = %s", gripper_command_result_ ? (gripper_command_result_->reached_goal ? "TRUE" : "FALSE") : "null");
            ROS_WARN("    result.stalled = %s", gripper_command_result_ ? (gripper_command_result_->stalled ? "TRUE" : "FALSE") : "null");
            return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP;
        }
    }

    return GraspObjectExecutionStatus::OPENING_GRIPPER;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onExecutingVisualServoMotionToPregrasp()
{
    if (!sent_viservo_command_) {
        ROS_WARN("Sending Viservo Goal to pregrasp pose");

        if (!wait_for_action_server(
                viservo_command_client_,
                viservo_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            std::stringstream ss; ss << "Failed to connect to '" << viservo_command_action_name_ << "' action server";
            as_->setAborted(result, ss.str());
            ROS_ERROR("%s", ss.str().c_str());
            return GraspObjectExecutionStatus::FAULT;
        }

        const std::string kinematics_frame = "arm_mount_panel_dummy";
        const std::string camera_frame = "camera_rgb_frame";

        tf::StampedTransform tf_transform;
        try {
            listener_.lookupTransform(camera_frame, kinematics_frame, ros::Time(0), tf_transform);
        }
        catch (const tf::TransformException& ex) {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            std::stringstream ss;
            ss << "Failed to lookup transform " << kinematics_frame << " -> " << camera_frame << "; Unable to determine viservo goal";
            as_->setAborted(result, ss.str());
            return GraspObjectExecutionStatus::FAULT;
        }

        Eigen::Affine3d camera_to_kinematics;
        msg_utils::convert(tf_transform, camera_to_kinematics);

        // camera -> kinematics * kinematics -> wrist
        Eigen::Affine3d kinematics_to_wrist;
        tf::poseMsgToEigen(last_move_arm_pregrasp_goal_.goal_pose, kinematics_to_wrist);

        Eigen::Affine3d viservo_pregrasp_goal_transform =
                camera_to_kinematics * kinematics_to_wrist;

        last_viservo_pregrasp_goal_.goal_id = current_goal_->id;
        tf::poseEigenToMsg(viservo_pregrasp_goal_transform, last_viservo_pregrasp_goal_.goal_pose);

        auto result_cb = boost::bind(&GraspObjectExecutor::viservo_command_result_cb, this, _1, _2);
        viservo_command_client_->sendGoal(last_viservo_pregrasp_goal_, result_cb);

        pending_viservo_command_ = true;
        sent_viservo_command_ = true;
    }
    else if (!pending_viservo_command_) {
        ROS_INFO("Viservo Goal to pregrasp is no longer pending");

        sent_viservo_command_ = false; // reset for future viservo goals
        if (viservo_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED &&
            viservo_command_result_->result == rcta::ViservoCommandResult::SUCCESS)
        {
            ROS_INFO("Viservo Command Succeeded");
            return GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_GRASP;
        }
        else {
            ROS_INFO("Viservo Command failed");
            ROS_INFO("    Simple Client Goal State: %s", viservo_command_goal_state_.toString().c_str());
            ROS_INFO("    Error Text: %s", viservo_command_goal_state_.getText().c_str());
            ROS_INFO("    result.result = %s", viservo_command_result_ ? (viservo_command_result_->result == rcta::ViservoCommandResult::SUCCESS? "SUCCESS" : "NOT SUCCESS") : "null");
            ROS_WARN("Failed to complete visual servo motion to pregrasp");
            return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP;
        }
    }

    return GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_PREGRASP;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onExecutingVisualServoMotionToGrasp()
{
    if (!sent_viservo_command_) {
        ROS_WARN("Sending Viservo Goal to grasp pose");

        if (!wait_for_action_server(
                viservo_command_client_,
                viservo_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            std::stringstream ss; ss << "Failed to connect to '" << viservo_command_action_name_ << "' action server";
            as_->setAborted(result, ss.str());
            ROS_ERROR("%s", ss.str().c_str());
            return GraspObjectExecutionStatus::FAULT;
        }

        Eigen::Affine3d viservo_grasp_goal_transform;
        tf::poseMsgToEigen(last_viservo_pregrasp_goal_.goal_pose, viservo_grasp_goal_transform);
        // camera -> pregrasp * pregrasp -> grasp = camera -> grasp
        viservo_grasp_goal_transform = viservo_grasp_goal_transform * m_grasp_planner.graspToPregrasp().inverse();

        last_viservo_grasp_goal_.goal_id = current_goal_->id;
        tf::poseEigenToMsg(viservo_grasp_goal_transform, last_viservo_grasp_goal_.goal_pose);

        auto result_cb = boost::bind(&GraspObjectExecutor::viservo_command_result_cb, this, _1, _2);
        viservo_command_client_->sendGoal(last_viservo_grasp_goal_, result_cb);

        pending_viservo_command_ = true;
        sent_viservo_command_ = true;;
    }
    else if (!pending_viservo_command_) {
        ROS_INFO("Viservo Goal to grasp is no longer pending");

        sent_viservo_command_ = false; // reset for future viservo goals
        if (viservo_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED &&
            viservo_command_result_->result == rcta::ViservoCommandResult::SUCCESS)
        {
            ROS_INFO("Viservo Command Succeeded");
            return GraspObjectExecutionStatus::GRASPING_OBJECT;
        }
        else {
            ROS_INFO("Viservo Command failed");
            ROS_INFO("    Simple Client Goal State: %s", viservo_command_goal_state_.toString().c_str());
            ROS_INFO("    Error Text: %s", viservo_command_goal_state_.getText().c_str());
            ROS_INFO("    result.result = %s", viservo_command_result_ ? (viservo_command_result_->result == rcta::ViservoCommandResult::SUCCESS? "SUCCESS" : "NOT SUCCESS (lol)") : "null");
            ROS_WARN("Failed to complete visual servo motion to grasp");
            return GraspObjectExecutionStatus::FAULT;
        }
    }

    return GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_GRASP;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onGraspingObject()
{
    // send the gripper command to close the gripper
    if (!sent_gripper_command_) {
        ROS_WARN("Sending Gripper Goal to close gripper");
        if (!wait_for_action_server(
                gripper_command_client_,
                gripper_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            std::stringstream ss; ss << "Failed to connect to '" << gripper_command_action_name_ << "' action server";
            as_->setAborted(result, ss.str());
            ROS_ERROR("%s", ss.str().c_str());
            return GraspObjectExecutionStatus::FAULT;
        }

        control_msgs::GripperCommandGoal gripper_goal;
        gripper_goal.command.max_effort = GripperModel().maximum_force();
        gripper_goal.command.position = GripperModel().minimum_width();

        auto result_cb = boost::bind(&GraspObjectExecutor::gripper_command_result_cb, this, _1, _2);
        gripper_command_client_->sendGoal(gripper_goal, result_cb);

        pending_gripper_command_ = true;
        sent_gripper_command_ = true;
    }

    // check whether we've grabbed the object
    if (!pending_gripper_command_) {
        ROS_INFO("Gripper Goal to close gripper is no longer pending");

        const bool grabbed_object = gripper_command_result_->reached_goal || gripper_command_result_->stalled;

        // note: reverting to the old method since the gripper does not
        // think that it has made contact with an object when a wraparound grasp is performed
//                const bool grabbed_object = !gripper_command_result_->reached_goal || gripper_command_result_->stalled;

        sent_gripper_command_ = false; // reset for future gripper goals
        if (gripper_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED && grabbed_object) {
            ROS_INFO("Gripper Command Succeeded");
            return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_STOW_POSITION;
        }
        else {
            ROS_WARN("Close Gripper Command failed");
            ROS_WARN("    Simple Client Goal State: %s", gripper_command_goal_state_.toString().c_str());
            ROS_WARN("    Error Text: %s", gripper_command_goal_state_.getText().c_str());
            ROS_WARN("    result.reached_goal = %s", gripper_command_result_ ? (gripper_command_result_->reached_goal ? "TRUE" : "FALSE") : "null");
            ROS_WARN("    result.stalled = %s", gripper_command_result_ ? (gripper_command_result_->stalled ? "TRUE" : "FALSE") : "null");
            return GraspObjectExecutionStatus::RETRACTING_GRIPPER;
        }
    }

    return GraspObjectExecutionStatus::GRASPING_OBJECT;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onRetractingGripper()
{
    // send the first gripper command to close the gripper
    if (!sent_gripper_command_) {
        ROS_WARN("Sending Gripper Goal to retract gripper");
        if (!wait_for_action_server(
                gripper_command_client_,
                gripper_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            std::stringstream ss; ss << "Failed to connect to '" << gripper_command_action_name_ << "' action server";
            as_->setAborted(result, ss.str());
            ROS_ERROR("%s", ss.str().c_str());
            return GraspObjectExecutionStatus::FAULT;
        }

        control_msgs::GripperCommandGoal gripper_goal;
        gripper_goal.command.max_effort = GripperModel().maximum_force();
        gripper_goal.command.position = GripperModel().maximum_width();

        auto result_cb = boost::bind(&GraspObjectExecutor::gripper_command_result_cb, this, _1, _2);
        gripper_command_client_->sendGoal(gripper_goal, result_cb);

        pending_gripper_command_ = true;
        sent_gripper_command_ = true;
    }

    // check whether we've grabbed the object
    if (!pending_gripper_command_) {
        ROS_INFO("Gripper Goal to retract gripper is no longer pending");

        if (gripper_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED) {
            ROS_INFO("Gripper Command Succeeded");
        }
        else {
            ROS_INFO("Close Gripper Command failed");
            ROS_INFO("    Simple Client Goal State: %s", gripper_command_goal_state_.toString().c_str());
            ROS_INFO("    Error Text: %s", gripper_command_goal_state_.getText().c_str());
            ROS_INFO("    result.reached_goal = %s", gripper_command_result_ ? (gripper_command_result_->reached_goal ? "TRUE" : "FALSE") : "null");
            ROS_INFO("    result.stalled = %s", gripper_command_result_ ? (gripper_command_result_->stalled ? "TRUE" : "FALSE") : "null");
        }

        sent_gripper_command_ = false; // reset for future gripper goals
        return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP; // Transfer back to planning regardless
    }

    return GraspObjectExecutionStatus::RETRACTING_GRIPPER;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onPlanningArmMotionToStowPosition()
{
    if (!sent_move_arm_goal_) {
        rcta_msgs::GraspObjectCommandFeedback feedback;
        feedback.status = execution_status_to_feedback_status(status_);
        as_->publishFeedback(feedback);

        ROS_INFO("Sending Move Arm Goal to stow position");
        if (!wait_for_action_server(
                move_arm_command_client_,
                move_arm_command_action_name_,
                ros::Duration(0.1),
                ros::Duration(5.0)))
        {
            std::stringstream ss; ss << "Failed to connect to '" << move_arm_command_action_name_ << "' action server";
            ROS_ERROR("%s", ss.str().c_str());
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
            as_->setAborted(result, ss.str());
            next_stow_position_to_attempt_ = 0;
            return GraspObjectExecutionStatus::FAULT;
        }

        if (next_stow_position_to_attempt_ >= stow_positions_.size()) {
            rcta_msgs::GraspObjectCommandResult result;
            std::string error = "Ran out of stow positions to attempt";
            ROS_ERROR("%s", error.c_str());
            result.result = rcta_msgs::GraspObjectCommandResult::PLANNING_FAILED;
            as_->setAborted(result, error);
            next_stow_position_to_attempt_ = 0;
            return GraspObjectExecutionStatus::FAULT;
        }

        // 4. send a move arm goal for the best grasp

        const StowPosition& next_stow_position = stow_positions_[next_stow_position_to_attempt_++];

        last_move_arm_stow_goal_.type = rcta::MoveArmGoal::JointGoal;
        last_move_arm_stow_goal_.goal_joint_state.name = robot_model_->joint_names();
        last_move_arm_stow_goal_.goal_joint_state.position = next_stow_position.joint_positions;

        last_move_arm_stow_goal_.octomap = use_extrusion_octomap_ ?
                *current_octomap_ : current_goal_->octomap;

        //include the attached object in the goal
        // TODO: include the attached object here

        moveit_msgs::AttachedCollisionObject attached_object;// = last_move_arm_stow_goal_.attached_object;
        attached_object.link_name = "arm_7_gripper_lift_link";
        attached_object.object.id = "gas_can";
        attached_object.object.primitives.resize(2);
        attached_object.object.primitive_poses.resize(2);
        attached_object.object.primitives[0].type = shape_msgs::SolidPrimitive::BOX;
        attached_object.object.primitives[0].dimensions.resize(3);
        //rough gas can dimensions
        attached_object.object.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_X] = 0.20;
        attached_object.object.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Y] = 0.25;
        attached_object.object.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Z] = 0.25;
        //nozzle
        attached_object.object.primitives[1].type = shape_msgs::SolidPrimitive::CYLINDER;
        attached_object.object.primitives[1].dimensions.resize(2);
        attached_object.object.primitives[1].dimensions[shape_msgs::SolidPrimitive::CYLINDER_HEIGHT] = 0.35;
        attached_object.object.primitives[1].dimensions[shape_msgs::SolidPrimitive::CYLINDER_RADIUS] = 0.035;

        //compute the pose of attachment based on grasp pose used
        Eigen::Affine3d obj_body_offset_;
        tf::Transform obj_body_offset_tf_(tf::Quaternion::getIdentity(), tf::Vector3(0.0, 0.0, -0.05));
        tf::poseTFToEigen(obj_body_offset_tf_, obj_body_offset_);
        Eigen::Affine3d obj_body_in_grasp_frame = last_successful_grasp_.pose_in_object.inverse() * obj_body_offset_;
        tf::poseEigenToMsg(obj_body_in_grasp_frame, attached_object.object.primitive_poses[0]);
        //nozzle
        Eigen::Affine3d obj_nozzle_offset_;
        tf::Transform obj_nozzle_offset_tf_ = tf::Transform(tf::Quaternion(tf::Vector3(1,0,0), 0.25*M_PI), tf::Vector3(0.0, 0.0, -0.05)) * tf::Transform(tf::Quaternion::getIdentity(), tf::Vector3(0.0, 0.0, 0.15));
        tf::poseTFToEigen(obj_nozzle_offset_tf_, obj_nozzle_offset_);
        Eigen::Affine3d obj_nozzle_in_grasp_frame = last_successful_grasp_.pose_in_object.inverse() * obj_nozzle_offset_;
        tf::poseEigenToMsg(obj_nozzle_in_grasp_frame, attached_object.object.primitive_poses[1]);

        ROS_WARN("Using attached object!");
        ROS_WARN("Object attached to xyz(%.3f, %.3f, %.3f) q_xyzw(%.3f, %.3f, %.3f, %.3f) relative to wrist!",
                    attached_object.object.primitive_poses[0].position.x,
                    attached_object.object.primitive_poses[0].position.y,
                    attached_object.object.primitive_poses[0].position.z,
                    attached_object.object.primitive_poses[0].orientation.x,
                    attached_object.object.primitive_poses[0].orientation.y,
                    attached_object.object.primitive_poses[0].orientation.z,
                    attached_object.object.primitive_poses[0].orientation.w);

        attached_object.object.primitive_poses[0].position.x -= 0.15;
        attached_object.object.primitive_poses[1].position.x -= 0.15;
        attached_object.weight = 1.0; //arbitrary (not used anyways)
        last_move_arm_stow_goal_.execute_path = true;

        auto result_cb = boost::bind(&GraspObjectExecutor::move_arm_command_result_cb, this, _1, _2);
        move_arm_command_client_->sendGoal(last_move_arm_stow_goal_, result_cb);

        pending_move_arm_command_ = true;
        sent_move_arm_goal_ = true;
    }
    else if (!pending_move_arm_command_) {
        // NOTE: short-circuiting "EXECUTING_ARM_MOTION_TO_PREGRASP" for
        // now since the move_arm action handles execution and there is
        // presently no feedback to distinguish planning vs. execution

        sent_move_arm_goal_ = false; // reset for future move arm goals
        ROS_INFO("Move Arm Goal is no longer pending");
        if (move_arm_command_goal_state_ == actionlib::SimpleClientGoalState::SUCCEEDED &&
            move_arm_command_result_ && move_arm_command_result_->success)
        {
            ROS_INFO("Move Arm Command succeeded");
            next_stow_position_to_attempt_ = 0;
            return GraspObjectExecutionStatus::COMPLETING_GOAL;
        }
        else {
            ROS_INFO("Move Arm Command failed");
            ROS_INFO("    Simple Client Goal State: %s", move_arm_command_goal_state_.toString().c_str());
            ROS_INFO("    Error Text: %s", move_arm_command_goal_state_.getText().c_str());
            ROS_INFO("    result.success = %s", move_arm_command_result_ ? (move_arm_command_result_->success ? "TRUE" : "FALSE") : "null");
        }
    }

    return GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_STOW_POSITION;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onExecutingArmMotionToStowPosition()
{
    return GraspObjectExecutionStatus::EXECUTING_ARM_MOTION_TO_STOW_POSITION;
}

GraspObjectExecutionStatus::Status GraspObjectExecutor::onCompletingGoal()
{
    if (!wait_for_after_grasp_grid_) {
        ros::Time now = ros::Time::now();
        ROS_INFO("Waiting for a costmap more recent than %s", boost::posix_time::to_simple_string(now.toBoost()).c_str());
        wait_for_grid_start_time_ = now;
        wait_for_after_grasp_grid_ = true;
    }

    if (last_occupancy_grid_ && last_occupancy_grid_->header.stamp > wait_for_grid_start_time_) {
        ROS_INFO("Received a fresh costmap");
        // get here once we've received a newer costmap to evaluate for the object's position
        wait_for_after_grasp_grid_ = false;
        occupancy_grid_after_grasp_ = last_occupancy_grid_;

        double success_pct = calc_prob_successful_grasp(
            *occupancy_grid_after_grasp_,
            gas_can_in_grid_frame_.pose.position.x,
            gas_can_in_grid_frame_.pose.position.y,
            object_filter_radius_m_);

        occupancy_grid_after_grasp_.reset();

        success_pct = clamp(success_pct, 0.0, 1.0);
        if (success_pct == 1.0) {
            ROS_WARN("Mighty confident now, aren't we?");
        }
        else {
            ROS_WARN("Grasped object with %0.3f %% confidence", 100.0 * success_pct);
        }

        if (success_pct > gas_can_detection_threshold_) {
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::SUCCESS;
            as_->setSucceeded(result);
            return GraspObjectExecutionStatus::IDLE;
        }
        else {
            std::string message = "It appears that we have likely not grasped the object";
            ROS_WARN("%s", message.c_str());
            rcta_msgs::GraspObjectCommandResult result;
            result.result = rcta_msgs::GraspObjectCommandResult::EXECUTION_FAILED;
            as_->setAborted(result, message);
            return GraspObjectExecutionStatus::FAULT;
        }
    }

    return GraspObjectExecutionStatus::COMPLETING_GOAL;
}

void GraspObjectExecutor::move_arm_command_active_cb()
{

}

void GraspObjectExecutor::move_arm_command_feedback_cb(const rcta::MoveArmFeedback::ConstPtr& feedback)
{

}

void GraspObjectExecutor::move_arm_command_result_cb(
    const actionlib::SimpleClientGoalState& state,
    const rcta::MoveArmResult::ConstPtr& result)
{
    move_arm_command_goal_state_ = state;
    move_arm_command_result_ = result;
    pending_move_arm_command_ = false;
}

void GraspObjectExecutor::viservo_command_active_cb()
{

}

void GraspObjectExecutor::viservo_command_feedback_cb(const rcta::ViservoCommandFeedback::ConstPtr& feedback)
{

}

void GraspObjectExecutor::viservo_command_result_cb(
    const actionlib::SimpleClientGoalState& state,
    const rcta::ViservoCommandResult::ConstPtr& result)
{
    viservo_command_goal_state_ = state;
    viservo_command_result_ = result;
    pending_viservo_command_ = false;
}

void GraspObjectExecutor::gripper_command_active_cb()
{

}

void GraspObjectExecutor::gripper_command_feedback_cb(const control_msgs::GripperCommandFeedback::ConstPtr& feedback)
{
}

void GraspObjectExecutor::gripper_command_result_cb(
    const actionlib::SimpleClientGoalState& state,
    const control_msgs::GripperCommandResult::ConstPtr& result)
{
    gripper_command_goal_state_ = state;
    gripper_command_result_ = result;
    pending_gripper_command_ = false;
}

void GraspObjectExecutor::occupancy_grid_cb(const nav_msgs::OccupancyGrid::ConstPtr& msg)
{
    last_occupancy_grid_ = msg;
}

uint8_t GraspObjectExecutor::execution_status_to_feedback_status(GraspObjectExecutionStatus::Status status)
{
    switch (status) {
    case GraspObjectExecutionStatus::IDLE:
        return -1;
    case GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_PREGRASP:
        return rcta_msgs::GraspObjectCommandFeedback::EXECUTING_ARM_MOTION_TO_PREGRASP;
    case GraspObjectExecutionStatus::EXECUTING_ARM_MOTION_TO_PREGRASP:
        return rcta_msgs::GraspObjectCommandFeedback::EXECUTING_ARM_MOTION_TO_PREGRASP;
    case GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_PREGRASP:
        return rcta_msgs::GraspObjectCommandFeedback::EXECUTING_VISUAL_SERVO_MOTION_TO_PREGRASP;
    case GraspObjectExecutionStatus::EXECUTING_VISUAL_SERVO_MOTION_TO_GRASP:
        return rcta_msgs::GraspObjectCommandFeedback::EXECUTING_VISUAL_SERVO_MOTION_TO_GRASP;
    case GraspObjectExecutionStatus::GRASPING_OBJECT:
        return rcta_msgs::GraspObjectCommandFeedback::GRASPING_OBJECT;
    case GraspObjectExecutionStatus::PLANNING_ARM_MOTION_TO_STOW_POSITION:
        return rcta_msgs::GraspObjectCommandFeedback::PLANNING_ARM_MOTION_TO_STOW;
    case GraspObjectExecutionStatus::EXECUTING_ARM_MOTION_TO_STOW_POSITION:
        return rcta_msgs::GraspObjectCommandFeedback::EXECUTING_ARM_MOTION_TO_STOW;
    case GraspObjectExecutionStatus::COMPLETING_GOAL:
        return -1;
    default:
        return -1;
    }
}

void GraspObjectExecutor::filterGraspCandidates(
    std::vector<rcta::GraspCandidate>& candidates,
    const Eigen::Affine3d& T_kinematics_robot,  // keep these names to indicate improper usage
    const Eigen::Affine3d& T_camera_robot,      // keep these names to indicate improper usage
    double marker_incident_angle_threshold_rad) const
{
    const Eigen::Affine3d& camera_pose = T_camera_robot;

    EigenSTL::vector_Affine3d marker_poses;
    marker_poses.reserve(attached_markers_.size());
    for (const auto& marker : attached_markers_) {
        marker_poses.push_back(marker.link_to_marker);
    }

    rcta::PruneGraspsByVisibility(
            candidates,
            marker_poses,
            camera_pose,
            marker_incident_angle_threshold_rad);
}

void GraspObjectExecutor::cull_grasp_candidates(std::vector<rcta::GraspCandidate>& candidates, int max_candidates) const
{
	std::reverse(candidates.begin(), candidates.end());
	while (candidates.size() > max_candidates) {
		candidates.pop_back();
	}
	std::reverse(candidates.begin(), candidates.end());
}

visualization_msgs::MarkerArray
GraspObjectExecutor::getGraspCandidatesVisualization(
    const std::vector<rcta::GraspCandidate>& grasps,
    const std::string& ns) const
{
    const std::string& frame_id = current_goal_->gas_can_in_base_link.header.frame_id;
    return rcta::GetGraspCandidatesVisualization(grasps, frame_id, ns);
}

void GraspObjectExecutor::clear_circle_from_grid(
    nav_msgs::OccupancyGrid& grid,
    double circle_x, double circle_y,
    double circle_radius) const
{
    ROS_INFO("Clearing circle at (%0.3f, %0.3f) with radius %0.3f from costmap", circle_x, circle_y, circle_radius);
    Eigen::Vector2d circle_center(circle_x, circle_y);

    // get the min and max grid coordinates to scan
    Eigen::Vector2i min_grid, max_grid;
    world_to_grid(grid, circle_x - circle_radius, circle_y - circle_radius, min_grid(0), min_grid(1));
    max_grid(0) = (int)std::ceil((circle_x + circle_radius - grid.info.origin.position.x) / grid.info.resolution);
    max_grid(1) = (int)std::ceil((circle_y + circle_radius - grid.info.origin.position.y) / grid.info.resolution);

    ROS_INFO("Clearing circle points within [%d, %d] x [%d, %d]", min_grid(0), min_grid(1), max_grid(0), max_grid(1));

    int num_cleared = 0;
    for (int grid_x = min_grid(0); grid_x < max_grid(0); ++grid_x) {
        for (int grid_y = min_grid(1); grid_y <  max_grid(1); ++grid_y) {
            Eigen::Vector2i gp(grid_x, grid_y);
            if (!within_bounds(grid, gp(0), gp(1))) {
                ROS_WARN("Grid point (%d, %d) is outside of costmap bounds", gp(0), gp(1));
                continue;
            }

            Eigen::Vector2d wp;
            grid_to_world(grid, grid_x, grid_y, wp(0), wp(1));

            Eigen::Vector2d bl = wp - Eigen::Vector2d(-0.5 * grid.info.resolution, -0.5 * grid.info.resolution);
            Eigen::Vector2d br = wp - Eigen::Vector2d(0.5 * grid.info.resolution, -0.5 * grid.info.resolution);
            Eigen::Vector2d tr = wp - Eigen::Vector2d(0.5 * grid.info.resolution, 0.5 * grid.info.resolution);
            Eigen::Vector2d tl = wp - Eigen::Vector2d(-0.5 * grid.info.resolution, 0.5 * grid.info.resolution);
            if ((bl - circle_center).norm() <= circle_radius ||
                (br - circle_center).norm() <= circle_radius ||
                (tr - circle_center).norm() <= circle_radius ||
                (tl - circle_center).norm() <= circle_radius)
            {
                grid_at(grid, gp(0), gp(1)) = 0;
                ++num_cleared;
                ROS_INFO("Clearing cell (%d, %d)", gp(0), gp(1));
            }
        }
    }

    ROS_INFO("Cleared %d cells", num_cleared);
}

bool GraspObjectExecutor::within_bounds(const nav_msgs::OccupancyGrid& grid, int x, int y) const
{
    return x >= 0 && x < grid.info.width && y >= 0 && y < grid.info.height;
}

void GraspObjectExecutor::grid_to_world(
    const nav_msgs::OccupancyGrid& grid,
    int grid_x,
    int grid_y,
    double& world_x,
    double& world_y) const
{
    world_x = grid_x * grid.info.resolution + 0.5 * grid.info.resolution + grid.info.origin.position.x;
    world_y = grid_y * grid.info.resolution + 0.5 * grid.info.resolution + grid.info.origin.position.y;
}

void GraspObjectExecutor::world_to_grid(
    const nav_msgs::OccupancyGrid& grid,
    double world_x,
    double world_y,
    int& grid_x,
    int& grid_y) const
{
    grid_x = (int)((world_x - grid.info.origin.position.x) / grid.info.resolution);
    grid_y = (int)((world_y - grid.info.origin.position.y) / grid.info.resolution);
}

std::int8_t& GraspObjectExecutor::grid_at(nav_msgs::OccupancyGrid& grid, int grid_x, int grid_y) const
{
    return grid.data[grid_y * grid.info.width + grid_x];
}

const std::int8_t& GraspObjectExecutor::grid_at(const nav_msgs::OccupancyGrid& grid, int grid_x, int grid_y) const
{
    return grid.data[grid_y * grid.info.width + grid_x];
}

double GraspObjectExecutor::calc_prob_successful_grasp(
    const nav_msgs::OccupancyGrid& grid,
    double circle_x,
    double circle_y,
    double circle_radius) const
{
    au::vector2d mean = { circle_x, circle_y };
    au::matrix2d covariance(au::matrix2d::zeros());
    covariance(0, 0) = 0.1;
    covariance(1, 1) = 0.1;
    au::gaussian_distribution<2> gauss(mean, covariance);

    ROS_INFO("Setting up gaussian with mean (%0.3f, %0.3f) and covariance (%0.3f, %0.3f)", mean[0], mean[1], covariance(0, 0), covariance(1, 1));

    ROS_INFO("Evaluating circle at (%0.3f, %0.3f) with radius %0.3f from costmap", circle_x, circle_y, circle_radius);
    Eigen::Vector2d circle_center(circle_x, circle_y);

    // get the min and max grid coordinates to scan
    Eigen::Vector2i min_grid, max_grid;
    world_to_grid(grid, circle_x - circle_radius, circle_y - circle_radius, min_grid(0), min_grid(1));
    max_grid(0) = (int)std::ceil((circle_x + circle_radius - grid.info.origin.position.x) / grid.info.resolution);
    max_grid(1) = (int)std::ceil((circle_y + circle_radius - grid.info.origin.position.y) / grid.info.resolution);

    int num_x_cells = max_grid(0) - min_grid(0);
    int num_y_cells = max_grid(1) - min_grid(1);
    ROS_INFO("Instantiating %d x %d probability mask", num_x_cells, num_y_cells);
    Eigen::MatrixXd mask(num_x_cells, num_y_cells);
    for (int i = 0; i < num_x_cells; ++i) {
        for (int j = 0; j < num_y_cells; ++j) {
            mask(i, j) = 0.0;
        }
    }

    ROS_INFO("Initializing probability mask over [%d, %d] x [%d, %d]", min_grid(0), min_grid(1), max_grid(0), max_grid(1));
    // initialize the mask to the gaussian values in each cell
    double sum = 0.0;
    for (int grid_x = min_grid(0); grid_x < max_grid(0); ++grid_x) {
        for (int grid_y = min_grid(1); grid_y <  max_grid(1); ++grid_y) {

            Eigen::Vector2i gp(grid_x, grid_y);
            if (!within_bounds(grid, gp(0), gp(1))) {
                ROS_WARN("Grid point (%d, %d) is outside of costmap bounds", gp(0), gp(1));
                continue;
            }

            Eigen::Vector2d wp;
            grid_to_world(grid, grid_x, grid_y, wp(0), wp(1));

            // calculate the probability modulus for this cell
            int xrow = grid_x - min_grid(0);
            int ycol = grid_y - min_grid(1);
            mask(xrow, ycol) = gauss({ wp.x(), wp.y() });
            sum += mask(xrow, ycol);

        }
    }

    sum = gauss(mean);
    ROS_INFO("Normalizing gaussian mask with normalizer %0.6f", sum);

    // normalize the mask
    for (int x = 0; x < num_x_cells; ++x) {
        for (int y = 0; y < num_y_cells; ++y) {
            mask(x, y) *= (1.0 / sum);
            printf("%0.6f ", mask(x, y));
        }
        printf("\n");
    }

    const std::int8_t obsthresh = 100;

    double probability = 1.0;
    for (int grid_x = min_grid(0); grid_x < max_grid(0); ++grid_x) {
        for (int grid_y = min_grid(1); grid_y <  max_grid(1); ++grid_y) {
            int xrow = grid_x - min_grid(0);
            int ycol = grid_y - min_grid(1);
            double probmask = mask(xrow, ycol);

            if (grid_at(grid, grid_x, grid_y) >= obsthresh) {
                ROS_INFO("Obstacle cell detected. probmask = %0.6f", probmask);
                probability *= (1.0 - probmask);
            }
            else {
                ROS_INFO("Free cell detected");
            }
        }
    }

    return probability;
}

bool GraspObjectExecutor::download_marker_params()
{
    double marker_to_link_x;
    double marker_to_link_y;
    double marker_to_link_z;
    double marker_to_link_roll_degs;
    double marker_to_link_pitch_degs;
    double marker_to_link_yaw_degs;

    AttachedMarker attached_marker;

    bool success =
            msg_utils::download_param(ph_, "tracked_marker_id", attached_marker.marker_id) &&
            msg_utils::download_param(ph_, "tracked_marker_attached_link", attached_marker.attached_link) &&
            msg_utils::download_param(ph_, "marker_to_link_x", marker_to_link_x) &&
            msg_utils::download_param(ph_, "marker_to_link_y", marker_to_link_y) &&
            msg_utils::download_param(ph_, "marker_to_link_z", marker_to_link_z) &&
            msg_utils::download_param(ph_, "marker_to_link_roll_deg", marker_to_link_roll_degs) &&
            msg_utils::download_param(ph_, "marker_to_link_pitch_deg", marker_to_link_pitch_degs) &&
            msg_utils::download_param(ph_, "marker_to_link_yaw_deg", marker_to_link_yaw_degs);

    attached_marker.link_to_marker = Eigen::Affine3d(
        Eigen::Translation3d(marker_to_link_x, marker_to_link_y, marker_to_link_z) *
        Eigen::AngleAxisd(sbpl::utils::ToRadians(marker_to_link_yaw_degs), Eigen::Vector3d::UnitZ()) *
        Eigen::AngleAxisd(sbpl::utils::ToRadians(marker_to_link_pitch_degs), Eigen::Vector3d::UnitY()) *
        Eigen::AngleAxisd(sbpl::utils::ToRadians(marker_to_link_roll_degs), Eigen::Vector3d::UnitX())).inverse();

    attached_markers_.push_back(std::move(attached_marker));

    if (!success) {
        ROS_WARN("Failed to download marker params");
    }

    return success;
}
