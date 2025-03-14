/*
 *  Copyright 2018, Magazino GmbH, Sebastian Pütz, Jorge Santos Simón
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  move_base_action.cpp
 *
 *  authors:
 *    Sebastian Pütz <spuetz@uni-osnabrueck.de>
 *    Jorge Santos Simón <santos@magazino.eu>
 *
 */

#include <limits>

#include <mbf_utility/navigation_utility.h>

#include "mbf_abstract_nav/MoveBaseFlexConfig.h"
#include "mbf_abstract_nav/move_base_action.h"

namespace mbf_abstract_nav
{
MoveBaseAction::MoveBaseAction(const std::string& name, const mbf_utility::RobotInformation& robot_info,
                               const std::vector<std::string>& behaviors)
  : name_(name)
  , robot_info_(robot_info)
  , private_nh_("~")
  , action_client_exe_path_(private_nh_, "exe_path")
  , action_client_get_path_(private_nh_, "get_path")
  , action_client_recovery_(private_nh_, "recovery")
  , oscillation_timeout_(0)
  , oscillation_distance_(0)
  , oscillation_angle_(0)
  , replanning_thread_shutdown_(false)
  , recovery_enabled_(true)
  , behaviors_(behaviors)
  , action_state_(NONE)
  , recovery_trigger_(NONE)
  , dist_to_goal_(std::numeric_limits<double>::infinity())
  , replanning_thread_(boost::bind(&MoveBaseAction::replanningThread, this))
{
}

MoveBaseAction::~MoveBaseAction()
{
  action_state_ = NONE;
  replanning_thread_shutdown_ = true;
  if (replanning_thread_.joinable())
  {
    replanning_thread_.join();
  }
}

void MoveBaseAction::reconfigure(
    mbf_abstract_nav::MoveBaseFlexConfig &config, uint32_t level)
{
  if (config.planner_frequency > 0.0)
    replanning_period_.fromSec(1.0 / config.planner_frequency);
  else
    replanning_period_.fromSec(0.0);
  oscillation_timeout_ = ros::Duration(config.oscillation_timeout);
  oscillation_distance_ = config.oscillation_distance;
  oscillation_angle_ = config.oscillation_angle;
  recovery_enabled_ = config.recovery_enabled;
}

void MoveBaseAction::cancel()
{
  action_state_ = CANCELED;

  if (!action_client_get_path_.getState().isDone())
  {
    action_client_get_path_.cancelGoal();
  }

  if (!action_client_exe_path_.getState().isDone())
  {
    action_client_exe_path_.cancelGoal();
  }

  if (!action_client_recovery_.getState().isDone())
  {
    action_client_recovery_.cancelGoal();
  }
}

void MoveBaseAction::start(GoalHandle &goal_handle)
{
  dist_to_goal_ = std::numeric_limits<double>::infinity();

  action_state_ = GET_PATH;

  goal_handle.setAccepted();

  goal_handle_ = goal_handle;

  ROS_DEBUG_STREAM_NAMED("move_base", "Start action \"move_base\"");

  const mbf_msgs::MoveBaseGoal& goal = *goal_handle.getGoal();

  get_path_goal_.target_pose = goal.target_pose;
  get_path_goal_.use_start_pose = false; // use the robot pose
  get_path_goal_.planner = goal.planner;
  exe_path_goal_.controller = goal.controller;

  ros::Duration connection_timeout(1.0);

  last_oscillation_reset_ = ros::Time::now();

  // start recovering with the first behavior, use the recovery behaviors from the action request, if specified,
  // otherwise, use all loaded behaviors.

  recovery_behaviors_ = goal.recovery_behaviors.empty() ? behaviors_ : goal.recovery_behaviors;
  current_recovery_behavior_ = recovery_behaviors_.begin();

  // get the current robot pose only at the beginning, as exe_path will keep updating it as we move
  if (!robot_info_.getRobotPose(robot_pose_))
  {
    ROS_ERROR_STREAM_NAMED("move_base", "Could not get the current robot pose!");
    move_base_result_.message = "Could not get the current robot pose!";
    move_base_result_.outcome = mbf_msgs::MoveBaseResult::TF_ERROR;
    goal_handle.setAborted(move_base_result_, move_base_result_.message);
    return;
  }
  goal_pose_ = goal.target_pose;
  last_oscillation_pose_ = robot_pose_;

  // wait for server connections
  if (!action_client_get_path_.waitForServer(connection_timeout) ||
      !action_client_exe_path_.waitForServer(connection_timeout) ||
      !action_client_recovery_.waitForServer(connection_timeout))
  {
    ROS_ERROR_STREAM_NAMED("move_base", "Could not connect to one or more of move_base_flex actions: "
        "\"get_path\", \"exe_path\", \"recovery \"!");
    move_base_result_.outcome = mbf_msgs::MoveBaseResult::INTERNAL_ERROR;
    move_base_result_.message = "Could not connect to the move_base_flex actions!";
    goal_handle.setAborted(move_base_result_, move_base_result_.message);
    return;
  }

  // call get_path action server to get a first plan
  action_client_get_path_.sendGoal(
      get_path_goal_,
      boost::bind(&MoveBaseAction::actionGetPathDone, this, _1, _2));
}

void MoveBaseAction::actionExePathActive()
{
  ROS_DEBUG_STREAM_NAMED("move_base", "The \"exe_path\" action is active.");
}

void MoveBaseAction::actionExePathFeedback(
    const mbf_msgs::ExePathFeedbackConstPtr &feedback)
{
  mbf_msgs::MoveBaseFeedback move_base_feedback;
  move_base_feedback.outcome = feedback->outcome;
  move_base_feedback.message = feedback->message;
  move_base_feedback.angle_to_goal = feedback->angle_to_goal;
  move_base_feedback.dist_to_goal = feedback->dist_to_goal;
  move_base_feedback.current_pose = feedback->current_pose;
  move_base_feedback.last_cmd_vel = feedback->last_cmd_vel;
  goal_handle_.publishFeedback(move_base_feedback);
  dist_to_goal_ = feedback->dist_to_goal;
  robot_pose_ = feedback->current_pose;

  // we create a navigation-level oscillation detection using exe_path action's feedback,
  // as the latter doesn't handle oscillations created by quickly failing repeated plans

  // if oscillation detection is enabled by oscillation_timeout != 0
  if (!oscillation_timeout_.isZero())
  {
    // check if oscillating
    // moved more than the minimum oscillation distance
    if (mbf_utility::distance(robot_pose_, last_oscillation_pose_) >= oscillation_distance_ ||
        mbf_utility::angle(robot_pose_, last_oscillation_pose_) >= oscillation_angle_)
    {
      last_oscillation_reset_ = ros::Time::now();
      last_oscillation_pose_ = robot_pose_;

      if (recovery_trigger_ == OSCILLATING)
      {
        ROS_INFO_NAMED("move_base", "Recovered from robot oscillation: restart recovery behaviors");
        current_recovery_behavior_ = recovery_behaviors_.begin();
        recovery_trigger_ = NONE;
      }
    }
    else if (last_oscillation_reset_ + oscillation_timeout_ < ros::Time::now())
    {
      std::stringstream oscillation_msgs;
      oscillation_msgs << "Robot is oscillating for " << (ros::Time::now() - last_oscillation_reset_).toSec() << "s!";
      ROS_WARN_STREAM_NAMED("move_base", oscillation_msgs.str());
      action_client_exe_path_.cancelGoal();

      if (attemptRecovery())
      {
        recovery_trigger_ = OSCILLATING;
      }
      else
      {
        move_base_result_.outcome = mbf_msgs::MoveBaseResult::OSCILLATION;
        move_base_result_.message = oscillation_msgs.str();
        move_base_result_.final_pose = robot_pose_;
        move_base_result_.angle_to_goal = move_base_feedback.angle_to_goal;
        move_base_result_.dist_to_goal = move_base_feedback.dist_to_goal;
        goal_handle_.setAborted(move_base_result_, move_base_result_.message);
      }
    }
  }
}

void MoveBaseAction::actionGetPathDone(
    const actionlib::SimpleClientGoalState &state,
    const mbf_msgs::GetPathResultConstPtr &result_ptr)
{
  const mbf_msgs::GetPathResult &get_path_result = *result_ptr;

  // copy result from get_path action
  fillMoveBaseResult(get_path_result, move_base_result_);

  switch (state.state_)
  {
    case actionlib::SimpleClientGoalState::PENDING:
      ROS_FATAL_STREAM_NAMED("move_base", "get_path PENDING state not implemented, this should not be reachable!");
      break;

    case actionlib::SimpleClientGoalState::SUCCEEDED:
      ROS_DEBUG_STREAM_NAMED("move_base", "Action \""
          << "move_base\" received a path from \""
          << "get_path\": " << state.getText());

      exe_path_goal_.path = get_path_result.path;
      ROS_DEBUG_STREAM_NAMED("move_base", "Action \""
          << "move_base\" sends the path to \""
          << "exe_path\".");

      if (recovery_trigger_ == GET_PATH)
      {
        ROS_WARN_NAMED("move_base", "Recovered from planner failure: restart recovery behaviors");
        current_recovery_behavior_ = recovery_behaviors_.begin();
        recovery_trigger_ = NONE;
      }

      action_client_exe_path_.sendGoal(
          exe_path_goal_,
          boost::bind(&MoveBaseAction::actionExePathDone, this, _1, _2),
          boost::bind(&MoveBaseAction::actionExePathActive, this),
          boost::bind(&MoveBaseAction::actionExePathFeedback, this, _1));
      action_state_ = EXE_PATH;
      break;

    case actionlib::SimpleClientGoalState::ABORTED:
      if (!action_client_exe_path_.getState().isDone())
      {
        ROS_WARN_STREAM_NAMED("move_base", "Cancel previous goal, as planning to the new one has failed");
        cancel();
      }
      if (attemptRecovery())
      {
        recovery_trigger_ = GET_PATH;
      }
      else
      {
        // copy result from get_path action
        ROS_WARN_STREAM_NAMED("move_base", "Abort the execution of the planner: " << get_path_result.message);
        goal_handle_.setAborted(move_base_result_, state.getText());
      }
      action_state_ = FAILED;
      break;

    case actionlib::SimpleClientGoalState::RECALLED:
    case actionlib::SimpleClientGoalState::PREEMPTED:
      ROS_INFO_STREAM_NAMED("move_base", "The last action goal to \"get_path\" has been " << state.toString());
      if (action_state_ == CANCELED)
      {
        // move_base preempted while executing get_path; fill result and report canceled to the client
        ROS_INFO_STREAM_NAMED("move_base", "move_base preempted while executing get_path");
        goal_handle_.setCanceled(move_base_result_, state.getText());
      }
      break;

    case actionlib::SimpleClientGoalState::REJECTED:
      ROS_ERROR_STREAM_NAMED("move_base", "The last action goal to \"get_path\" has been " << state.toString());
      goal_handle_.setCanceled(move_base_result_, state.getText());
      action_state_ = FAILED;
      break;

    case actionlib::SimpleClientGoalState::LOST:
      ROS_FATAL_STREAM_NAMED("move_base", "Connection lost to the action \"get_path\"!");
      goal_handle_.setAborted();
      action_state_ = FAILED;
      break;

    default:
      ROS_FATAL_STREAM_NAMED("move_base", "Reached unknown action server state!");
      goal_handle_.setAborted();
      action_state_ = FAILED;
      break;
  }
}

void MoveBaseAction::actionExePathDone(
    const actionlib::SimpleClientGoalState &state,
    const mbf_msgs::ExePathResultConstPtr &result_ptr)
{
  ROS_DEBUG_STREAM_NAMED("move_base", "Action \"exe_path\" finished.");

  const mbf_msgs::ExePathResult& exe_path_result = *result_ptr;

  // copy result from exe_path action
  fillMoveBaseResult(exe_path_result, move_base_result_);

  ROS_DEBUG_STREAM_NAMED("move_base", "Current state: " << state.toString());

  switch (state.state_)
  {
    case actionlib::SimpleClientGoalState::SUCCEEDED:
      move_base_result_.outcome = mbf_msgs::MoveBaseResult::SUCCESS;
      move_base_result_.message = "Action \"move_base\" succeeded!";
      ROS_INFO_STREAM_NAMED("move_base", move_base_result_.message);
      goal_handle_.setSucceeded(move_base_result_, move_base_result_.message);
      action_state_ = SUCCEEDED;
      break;

    case actionlib::SimpleClientGoalState::ABORTED:
      action_state_ = FAILED;

      switch (exe_path_result.outcome)
      {
        case mbf_msgs::ExePathResult::INVALID_PATH:
        case mbf_msgs::ExePathResult::TF_ERROR:
        case mbf_msgs::ExePathResult::NOT_INITIALIZED:
        case mbf_msgs::ExePathResult::INVALID_PLUGIN:
        case mbf_msgs::ExePathResult::INTERNAL_ERROR:
          // none of these errors is recoverable
          goal_handle_.setAborted(move_base_result_, state.getText());
          break;

        default:
          // all the rest are, so we start calling the recovery behaviors in sequence

          if (attemptRecovery())
          {
            recovery_trigger_ = EXE_PATH;
          }
          else
          {
            ROS_WARN_STREAM_NAMED("move_base", "Abort the execution of the controller: " << exe_path_result.message);
            goal_handle_.setAborted(move_base_result_, state.getText());
          }
          break;
      }
      break;

    case actionlib::SimpleClientGoalState::RECALLED:
    case actionlib::SimpleClientGoalState::PREEMPTED:
      ROS_INFO_STREAM_NAMED("move_base", "The last action goal to \"exe_path\" has been " << state.toString());
      if (action_state_ == CANCELED)
      {
        // move_base preempted while executing exe_path; fill result and report canceled to the client
        ROS_INFO_STREAM_NAMED("move_base", "move_base preempted while executing exe_path");
        goal_handle_.setCanceled(move_base_result_, state.getText());
      }
      break;

    case actionlib::SimpleClientGoalState::REJECTED:
      ROS_ERROR_STREAM_NAMED("move_base", "The last action goal to \"exe_path\" has been " << state.toString());
      goal_handle_.setCanceled(move_base_result_, state.getText());
      action_state_ = FAILED;
      break;

    case actionlib::SimpleClientGoalState::LOST:
      ROS_FATAL_STREAM_NAMED("move_base", "Connection lost to the action \"exe_path\"!");
      goal_handle_.setAborted();
      action_state_ = FAILED;
      break;

    default:
      ROS_FATAL_STREAM_NAMED("move_base", "Reached unreachable case! Unknown SimpleActionServer state!");
      goal_handle_.setAborted();
      action_state_ = FAILED;
      break;
  }
}

bool MoveBaseAction::attemptRecovery()
{
  if (!recovery_enabled_)
  {
    ROS_WARN_STREAM_NAMED("move_base", "Recovery behaviors are disabled!");
    return false;
  }

  if (current_recovery_behavior_ == recovery_behaviors_.end())
  {
    if (recovery_behaviors_.empty())
    {
      ROS_WARN_STREAM_NAMED("move_base", "No Recovery Behaviors loaded!");
    }
    else
    {
      ROS_WARN_STREAM_NAMED("move_base", "Executed all available recovery behaviors!");
    }
    return false;
  }

  recovery_goal_.behavior = *current_recovery_behavior_;
  ROS_DEBUG_STREAM_NAMED("move_base", "Start recovery behavior\""
      << *current_recovery_behavior_ <<"\".");
  action_client_recovery_.sendGoal(
      recovery_goal_,
      boost::bind(&MoveBaseAction::actionRecoveryDone, this, _1, _2)
  );
  action_state_ = RECOVERY;
  return true;
}

void MoveBaseAction::actionRecoveryDone(
    const actionlib::SimpleClientGoalState &state,
    const mbf_msgs::RecoveryResultConstPtr &result_ptr)
{
  // give the robot some time to stop oscillating after executing the recovery behavior
  last_oscillation_reset_ = ros::Time::now();

  const mbf_msgs::RecoveryResult& recovery_result = *result_ptr;

  switch (state.state_)
  {
    case actionlib::SimpleClientGoalState::REJECTED:
    case actionlib::SimpleClientGoalState::ABORTED:
    case actionlib::SimpleClientGoalState::RECALLED:
    case actionlib::SimpleClientGoalState::PREEMPTED:
      ROS_WARN_STREAM_NAMED("move_base", "The recovery behavior \"" << *current_recovery_behavior_ << "\" has been "
                                                                    << state.toString());
      ROS_DEBUG_STREAM("Recovery behavior message: " << recovery_result.message
                                                     << ", outcome: " << recovery_result.outcome);

      if (action_state_ == CANCELED)
      {
        // move_base preempted while executing a recovery; fill result and report canceled to the client
        fillMoveBaseResult(recovery_result, move_base_result_);
        ROS_INFO_STREAM_NAMED("move_base", "move_base preempted while executing a recovery behavior");
        goal_handle_.setCanceled(move_base_result_, state.getText());
        break;
      }
      if (current_recovery_behavior_ == recovery_behaviors_.end())
      {
        // we run out of recovery behaviors; concede defeat
        action_state_ = FAILED;
        ROS_DEBUG_STREAM_NAMED("move_base", "All recovery behaviors failed. Aborting move_base action");
        goal_handle_.setAborted(move_base_result_, "All recovery behaviors failed.");
        break;
      }
      // recovery failed, but we still have recovery behaviors to try; we cannot call the next one here,
      // (because simple action client will complain about transitioning to ACTIVE from DONE states)
      // so we try replanning again (wrong, but works)
      // [[fallthrough]]; C++17
    case actionlib::SimpleClientGoalState::SUCCEEDED:
      // go to planning state
      ROS_DEBUG_STREAM_COND_NAMED(state == actionlib::SimpleClientGoalState::SUCCEEDED, "move_base",
                                  "Execution of the recovery behavior \"" << *current_recovery_behavior_
                                                                          << "\" succeeded!");
      ROS_DEBUG_STREAM_NAMED("move_base",
                             "Try planning again and increment the current recovery behavior in the list.");
      action_state_ = GET_PATH;
      current_recovery_behavior_++; // use next behavior, the next time;
      action_client_get_path_.sendGoal(
          get_path_goal_,
          boost::bind(&MoveBaseAction::actionGetPathDone, this, _1, _2)
      );
      break;

    case actionlib::SimpleClientGoalState::LOST:
      ROS_FATAL_STREAM_NAMED("move_base", "Connection lost to the action \"recovery\"!");
      goal_handle_.setAborted();
      action_state_ = FAILED;
      break;
    default:
      ROS_FATAL_STREAM_NAMED("move_base", "Reached unreachable case! Unknown state!");
      goal_handle_.setAborted();
      action_state_ = FAILED;
      break;
  }
}

bool MoveBaseAction::replanningActive() const
{
  // replan only while following a path and if replanning is enabled (can be disabled by dynamic reconfigure)
  return !replanning_period_.isZero() && action_state_ == EXE_PATH && dist_to_goal_ > 0.1;
}

void MoveBaseAction::replanningThread()
{
  ros::Duration update_period(0.005);
  ros::Rate update_rate(1.0/update_period.toSec());
  ros::Time last_replan_time = ros::Time::now();

  while (ros::ok() && !replanning_thread_shutdown_)
  {
    if (!action_client_get_path_.getState().isDone())
    {
      if (action_client_get_path_.waitForResult(update_period))
      {
        actionlib::SimpleClientGoalState state = action_client_get_path_.getState();
        mbf_msgs::GetPathResultConstPtr result = action_client_get_path_.getResult();
        if (state == actionlib::SimpleClientGoalState::SUCCEEDED && replanningActive())
        {
          ROS_DEBUG_STREAM_NAMED("move_base", "Replanning succeeded; sending a goal to \"exe_path\" with the new plan");
          exe_path_goal_.path = result->path;
          mbf_msgs::ExePathGoal goal(exe_path_goal_);
          action_client_exe_path_.sendGoal(goal, boost::bind(&MoveBaseAction::actionExePathDone, this, _1, _2),
                                           boost::bind(&MoveBaseAction::actionExePathActive, this),
                                           boost::bind(&MoveBaseAction::actionExePathFeedback, this, _1));
        }
        else
        {
          ROS_DEBUG_STREAM_NAMED("move_base",
                                 "Replanning failed with error code " << result->outcome << ": " << result->message);
        }
      }
      // else keep waiting for planning to complete (we already waited update_period in waitForResult)
    }
    else if (!replanningActive() && ros::Time::now() - last_replan_time >= replanning_period_)
    {
      last_replan_time = ros::Time::now();
      update_period.sleep();
    }
    else if (ros::Time::now() - last_replan_time >= replanning_period_)
    {
      ROS_DEBUG_STREAM_NAMED("move_base", "Next replanning cycle, using the \"get_path\" action!");
      action_client_get_path_.sendGoal(get_path_goal_);
      last_replan_time = ros::Time::now();
    }
    update_rate.sleep();
  }
}

} /* namespace mbf_abstract_nav */
