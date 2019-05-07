#!/usr/bin/env python
# The find object action in a task plan

from __future__ import print_function, division

import numpy as np

import rospy
import actionlib

from task_executor.abstract_step import AbstractStep

from rail_manipulation_msgs.msg import SegmentedObject
from rail_object_recognition.srv import ExtractPointCloud
from manipulation_actions.msg import ChallengeObject
from task_execution_msgs.srv import GetPartsAtLocation, GetBeliefs, GetSemanticLocations


class RecognizeObjectAction(AbstractStep):
    """
    Given a list of point clouds from ``rail_segmentatsion``, recognize the
    desired ``manipulation_actions/ChallengeObject`` and return the most likely
    candidate point cloud in the list.
    """

    RECOGNIZE_OBJECT_SERVICE_NAME = "/rail_object_recognition/recognize_object"
    PARTS_AT_LOCATIONS_SERVICE_NAME = "/database/parts_at_location"
    SEMANTIC_LOCATIONS_SERVICE_NAME = "/database/semantic_locations"
    BELIEFS_SERVICE_NAME = "/beliefs/get_beliefs"

    # The indices of the challenge objects in the returned recognition output
    CHALLENGE_OBJECT_INDICES = {
        ChallengeObject.NONE: 5,
        ChallengeObject.BOLT: 4,
        ChallengeObject.SMALL_GEAR: 3,
        ChallengeObject.LARGE_GEAR: 2,
        ChallengeObject.GEARBOX_TOP: 0,
        ChallengeObject.GEARBOX_BOTTOM: 1,
    }

    # make this an argument
    USE_BELIEF = True

    def init(self, name):
        self.name = name

        # The object recognition API
        self._recognize_object_srv = rospy.ServiceProxy(
            RecognizeObjectAction.RECOGNIZE_OBJECT_SERVICE_NAME,
            ExtractPointCloud
        )

        # Set a stop flag
        self._stopped = False

        # Wait for a connection to the recognition API
        rospy.loginfo("Connecting to rail_object_recognition..")
        self._recognize_object_srv.wait_for_service()
        rospy.loginfo("...rail_object_recognition connected")

        if RecognizeObjectAction.USE_BELIEF:
            # Postprocessing recognition results using semantic knowledge
            self._get_parts_at_location_srv = rospy.ServiceProxy(
                RecognizeObjectAction.PARTS_AT_LOCATIONS_SERVICE_NAME,
                GetPartsAtLocation
            )
            self._get_semantic_locations_srv = rospy.ServiceProxy(
                RecognizeObjectAction.SEMANTIC_LOCATIONS_SERVICE_NAME,
                GetSemanticLocations
            )
            self._get_beliefs_srv = rospy.ServiceProxy(
                RecognizeObjectAction.BELIEFS_SERVICE_NAME,
                GetBeliefs
            )

            rospy.loginfo("Connecting to database services...")
            self._get_semantic_locations_srv.wait_for_service()
            rospy.loginfo("...database services connected")

            rospy.loginfo("Connecting to database services...")
            self._get_parts_at_location_srv.wait_for_service()
            rospy.loginfo("...database services connected")

            rospy.loginfo("Connecting to belief services...")
            self._get_parts_at_location_srv.wait_for_service()
            rospy.loginfo("...belief services connected")

            self._parts_at_locations = {}
            self._get_parts_at_locations()

    def _get_parts_at_locations(self):
        locations = self._get_semantic_locations_srv().locations
        for location in locations:
            try:
                resp = self._get_parts_at_location_srv(location)
            except:
                continue
            parts = [p.object for p in resp.parts_at_location.parts]
            self._parts_at_locations[location] = parts

    def run(self, desired_obj, segmented_objects):
        """
        The run function for this step

        Args:
            desired_obj (manipulation_actions/ChallengeObject) : the type of
                challenge object that we wish to recognize
            segmented_objects (list of rail_manipulation_msgs/SegmentedObject) :
                a list of object point clouds, from
                :mod:`task_executor.actions.segment`, that we want to recognize
                the `desired_obj` in

        Yields:
            object_idx (int) : the index of the desired object in the input list

        .. seealso::

            :meth:`task_executor.abstract_step.AbstractStep.run`
        """

        # We expect segmented_objects to be the output from `segment`
        rospy.loginfo("Action {}: Inspecting scene for object {}"
                      .format(self.name, desired_obj))

        # If the desired object is specified by its key, then disambiguate it
        if isinstance(desired_obj, str):
            desired_obj = getattr(ChallengeObject, desired_obj.upper())

        # Ensure that the args are valid
        assert desired_obj in RecognizeObjectAction.CHALLENGE_OBJECT_INDICES.keys(), \
            "Unknown desired object {}".format(desired_obj)

        # Use belief about current location to check if desired_object is valid
        if RecognizeObjectAction.USE_BELIEF:
            resp = self._get_beliefs_srv()
            belief_keys = resp.beliefs
            belief_values = resp.values
            current_location = None
            for key, value in zip(belief_keys, belief_values):
                if value == 0:
                    continue
                if "robot_at_" not in key:
                    continue
                current_location = key.replace("robot_at_", "").lower()
            rospy.loginfo("current location {}".format(current_location))

            if desired_obj not in self._parts_at_locations[current_location]:
                rospy.loginfo("")
                yield self.set_aborted(
                    action=self.name,
                    goal=desired_obj,
                )
                raise StopIteration()

        # Ensure there are segmented objects
        assert len(segmented_objects) > 0, "Cannot recognize wih 0 point clouds"
        self._stopped = False

        # Send the point clouds of the segmented objects
        classifications = self._recognize_object_srv(
            clouds=[obj.point_cloud for obj in segmented_objects]
        ).classes_of_parts
        self.notify_service_called(RecognizeObjectAction.RECOGNIZE_OBJECT_SERVICE_NAME)
        yield self.set_running()
        if self._stopped:
            yield self.set_preempted(
                action=self.name,
                goal=desired_obj,
                srv=self._recognize_object_srv.resolved_name,
                classification=classification
            )
            raise StopIteration()

        # Load the classifications and reshape them
        classifications = np.array(classifications).reshape(
            -1, len(RecognizeObjectAction.CHALLENGE_OBJECT_INDICES.keys())
        )

        # Then figure out the index of the desired object using max and argmax
        object_idx = np.argmax(
            classifications[:, RecognizeObjectAction.CHALLENGE_OBJECT_INDICES[desired_obj]],
        )

        # For now, just stop
        rospy.loginfo("Action {}: Recognized object_idx is {}".format(self.name, object_idx))
        yield self.set_succeeded(object_idx=object_idx)

    def stop(self):
        self._stopped = True
