// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Soft-Capacitated Vehicle Routing Problem.
// A description of the problem can be found here:
// http://en.wikipedia.org/wiki/Vehicle_routing_problem.
// The variant which is tackled by this model includes a capacity dimension,
// implemented as a soft constraint: using more than the available capacity is
// penalized (i.e. "costs" more) but not forbidden. For the sake of simplicity,
// orders are randomly located and distances are computed using the Manhattan
// distance. Distances are assumed to be in meters and times in seconds.

#include <cstdint>
#include <random>
#include <vector>

#include "absl/random/random.h"
#include "google/protobuf/text_format.h"
#include "ortools/base/commandlineflags.h"
#include "ortools/base/init_google.h"
#include "ortools/base/types.h"
#include "ortools/base/logging.h"
#include "ortools/constraint_solver/routing.h"
#include "ortools/constraint_solver/routing_index_manager.h"
#include "ortools/constraint_solver/routing_parameters.h"
#include "ortools/constraint_solver/routing_parameters.pb.h"
#include "ortools/routing/cvrptw_lib.h"

using operations_research::Assignment;
using operations_research::DefaultRoutingSearchParameters;
using operations_research::GetSeed;
using operations_research::LocationContainer;
using operations_research::RandomDemand;
using operations_research::RoutingDimension;
using operations_research::RoutingIndexManager;
using operations_research::RoutingModel;
using operations_research::RoutingNodeIndex;
using operations_research::RoutingSearchParameters;
using operations_research::ServiceTimePlusTransition;

ABSL_FLAG(int, vrp_orders, 100, "Number of nodes in the problem.");
ABSL_FLAG(int, vrp_vehicles, 20, "Number of vehicles in the problem.");
ABSL_FLAG(int, vrp_vehicle_hard_capacity, 80,
          "Hard capacity for a vehicle; set to 0 to disable the hard capacity "
          "constraint");
ABSL_FLAG(int, vrp_vehicle_soft_capacity, 40,
          "Soft capacity for a vehicle; set to 0 to disable the soft capacity "
          "constraint");
ABSL_FLAG(int, vrp_vehicle_soft_capacity_cost, 5000,
          "Cost of using a vehicle beyond its soft capacity (per unit "
          "of storage over the soft capacity)");
ABSL_FLAG(bool, vrp_use_deterministic_random_seed, false,
          "Use deterministic random seeds.");
ABSL_FLAG(bool, vrp_use_same_vehicle_costs, false,
          "Use same vehicle costs in the routing model");
ABSL_FLAG(std::string, routing_search_parameters, "",
          "Text proto RoutingSearchParameters (possibly partial) that will "
          "override the DefaultRoutingSearchParameters()");

const char* kTime = "Time";
const char* kCapacity = "Capacity";
const int64_t kMaxNodesPerGroup = 10;
const int64_t kSameVehicleCost = 1000;

int main(int argc, char** argv) {
  InitGoogle(argv[0], &argc, &argv, true);
  CHECK_LT(0, absl::GetFlag(FLAGS_vrp_orders))
      << "Specify an instance size greater than 0.";
  CHECK_LT(0, absl::GetFlag(FLAGS_vrp_vehicles))
      << "Specify a non-null vehicle fleet size.";
  if (absl::GetFlag(FLAGS_vrp_vehicle_hard_capacity) > 0 &&
      absl::GetFlag(FLAGS_vrp_vehicle_soft_capacity) > 0) {
    CHECK_LT(absl::GetFlag(FLAGS_vrp_vehicle_soft_capacity),
             absl::GetFlag(FLAGS_vrp_vehicle_hard_capacity))
        << "The hard capacity must be higher than the soft capacity.";
  }

  // VRP of size absl::GetFlag(FLAGS_vrp_size).
  // Nodes are indexed from 0 to absl::GetFlag(FLAGS_vrp_orders), the starts and
  // ends of the routes are at node 0.
  const RoutingIndexManager::NodeIndex kDepot(0);
  RoutingIndexManager manager(absl::GetFlag(FLAGS_vrp_orders) + 1,
                              absl::GetFlag(FLAGS_vrp_vehicles), kDepot);
  RoutingModel routing(manager);

  // Setting up locations.
  const int64_t kXMax = 100'000;
  const int64_t kYMax = 100'000;
  const int64_t kSpeed = 10;
  LocationContainer locations(
      kSpeed, absl::GetFlag(FLAGS_vrp_use_deterministic_random_seed));
  for (int location = 0; location <= absl::GetFlag(FLAGS_vrp_orders);
       ++location) {
    locations.AddRandomLocation(kXMax, kYMax);
  }

  // Setting the cost function.
  const int vehicle_cost = routing.RegisterTransitCallback(
      [&locations, &manager](int64_t i, int64_t j) {
        return locations.ManhattanDistance(manager.IndexToNode(i),
                                           manager.IndexToNode(j));
      });
  routing.SetArcCostEvaluatorOfAllVehicles(vehicle_cost);

  // Adding capacity dimension constraints with slacks.
  const int64_t kNullCapacitySlack = 0;
  RandomDemand demand(manager.num_nodes(), kDepot,
                      absl::GetFlag(FLAGS_vrp_use_deterministic_random_seed));
  demand.Initialize();
  routing.AddDimension(
      routing.RegisterTransitCallback([&demand, &manager](int64_t i,
                                                          int64_t j) {
        return demand.Demand(manager.IndexToNode(i), manager.IndexToNode(j));
      }),
      kNullCapacitySlack, absl::GetFlag(FLAGS_vrp_vehicle_hard_capacity),
      /*fix_start_cumul_to_zero=*/true, kCapacity);
  RoutingDimension* capacity_dimension = routing.GetMutableDimension(kCapacity);

  // Penalise the capacity slacks to implement the soft constraint (a hard
  // constraint has a zero slack).
  const int num_vehicles = absl::GetFlag(FLAGS_vrp_vehicles);
  for (int vehicle = 0; vehicle < num_vehicles; ++vehicle) {
    capacity_dimension->SetCumulVarSoftUpperBound(
        routing.End(vehicle), absl::GetFlag(FLAGS_vrp_vehicle_soft_capacity),
        absl::GetFlag(FLAGS_vrp_vehicle_soft_capacity_cost));
  }

  // Adding time dimension constraints.
  const int64_t kTimePerDemandUnit = 300;
  const int64_t kHorizon = 24 * 3600;
  ServiceTimePlusTransition time(
      kTimePerDemandUnit,
      [&demand](RoutingNodeIndex i, RoutingNodeIndex j) {
        return demand.Demand(i, j);
      },
      [&locations](RoutingNodeIndex i, RoutingNodeIndex j) {
        return locations.ManhattanTime(i, j);
      });
  routing.AddDimension(
      routing.RegisterTransitCallback([&time, &manager](int64_t i, int64_t j) {
        return time.Compute(manager.IndexToNode(i), manager.IndexToNode(j));
      }),
      kHorizon, kHorizon, /*fix_start_cumul_to_zero=*/true, kTime);
  const RoutingDimension& time_dimension = routing.GetDimensionOrDie(kTime);

  // Adding time windows.
  std::mt19937 randomizer(
      GetSeed(absl::GetFlag(FLAGS_vrp_use_deterministic_random_seed)));
  const int64_t kTWDuration = 5 * 3600;
  for (int order = 1; order < manager.num_nodes(); ++order) {
    const int64_t start =
        absl::Uniform<int32_t>(randomizer, 0, kHorizon - kTWDuration);
    time_dimension.CumulVar(order)->SetRange(start, start + kTWDuration);
  }

  // Adding penalty costs to allow skipping orders.
  const int64_t kPenalty = 10'000'000;
  const RoutingIndexManager::NodeIndex kFirstNodeAfterDepot(1);
  for (RoutingIndexManager::NodeIndex order = kFirstNodeAfterDepot;
       order < manager.num_nodes(); ++order) {
    std::vector<int64_t> orders(1, manager.NodeToIndex(order));
    routing.AddDisjunction(orders, kPenalty);
  }

  // Adding same vehicle constraint costs for consecutive nodes.
  if (absl::GetFlag(FLAGS_vrp_use_same_vehicle_costs)) {
    std::vector<int64_t> group;
    for (RoutingIndexManager::NodeIndex order = kFirstNodeAfterDepot;
         order < manager.num_nodes(); ++order) {
      group.push_back(manager.NodeToIndex(order));
      if (group.size() == kMaxNodesPerGroup) {
        routing.AddSoftSameVehicleConstraint(group, kSameVehicleCost);
        group.clear();
      }
    }
    if (!group.empty()) {
      routing.AddSoftSameVehicleConstraint(group, kSameVehicleCost);
    }
  }

  // Solve, returns a solution if any (owned by RoutingModel).
  RoutingSearchParameters parameters = DefaultRoutingSearchParameters();
  CHECK(google::protobuf::TextFormat::MergeFromString(
      absl::GetFlag(FLAGS_routing_search_parameters), &parameters));
  const Assignment* solution = routing.SolveWithParameters(parameters);
  if (solution != nullptr) {
    DisplayPlan(manager, routing, *solution,
                absl::GetFlag(FLAGS_vrp_use_same_vehicle_costs),
                kMaxNodesPerGroup, kSameVehicleCost,
                routing.GetDimensionOrDie(kCapacity),
                routing.GetDimensionOrDie(kTime));
  } else {
    LOG(INFO) << "No solution found.";
  }
  return EXIT_SUCCESS;
}
