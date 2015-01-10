/*
    open source routing machine
    Copyright (C) Dennis Luxen, others 2010

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU AFFERO General Public License as published by
the Free Software Foundation; either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
or see http://www.gnu.org/licenses/agpl.txt.
 */

#ifndef MAP_MATCHING_H
#define MAP_MATCHING_H

#include "routing_base.hpp"

#include "../Util/simple_logger.hpp"
#include "../Util/container.hpp"

#include <algorithm>
#include <iomanip>
#include <numeric>

#include <fstream>

namespace Matching
{
typedef std::vector<std::pair<PhantomNode, double>> CandidateList;
typedef std::vector<CandidateList> CandidateLists;
typedef std::pair<PhantomNodes, double> PhantomNodesWithProbability;
}

// implements a hidden markov model map matching algorithm
template <class DataFacadeT> class MapMatching final : public BasicRoutingInterface<DataFacadeT>
{
    typedef BasicRoutingInterface<DataFacadeT> super;
    typedef typename SearchEngineData::QueryHeap QueryHeap;
    SearchEngineData &engine_working_data;

    constexpr static const double sigma_z = 4.07;

    constexpr double emission_probability(const double distance) const
    {
        return (1. / (std::sqrt(2. * M_PI) * sigma_z)) *
               std::exp(-0.5 * std::pow((distance / sigma_z), 2.));
    }

    constexpr double log_probability(const double probability) const
    {
        return std::log2(probability);
    }

    // TODO: needs to be estimated from the input locations
    //constexpr static const double beta = 1.;
    // samples/min and beta
    // 1 0.49037673
    // 2 0.82918373
    // 3 1.24364564
    // 4 1.67079581
    // 5 2.00719298
    // 6 2.42513007
    // 7 2.81248831
    // 8 3.15745473
    // 9 3.52645392
    // 10 4.09511775
    // 11 4.67319795
    // 21 12.55107715
    // 12 5.41088180
    // 13 6.47666590
    // 14 6.29010734
    // 15 7.80752112
    // 16 8.09074504
    // 17 8.08550528
    // 18 9.09405065
    // 19 11.09090603
    // 20 11.87752824
    // 21 12.55107715
    // 22 15.82820829
    // 23 17.69496773
    // 24 18.07655652
    // 25 19.63438911
    // 26 25.40832185
    // 27 23.76001877
    // 28 28.43289797
    // 29 32.21683062
    // 30 34.56991141

    constexpr double transition_probability(const float d_t, const float beta) const
    {
        return (1. / beta) * std::exp(-d_t / beta);
    }

    // deprecated
    // translates a distance into how likely it is an input
    // double DistanceToProbability(const double distance) const
    // {
    //     if (0. > distance)
    //     {
    //         return 0.;
    //     }
    //     return 1. - 1. / (1. + exp((-distance + 35.) / 6.));
    // }

    double get_beta(const unsigned state_size,
                    const Matching::CandidateLists &timestamp_list,
                    const std::vector<FixedPointCoordinate> coordinate_list) const
    {
        std::vector<double> d_t_list, median_select_d_t_list;
        for (auto t = 1u; t < timestamp_list.size(); ++t)
        {
            for (auto s = 0u; s < state_size; ++s)
            {
                d_t_list.push_back(get_distance_difference(coordinate_list[t - 1],
                                                           coordinate_list[t],
                                                           timestamp_list[t - 1][s].first,
                                                           timestamp_list[t][s].first));
                median_select_d_t_list.push_back(d_t_list.back());
            }
        }

        std::nth_element(median_select_d_t_list.begin(),
                         median_select_d_t_list.begin() + median_select_d_t_list.size() / 2,
                         median_select_d_t_list.end());
        const auto median_d_t = median_select_d_t_list[median_select_d_t_list.size() / 2];

        return (1. / std::log(2)) * median_d_t;
    }


    double get_distance_difference(const FixedPointCoordinate &location1,
                                   const FixedPointCoordinate &location2,
                                   const PhantomNode &source_phantom,
                                   const PhantomNode &target_phantom) const
    {
        // great circle distance of two locations - median/avg dist table(candidate list1/2)
        const auto network_distance = get_network_distance(source_phantom, target_phantom);
        const auto great_circle_distance =
            FixedPointCoordinate::ApproximateDistance(location1, location2);

        if (great_circle_distance > network_distance)
        {
            return great_circle_distance - network_distance;
        }
        return network_distance - great_circle_distance;
    }

    double get_network_distance(const PhantomNode &source_phantom,
                                    const PhantomNode &target_phantom) const
    {
        EdgeWeight upper_bound = INVALID_EDGE_WEIGHT;
        NodeID middle_node = SPECIAL_NODEID;
        EdgeWeight edge_offset = std::min(0, -source_phantom.GetForwardWeightPlusOffset());
        edge_offset = std::min(edge_offset, -source_phantom.GetReverseWeightPlusOffset());

        engine_working_data.InitializeOrClearFirstThreadLocalStorage(
            super::facade->GetNumberOfNodes());
        engine_working_data.InitializeOrClearSecondThreadLocalStorage(
            super::facade->GetNumberOfNodes());

        QueryHeap &forward_heap = *(engine_working_data.forwardHeap);
        QueryHeap &reverse_heap = *(engine_working_data.backwardHeap);

        if (source_phantom.forward_node_id != SPECIAL_NODEID)
        {
            forward_heap.Insert(source_phantom.forward_node_id,
                                -source_phantom.GetForwardWeightPlusOffset(),
                                source_phantom.forward_node_id);
        }
        if (source_phantom.reverse_node_id != SPECIAL_NODEID)
        {
            forward_heap.Insert(source_phantom.reverse_node_id,
                                -source_phantom.GetReverseWeightPlusOffset(),
                                source_phantom.reverse_node_id);
        }

        if (target_phantom.forward_node_id != SPECIAL_NODEID)
        {
            reverse_heap.Insert(target_phantom.forward_node_id,
                                target_phantom.GetForwardWeightPlusOffset(),
                                target_phantom.forward_node_id);
        }
        if (target_phantom.reverse_node_id != SPECIAL_NODEID)
        {
            reverse_heap.Insert(target_phantom.reverse_node_id,
                                target_phantom.GetReverseWeightPlusOffset(),
                                target_phantom.reverse_node_id);
        }

        // search from s and t till new_min/(1+epsilon) > length_of_shortest_path
        while (0 < (forward_heap.Size() + reverse_heap.Size()))
        {
            if (0 < forward_heap.Size())
            {
                super::RoutingStep(
                    forward_heap, reverse_heap, &middle_node, &upper_bound, edge_offset, true);
            }
            if (0 < reverse_heap.Size())
            {
                super::RoutingStep(
                    reverse_heap, forward_heap, &middle_node, &upper_bound, edge_offset, false);
            }
        }

        double distance = std::numeric_limits<double>::max();
        if (upper_bound != INVALID_EDGE_WEIGHT)
        {
            std::vector<NodeID> packed_leg;
            super::RetrievePackedPathFromHeap(forward_heap, reverse_heap, middle_node, packed_leg);
            std::vector<PathData> unpacked_path;
            PhantomNodes nodes;
            nodes.source_phantom = source_phantom;
            nodes.target_phantom = target_phantom;
            super::UnpackPath(packed_leg, nodes, unpacked_path);

            FixedPointCoordinate previous_coordinate = source_phantom.location;
            FixedPointCoordinate current_coordinate;
            distance = 0;
            for (const auto& p : unpacked_path)
            {
                current_coordinate = super::facade->GetCoordinateOfNode(p.node);
                distance += FixedPointCoordinate::ApproximateDistance(previous_coordinate, current_coordinate);
                previous_coordinate = current_coordinate;
            }
            distance += FixedPointCoordinate::ApproximateDistance(previous_coordinate, target_phantom.location);
        }

        return distance;
    }

  public:
    MapMatching(DataFacadeT *facade, SearchEngineData &engine_working_data)
        : super(facade), engine_working_data(engine_working_data)
    {
    }

    void operator()(const unsigned state_size,
                    const Matching::CandidateLists &timestamp_list,
                    const std::vector<FixedPointCoordinate> coordinate_list,
                    std::vector<PhantomNode>& matched_nodes,
                    JSON::Object& debug_info) const
    {
        BOOST_ASSERT(state_size != std::numeric_limits<unsigned>::max());
        BOOST_ASSERT(state_size != 0);


        std::vector<std::vector<double>> viterbi(state_size,
                                                 std::vector<double>(timestamp_list.size(), -std::numeric_limits<double>::infinity()));
        std::vector<std::vector<std::size_t>> parent(
            state_size, std::vector<std::size_t>(timestamp_list.size(), 0));

        SimpleLogger().Write() << "initializing state probabilties: ";

        JSON::Array json_viterbi;
        JSON::Array json_initial_viterbi;
        for (auto s = 0u; s < state_size; ++s)
        {
            SimpleLogger().Write() << "initializing s: " << s << "/" << state_size;
            SimpleLogger().Write()
                << " distance: " << timestamp_list[0][s].second << " at "
                << timestamp_list[0][s].first.location << " prob " << std::setprecision(10)
                << emission_probability(timestamp_list[0][s].second) << " logprob "
                << log_probability(emission_probability(timestamp_list[0][s].second));

            // this might need to be squared as pi_s is also defined as the emission
            // probability in the paper.
            viterbi[s][0] = log_probability(emission_probability(timestamp_list[0][s].second));
            parent[s][0] = s;
            json_initial_viterbi.values.push_back(viterbi[s][0]);
        }
        json_viterbi.values.push_back(json_initial_viterbi);
        SimpleLogger().Write() << "running viterbi algorithm: ";

        // attention, this call is relatively expensive
        //const auto beta = get_beta(state_size, timestamp_list, coordinate_list);
        const auto beta = 10.0;

        JSON::Array json_timestamps;
        for (auto t = 1u; t < timestamp_list.size(); ++t)
        {
            JSON::Array json_transition_rows;
            JSON::Array json_viterbi_col;
            // compute d_t for this timestamp and the next one
            for (auto s = 0u; s < state_size; ++s)
            {
                JSON::Array json_row;
                for (auto s_prime = 0u; s_prime < state_size; ++s_prime)
                {
                    // how likely is candidate s_prime at time t to be emitted?
                    const double emission_pr = log_probability(emission_probability(timestamp_list[t][s_prime].second));

                    // get distance diff between loc1/2 and locs/s_prime
                    const auto d_t = get_distance_difference(coordinate_list[t-1],
                                                             coordinate_list[t],
                                                             timestamp_list[t-1][s].first,
                                                             timestamp_list[t][s_prime].first);

                    // plug probabilities together. TODO: change to addition for logprobs
                    const double transition_pr = log_probability(transition_probability(d_t, beta));
                    const double new_value = viterbi[s][t-1] + emission_pr + transition_pr;


                    JSON::Array json_element;
                    json_element.values.push_back(viterbi[s][t-1]);
                    json_element.values.push_back(emission_pr);
                    json_element.values.push_back(transition_pr == -std::numeric_limits<double>::infinity() ? -std::numeric_limits<double>::max() : transition_pr);
                    json_element.values.push_back(get_network_distance(timestamp_list[t-1][s].first, timestamp_list[t][s_prime].first));
                    json_element.values.push_back(FixedPointCoordinate::ApproximateDistance(coordinate_list[t-1], coordinate_list[t]));

                    json_row.values.push_back(json_element);

                    if (new_value > viterbi[s_prime][t])
                    {
                        SimpleLogger().Write() << "Updating: " << t << ": (" << s_prime << "): " << viterbi[s_prime][t] << " -> " << new_value << " (from " << s << ")";
                        viterbi[s_prime][t] = new_value;
                        parent[s_prime][t] = s;
                    } else {
                        SimpleLogger().Write() << " x " << t << ": " << new_value << " (from " << s << ")";
                    }
                }
                json_transition_rows.values.push_back(json_row);
                json_viterbi_col.values.push_back(viterbi[s][t]);
            }
            json_viterbi.values.push_back(json_viterbi_col);
            json_timestamps.values.push_back(json_transition_rows);
        }

        debug_info.values["transitions"] = json_timestamps;
        debug_info.values["viterbi"] = json_viterbi;
        debug_info.values["beta"] = beta;

        SimpleLogger().Write() << "Determining most plausible end state";
        // loop through the columns, and only compare the last entry
        auto max_element_iter = viterbi.begin();
        for (auto current = viterbi.begin(); current != viterbi.end(); current++)
        {
            if (max_element_iter->at(timestamp_list.size()-1) < current->at(timestamp_list.size()-1))
            {
                max_element_iter = current;
            }
        }
        auto parent_index = std::distance(viterbi.begin(), max_element_iter);
        std::cout << "Selected maxium: " << max_element_iter->at(timestamp_list.size()-1) << " at index " << parent_index << std::endl;
        std::deque<std::size_t> reconstructed_indices;

        SimpleLogger().Write() << "Backtracking to find most plausible state sequence";
        for (auto i = timestamp_list.size() - 1u; i > 0u; --i)
        {
            SimpleLogger().Write() << "[" << i << "] select: " << parent_index ;
            reconstructed_indices.push_front(parent_index);
            parent_index = parent[parent_index][i];
        }
        SimpleLogger().Write() << "[0] parent: " << parent_index;
        reconstructed_indices.push_front(parent_index);

        SimpleLogger().Write() << "Computing most plausible sequence of phantom nodes";

        JSON::Array chosen_candidates;
        matched_nodes.resize(reconstructed_indices.size());
        for (auto i = 0u; i < reconstructed_indices.size(); ++i)
        {
            auto location_index = reconstructed_indices[i];
            matched_nodes[i] = timestamp_list[i][location_index].first;
            chosen_candidates.values.push_back(location_index);
        }
        debug_info.values["chosen_candidates"] = chosen_candidates;

        SimpleLogger().Write() << "done";
    }
};

//[1] "Hidden Markov Map Matching Through Noise and Sparseness"; P. Newson and J. Krumm; 2009; ACM GIS

#endif /* MAP_MATCHING_H */