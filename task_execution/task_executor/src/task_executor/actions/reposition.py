#!/usr/bin/env python
# The move action in a task plan

from __future__ import print_function, division

from math import sin, cos

import rospy
import actionlib

from task_executor.abstract_step import AbstractStep

from fetchit_mapping.msg import RepositionAction as RepositionActionMessage
from fetchit_mapping.msg import RepositionGoal as RepositionGoalMessage
from actionlib_msgs.msg import GoalStatus
from task_execution_msgs.msg import Waypoint
from task_execution_msgs.srv import GetWaypoints


class RepositionAction(AbstractStep):
    """
    Used to reposition the robot - similar to navigation but the robot can move backwards, 
    it is slower and more accurate. 
    """

    REPOSITION_ACTION_SERVER = "/reposition"
    WAYPOINTS_SERVICE_NAME = "/database/waypoints"

    def init(self, name):
        self.name = name
        self._reposition_client = actionlib.SimpleActionClient(RepositionAction.REPOSITION_ACTION_SERVER, RepositionActionMessage)
        self._get_waypoints_srv = rospy.ServiceProxy(RepositionAction.WAYPOINTS_SERVICE_NAME, GetWaypoints)

        rospy.loginfo("Connecting to reposition server...")
        self._reposition_client.wait_for_server()
        rospy.loginfo("...reposition server connected")

        rospy.loginfo("Connecting to database services...")
        self._get_waypoints_srv.wait_for_service()
        rospy.loginfo("...database services connected")

    def run(self, location):
        """
        The run function for this step

        Args:
            location (str, list, tuple, dict) :
                The location to move to. If the type is:

                * str. Then if the string starts with
                    * `waypoints`, assume the rest of the string specifies the \
                        ``tf`` frame of the waypoint and therefore move to the \
                        pose ``[0, 0, 0]`` w.r.t that frame
                    * `locations`, get a list of ``task_execution_msgs/Waypoint`` \
                        poses from :const:`WAYPOINTS_SERVICE_NAME`; visit the \
                        waypoints in order
                * dict. Then if the keys of the dict are
                    * `x, y, theta, frame`, visit the waypoint defined by the dict
                * list, tuple. Then if the list is of
                    * `dicts of the previous case`, visit the waypoints in the \
                        list or tuple in order

        .. seealso::

            :meth:`task_executor.abstract_step.AbstractStep.run`
        """
        # Parse out the waypoints
        coords = self._parse_location(location)
        if coords is None:
            rospy.logerr("Action {}: FAIL. Unknown Format: {}".format(self.name, location))
            raise KeyError(self.name, "Unknown Format", location)

        rospy.logdebug("Action {}: Moving to location(s): {}".format(self.name, coords))

        status = GoalStatus.LOST
        for coord_num, coord in enumerate(coords):
            rospy.loginfo("Action {}: Going to {}/{}. Coordinate: {{ {} }}"
                          .format(self.name, coord_num + 1, len(coords), str(coord).replace("\n", ", ")))

            # Create and send the goal
            goal = RepositionGoalMessage()
            goal.goal.pose.position.x = coord.x
            goal.goal.pose.position.y = coord.y
            goal.goal.pose.orientation.z = sin(coord.theta/2.0)
            goal.goal.pose.orientation.w = cos(coord.theta/2.0)
            goal.goal.header.frame_id = coord.frame
            goal.goal.header.stamp = rospy.Time.now()
            self._reposition_client.send_goal(goal)
            self.notify_action_send_goal(RepositionAction.REPOSITION_ACTION_SERVER, goal)

            # Yield running while the move_client is executing
            while self._reposition_client.get_state() in AbstractStep.RUNNING_GOAL_STATES:
                yield self.set_running()

            # Check the status and stop executing if we didn't complete our goal
            status = self._reposition_client.get_state()
            self._reposition_client.wait_for_result()
            result = self._reposition_client.get_result()
            self.notify_action_recv_result(RepositionAction.REPOSITION_ACTION_SERVER, status, result)

            if status != GoalStatus.SUCCEEDED:
                break

        # Yield based on how we exited
        if status == GoalStatus.SUCCEEDED:
            yield self.set_succeeded()
        elif status == GoalStatus.PREEMPTED:
            yield self.set_preempted(
                action=self.name,
                status=status,
                goal=goal,
                coord_num=coord_num,
                result=result
            )
        else:
            yield self.set_aborted(
                action=self.name,
                status=status,
                goal=goal,
                coord_num=coord_num,
                result=result
            )

    def stop(self):
        self._reposition_client.cancel_goal()
        self.notify_action_cancel(RepositionAction.REPOSITION_ACTION_SERVER)

    def _parse_location(self, location):
        coords = None
        if isinstance(location, str):
            db_name, location = location.split('.', 1)
            if db_name == 'locations':
                coords = self._get_waypoints_srv(location).waypoints
                self.notify_service_called(RepositionAction.REPOSITION_ACTION_SERVER)
            elif db_name == 'waypoints':
                # These are predefined tf frames
                coords = [Waypoint(frame=location)]
        elif isinstance(location, dict):
            coords = [Waypoint(**location),]
        elif isinstance(location, (list, tuple,)):
            coords = [Waypoint(**x) for x in location]

        return coords
