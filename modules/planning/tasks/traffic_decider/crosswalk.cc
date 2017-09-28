/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include <vector>
#include <limits>

#include "modules/common/vehicle_state/vehicle_state.h"
#include "modules/common/proto/pnc_point.pb.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/perception/proto/perception_obstacle.pb.h"
#include "modules/planning/common/frame.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/tasks/traffic_decider/crosswalk.h"

namespace apollo {
namespace planning {

using apollo::perception::PerceptionObstacle;
using apollo::hdmap::HDMapUtil;

Crosswalk::Crosswalk() : TrafficRule("Crosswalk") {}

bool Crosswalk::ApplyRule(Frame *frame,
                          ReferenceLineInfo *const reference_line_info) {
  if (!FLAGS_enable_crosswalk) {
    return true;
  }

  if (!FindCrosswalks(reference_line_info)) {
    return true;
  }

  auto* path_decision = reference_line_info->path_decision();
  for (const auto* path_obstacle : path_decision->path_obstacles().Items()) {
    const PerceptionObstacle& perception_obstacle =
        path_obstacle->obstacle()->Perception();
    const std::string& obstacle_id = path_obstacle->Id();
    PerceptionObstacle::Type obstacle_type = perception_obstacle.type();
    std::string obstacle_type_name = PerceptionObstacle_Type_Name(
        obstacle_type);

    // check type
    if (obstacle_type != PerceptionObstacle::PEDESTRIAN
            && obstacle_type != PerceptionObstacle::BICYCLE
            && obstacle_type != PerceptionObstacle::UNKNOWN_MOVABLE
            && obstacle_type != PerceptionObstacle::UNKNOWN) {
        ADEBUG << "obstacle_id[" << obstacle_id
            << "] type[" << obstacle_type_name << "]. skip";
        continue;
    }

    for (auto crosswalk_overlap : crosswalk_overlaps_) {
      auto crosswalk_ptr = HDMapUtil::BaseMap().GetCrosswalkById(
          hdmap::MakeMapId(crosswalk_overlap->object_id));
      auto crosswalk_info = crosswalk_ptr.get();
      std::string crosswalk_id = crosswalk_info->id().id();

      // expand crosswalk polygon
      // note: crosswalk expanded area will include sideway area
      common::math::Vec2d point(perception_obstacle.position().x(),
                                perception_obstacle.position().y());
      const common::math::Polygon2d crosswalk_poly = crosswalk_info->polygon();
      bool in_crosswalk = crosswalk_poly.IsPointIn(point);
      const common::math::Polygon2d crosswalk_exp_poly =
          crosswalk_poly.ExpandByDistance(FLAGS_crosswalk_expand_distance);
      bool in_expanded_crosswalk = crosswalk_exp_poly.IsPointIn(point);

      double obstacle_l_distance = std::min(
          fabs(path_obstacle->perception_sl_boundary().start_l()),
          fabs(path_obstacle->perception_sl_boundary().end_l()));

      // TODO(all): check if on_road
      bool on_road = true;
      // bool on_road = reference_line_info->reference_line().IsOnRoad();

      ADEBUG << "obstacle_id[" << obstacle_id
          << "]; type[" << obstacle_type_name
          << "]; crosswalk_id[" << crosswalk_id
          << "]; obstacle_l_distance[" << obstacle_l_distance
          << "]; within_crosswalk_area[" << in_crosswalk
          << "]; within_expanded_crosswalk_area[" << in_expanded_crosswalk
          << "]; on_road[" << on_road << "]";

      if (!in_expanded_crosswalk) {
        ADEBUG << "skip: not in crosswalk expanded area. "
            << "obstacle_id[" << obstacle_id
            << "]; crosswalk_id[" << crosswalk_id << "]";
        continue;
      }

      // TODO(all): check if trajectory crosses with adc
      bool is_cross = true;

      bool stop = false;
      if (obstacle_l_distance >=
          FLAGS_crosswalk_max_l2_distance_to_ignore_pedestrian) {
        // (1) when l_distance is big enough(>= l2), STOP only if path crosses
        if (is_cross) {
          stop = true;
          ADEBUG << "need_stop(>=l2): obstacle_id[" << obstacle_id
              << "]; crosswalk_id[" << crosswalk_id << "]";
        }
      } else if (obstacle_l_distance <=
          FLAGS_crosswalk_max_l1_distance_to_ignore_pedestrian) {
        // (2) when l_distance is <=l1 + on_road(not on sideway),
        //     always STOP
        // (3) when l_distance is <=l1 + not on_road(on sideway),
        //     STOP only if path crosses
        if (on_road || (!on_road && is_cross)) {
          stop = true;
          ADEBUG << "need_stop(<=11): obstacle_id[" << obstacle_id
              << "]; crosswalk_id[" << crosswalk_id << "]";
        }
      } else {
        // TODO(all)
        // (4) when l_distance is between l1 and l2,
        //     use history decision of this crosswalk to smooth unsteadiness
      }

      if (!stop) {
        ADEBUG << "skip: obstacle_id[" << obstacle_id
            << "]; crosswalk_id[" << crosswalk_id << "]";
        continue;
      }

      // skip crosswalk if master vehicle body already passes the stop line
      double stop_line_start_s = crosswalk_overlap->start_s;
      double adc_front_edge_s = reference_line_info->AdcSlBoundary().end_s();
      if (stop_line_start_s + FLAGS_stop_max_distance_buffer
          <= adc_front_edge_s) {
        ADEBUG << "skip: obstacle_id[" << obstacle_id
            << "]; crosswalk_id[" << crosswalk_id
            << "]; crosswalk_start_s[" << stop_line_start_s
            << "]; adc_front_edge_s[" << adc_front_edge_s << "]";
        continue;
      }

      double stop_deceleration = GetStopDeceleration(reference_line_info,
                                                     crosswalk_overlap);
      if (stop_deceleration < FLAGS_stop_max_deceleration) {
        CreateStopObstacle(frame, reference_line_info, crosswalk_overlap);
      }
    }
  }

  return true;
}

bool Crosswalk::FindCrosswalks(
    ReferenceLineInfo *const reference_line_info) {
  crosswalk_overlaps_.clear();
  const std::vector<hdmap::PathOverlap> &crosswalk_overlaps =
      reference_line_info->reference_line().map_path().crosswalk_overlaps();
  for (const hdmap::PathOverlap &crosswalk_overlap : crosswalk_overlaps) {
    crosswalk_overlaps_.push_back(&crosswalk_overlap);
  }
  return crosswalk_overlaps_.size() > 0;
}

double Crosswalk::GetStopDeceleration(
    ReferenceLineInfo *const reference_line_info,
    const hdmap::PathOverlap* crosswalk_overlap) {
  double adc_speed = common::VehicleState::instance()->linear_velocity();
  if (adc_speed < FLAGS_stop_min_speed) {
    return 0.0;
  }
  double stop_distance = 0;
  double adc_front_s = reference_line_info->AdcSlBoundary().end_s();
  double stop_line_s = crosswalk_overlap->start_s;

  if (stop_line_s > adc_front_s) {
    stop_distance = stop_line_s - adc_front_s;
  } else {
    stop_distance = stop_line_s +
        FLAGS_stop_max_distance_buffer - adc_front_s;
  }
  if (stop_distance < 1e-5) {
    return std::numeric_limits<double>::max();
  }
  return (adc_speed * adc_speed) / (2 * stop_distance);
}

void Crosswalk::CreateStopObstacle(
    Frame* frame,
    ReferenceLineInfo *const reference_line_info,
    const hdmap::PathOverlap* crosswalk_overlap) {
  common::SLPoint sl_point;
  sl_point.set_s(crosswalk_overlap->start_s);
  sl_point.set_l(0);
  common::math::Vec2d vec2d;
  reference_line_info->reference_line().SLToXY(sl_point, &vec2d);
  double heading = reference_line_info->reference_line().
      GetReferencePoint(crosswalk_overlap->start_s).heading();
  double left_width;
  double right_width;
  reference_line_info->reference_line().GetLaneWidth(
      crosswalk_overlap->start_s, &left_width, &right_width);

  common::math::Box2d stop_wall_box{{vec2d.x(), vec2d.y()},
                               heading,
                               FLAGS_virtual_stop_wall_length,
                               left_width + right_width};

  std::string virtual_object_id = FLAGS_crosswalk_virtual_object_id_prefix
      + crosswalk_overlap->object_id;
  PathObstacle* stop_wall = reference_line_info->AddObstacle(
      frame->AddStaticVirtualObstacle(virtual_object_id,
                                      stop_wall_box));

  auto* path_decision = reference_line_info->path_decision();
  ObjectDecisionType stop;
  stop.mutable_stop();
  path_decision->AddLongitudinalDecision(Name(), stop_wall->Id(), stop);
}

}  // namespace planning
}  // namespace apollo