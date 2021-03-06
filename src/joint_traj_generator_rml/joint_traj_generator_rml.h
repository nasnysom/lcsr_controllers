#ifndef __LCSR_CONTROLLERS_JOINT_TRAJ_GENERATOR_RML_H
#define __LCSR_CONTROLLERS_JOINT_TRAJ_GENERATOR_RML_H

#include <iostream>

#include <boost/scoped_ptr.hpp>

#include <rtt/RTT.hpp>
#include <rtt/Port.hpp>

#include <kdl/jntarrayvel.hpp>
#include <kdl/tree.hpp>
#include <kdl/chain.hpp>
#include <kdl/velocityprofile_trap.hpp>

#include <trajectory_msgs/JointTrajectory.h>
#include <trajectory_msgs/JointTrajectoryPoint.h>
#include <sensor_msgs/JointState.h>
#include <control_msgs/JointTrajectoryControllerState.h>
#include <control_msgs/FollowJointTrajectoryAction.h>

#include <rtt_ros_tools/throttles.h>

#include <rtt_actionlib/rtt_actionlib.h>
#include <rtt_actionlib/rtt_action_server.h>

#include <conman/hook.h>

#include <ReflexxesAPI.h>
#include <RMLVelocityFlags.h>
#include <RMLVelocityInputParameters.h>
#include <RMLVelocityOutputParameters.h>

namespace lcsr_controllers {
  class JointTrajGeneratorRML : public RTT::TaskContext
  {
  public:
    // Convenience typedefs for actionlib
    ACTION_DEFINITION(control_msgs::FollowJointTrajectoryAction);
    typedef actionlib::ServerGoalHandle<control_msgs::FollowJointTrajectoryAction> GoalHandle;

    // RTT Properties
    bool use_rosparam_;
    bool use_rostopic_;
    std::string robot_description_;
    std::string robot_description_param_;
    std::string root_link_;
    std::string tip_link_;
    unsigned int n_dof_;
    double sampling_resolution_;
    Eigen::VectorXd 
      goal_position_tolerance_,
      goal_velocity_tolerance_,
      position_tolerance_,
      velocity_tolerance_,
      max_velocities_,
      max_accelerations_,
      max_jerks_;
    bool verbose_;
    bool stop_on_violation_;
    double stop_time_;

    typedef enum {
      INACTIVE = 0,
      FOLLOWING = 1,
      RECOVERING = 2
    } Mode;

    Mode traj_mode_;

  protected:
    // RTT Ports
    RTT::InputPort<Eigen::VectorXd> joint_position_in_;
    RTT::InputPort<Eigen::VectorXd> joint_velocity_in_;
    RTT::InputPort<Eigen::VectorXd> joint_position_cmd_in_;

    RTT::OutputPort<Eigen::VectorXd> joint_position_out_;
    RTT::OutputPort<Eigen::VectorXd> joint_velocity_out_;
    RTT::OutputPort<Eigen::VectorXd> joint_acceleration_out_;

    RTT::InputPort<trajectory_msgs::JointTrajectoryPoint> joint_traj_point_cmd_in_;
    RTT::InputPort<trajectory_msgs::JointTrajectory> joint_traj_cmd_in_;
    RTT::OutputPort<sensor_msgs::JointState> joint_state_desired_out_;

  public:
    JointTrajGeneratorRML(std::string const& name);
    virtual bool configureHook();
    virtual bool startHook();
    virtual void updateHook();
    virtual void stopHook();
    virtual void cleanupHook();
    virtual void errorHook();

    //! A trajectory segment structure for internal use 
    // This structure is used so that trajectory points can be decoupled from
    // each-other. The standard ROS trajectory message includes a timestamp
    // designating the start time, and then each point in the trajectory is
    // stamped relative to that one. Since this controller may splice different
    // trajectories together, re first translate them into an absolute
    // representation, where each point has a well-defined start and end time.
    struct TrajSegment 
    {
      static size_t segment_count;

      TrajSegment(
          size_t n_dof, 
          bool flexible_ = false) :
        queued(false),
        active(false),
        achieved(false),
        flexible(flexible_),
        start_time(ros::Time(0,0)),
        goal_time(ros::Time(0,0)), 
        expected_time(ros::Time(0,0)), 
        goal_positions(Eigen::VectorXd::Constant(n_dof,0.0)),
        goal_velocities(Eigen::VectorXd::Constant(n_dof,0.0)),
        goal_accelerations(Eigen::VectorXd::Constant(n_dof,0.0)),
        gh(NULL),
        gh_required(NULL)
      {
        id = segment_count++;
      }


      ~TrajSegment() {
        // If the goal handle is valid, check if the goal should succeed or abort
        if(queued && gh && gh->isValid() && gh->getGoalStatus().status == actionlib_msgs::GoalStatus::ACTIVE) {
          if(achieved) {
            *gh_required -= 1;
            if(*gh_required == 0) {
              RTT::log(RTT::Debug) << "All goal trajectory segments have been acheived. Setting goal succeeded." << RTT::endlog();
              gh->setSucceeded();
            }
          } else {
            RTT::log(RTT::Debug) << "Trajectory segment ("<<id<<") removed without being achieved. Aborting goal." << RTT::endlog();
            gh->setAborted();
          }
        }
      }

      size_t id;
      bool queued;
      bool active;
      bool achieved;
      bool flexible;
      ros::Time start_time;
      ros::Time goal_time;
      ros::Time expected_time;
      Eigen::VectorXd goal_positions;
      Eigen::VectorXd goal_velocities;
      Eigen::VectorXd goal_accelerations;

      // Associated goal handle
      GoalHandle *gh;
      size_t *gh_required;

      //! End-Time comparison function for binary search
      static bool StartTimeCompare(const TrajSegment &s1, const TrajSegment &s2) { 
        return s1.start_time < s2.start_time;
      }

      //! End-Time comparison function for binary search
      static bool GoalTimeCompare(const TrajSegment &s1, const TrajSegment &s2) { 
        return s1.goal_time < s2.goal_time;
      }
    };
    
    //! A container of trajectory segments with low complexity front and back modification
    typedef std::list<TrajSegment> TrajSegments;

    //! Segments to follow
    TrajSegments segments_;
    //! A copy of the active segment (the one currently solved in RMLAPI)
    TrajSegment active_segment_;

    //! Convert a ROS trajectory message to a list of TrajSegments
    static bool TrajectoryMsgToSegments(
        const trajectory_msgs::JointTrajectory &msg,
        const std::vector<size_t> &ip,
        const size_t n_dof,
        const ros::Time trajectory_start_time,
        TrajSegments &segments,
        GoalHandle *gh = NULL,
        size_t *gh_required = NULL);

    //! Update the one trajectory with points from another
    static bool SpliceTrajectory(
        TrajSegments &current_segments,
        const TrajSegments &new_segments);

    //! Configure some RML structures from this tasks's properties
    bool configureRML(
        boost::shared_ptr<ReflexxesAPI> &rml,
        boost::shared_ptr<RMLPositionInputParameters> &rml_in,
        boost::shared_ptr<RMLPositionOutputParameters> &rml_out,
        RMLPositionFlags &rml_flags) const;

    /** \brief Update the segments and determine if the traj needs to be
     * recomputed.
     *
     * Returns: true if the list of segments has changed or if a segment has
     * been activated
     */
    bool updateSegments(
        const ros::Time rtt_now,
        const Eigen::VectorXd &joint_position,
        const Eigen::VectorXd &joint_velocity,
        JointTrajGeneratorRML::TrajSegments &segments) const;

    //! Determine if the tolerances have been violated
    bool tolerancesViolated(
        const Eigen::VectorXd &joint_position,
        const Eigen::VectorXd &joint_velocity,
        const Eigen::VectorXd &joint_acceleration,
        const boost::shared_ptr<RMLPositionOutputParameters> rml_out,
        std::vector<bool> &position_tolerance_violations,
        std::vector<bool> &velocity_tolerance_violations) const;

    //! Compute trajectory initialized by an arbitrary state
    void computeTrajectory(
        const ros::Time rtt_now,
        const Eigen::VectorXd &init_position,
        const Eigen::VectorXd &init_velocity,
        const Eigen::VectorXd &init_acceleration,
        const ros::Duration duration,
        const Eigen::VectorXd &goal_position,
        const Eigen::VectorXd &goal_velocity,
        boost::shared_ptr<ReflexxesAPI> rml,
        boost::shared_ptr<RMLPositionInputParameters> rml_in,
        boost::shared_ptr<RMLPositionOutputParameters> rml_out,
        RMLPositionFlags &rml_flags) const;

    //! Compute trajectory initialized by the last output of rml_out
    void computeTrajectory(
        const ros::Time rtt_now,
        const ros::Duration duration,
        const Eigen::VectorXd &goal_position,
        const Eigen::VectorXd &goal_velocity,
        boost::shared_ptr<ReflexxesAPI> rml,
        boost::shared_ptr<RMLPositionInputParameters> rml_in,
        boost::shared_ptr<RMLPositionOutputParameters> rml_out,
        RMLPositionFlags &rml_flags) const;

    //! Compute trajectory initialized by the last output of rml_out 
    bool computeTrajectory(
        const ros::Time rtt_now,
        const JointTrajGeneratorRML::TrajSegments::iterator active_segment,
        boost::shared_ptr<ReflexxesAPI> rml,
        boost::shared_ptr<RMLPositionInputParameters> rml_in,
        boost::shared_ptr<RMLPositionOutputParameters> rml_out,
        RMLPositionFlags &rml_flags) const;

    /** \brief Sample the trajectory based on the current set of segments and robot state
     * This function does not change the state of the component, so it can be
     * used easily in testing or with lookaheads.
     *
     * Returns: true if segment achieved
     */
    bool sampleTrajectory(
        const ros::Time rtt_now,
        const ros::Time last_segment_start_time,
        boost::shared_ptr<ReflexxesAPI> rml,
        boost::shared_ptr<RMLPositionOutputParameters> rml_out,
        Eigen::VectorXd &joint_position_sample,
        Eigen::VectorXd &joint_velocity_sample,
        Eigen::VectorXd &joint_acceleration_sample) const;

    /** \brief Handle / cleanup sample error
     *
     * This cleans up any active goalhandles
     */
    void handleSampleError(const std::runtime_error &err);

    //! Throw runtime_error on RML error code
    void handleRMLResult(int rml_result) const;

    //! Output information about some RML input parameters
    static void RMLLog(
        const RTT::LoggerLevel level,
        const boost::shared_ptr<RMLPositionInputParameters> rml_in);

    void setMaxVelocity(const int i, const double d) {
      rml_in_->SetMaxVelocityVectorElement(d,i);
    }
    void setMaxAcceleration(const int i, const double d) { 
      rml_in_->SetMaxAccelerationVectorElement(d,i);
    }
    void setMaxJerk(const int i, const double d) {
      rml_in_->SetMaxJerkVectorElement(d,i);
    }

  protected:

    //! Read the command input ports
    bool readCommands(
        const ros::Time &rtt_now);

    //! Update a trajectory from an Eigen::VectorXd
    bool insertSegments(
        const Eigen::VectorXd &point,
        const ros::Time &time,
        TrajSegments &segments,
        std::vector<size_t> &index_permutation) const;

    //! Update a trajectory from a trajectory_msgs::JointTrajectoryPoint
    bool insertSegments(
        const trajectory_msgs::JointTrajectoryPoint &traj_point,
        const ros::Time &time,
        TrajSegments &segments,
        std::vector<size_t> &index_permutation) const;

    //! Update a trajectory from a trajectory_msgs::JointTrajectory
    bool insertSegments(
        const trajectory_msgs::JointTrajectory &trajectory,
        const ros::Time &time,
        TrajSegments &segments,
        std::vector<size_t> &index_permutation,
        GoalHandle *gh = NULL,
        size_t *gh_required = NULL) const;

    //! Get an identity permutation f(x) = x
    void getIdentityIndexPermutation(
        std::vector<size_t> &index_permutation) const
    {
      index_permutation.resize(n_dof_);
      for(int joint_index=0; joint_index<n_dof_; joint_index++) {
        index_permutation[joint_index] = joint_index;
      }
    }

    //! Get an index permutation based on the joint names
    void getIndexPermutation(
        const std::vector<std::string> &joint_names,
        std::vector<size_t> &index_permutation) const
    {
      index_permutation.resize(n_dof_);
      // Check if joint names are given
      if(joint_names.size() == n_dof_) {
        // Permute the joint names properly
        int joint_index=0;
        for(std::vector<std::string>::const_iterator it = joint_names.begin();
            it != joint_names.end();
            ++it)
        {
          index_permutation[joint_index] = joint_name_index_map_.find(*it)->second;
          joint_index++;
        }
      } else {
        this->getIdentityIndexPermutation(index_permutation);
      }
    }

    //! Trajectory Generator
    boost::shared_ptr<ReflexxesAPI> rml_;
    boost::shared_ptr<RMLPositionInputParameters> rml_in_;
    boost::shared_ptr<RMLPositionOutputParameters> rml_out_;
    RMLPositionFlags rml_flags_;
    RMLDoubleVector rml_zero_;
    RMLBoolVector rml_true_;
    ros::Time last_segment_start_time_;

    // Robot model
    std::vector<std::string> joint_names_;
    std::map<std::string,size_t> joint_name_index_map_;
    std::vector<size_t> index_permutation_;

    // State
    Eigen::VectorXd
      joint_zero_,
      joint_position_,
      joint_position_cmd_,
      joint_position_sample_,
      joint_position_err_,
      joint_velocity_,
      joint_velocity_last_,
      joint_velocity_sample_,
      joint_velocity_err_,
      joint_acceleration_,
      joint_acceleration_sample_;

    trajectory_msgs::JointTrajectoryPoint joint_traj_point_cmd_;
    trajectory_msgs::JointTrajectory joint_traj_cmd_;
    sensor_msgs::JointState joint_state_desired_;
    rtt_ros_tools::PeriodicThrottle ros_publish_throttle_;

    std::vector<bool> position_tolerance_violations_;
    std::vector<bool> velocity_tolerance_violations_;

    // Conman interface
    boost::shared_ptr<conman::Hook> conman_hook_;

  private:

    //! Current action goal
    GoalHandle current_gh_;
    size_t gh_segments_required_;

    //! Action feedback message
    Feedback feedback_;
    //! Action result message
    Result result_;

    //! RTT action server
    rtt_actionlib::RTTActionServer<control_msgs::FollowJointTrajectoryAction> rtt_action_server_;

    //! Accept/reject goal requests here
    // This function will get called before calling updateHook() again
    void goalCallback(GoalHandle gh);

    //! Handle preemption here
    void cancelCallback(GoalHandle gh);
  };
}


#endif // ifndef __LCSR_CONTROLLERS_JOINT_TRAJ_GENERATOR_RML_H
