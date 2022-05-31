/*
 * Copyright (c) 2022, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utilities/base_fixture.hpp>
#include <utilities/device_comm_wrapper.hpp>
#include <utilities/high_res_clock.h>
#include <utilities/test_graphs.hpp>
#include <utilities/test_utilities.hpp>
#include <utilities/thrust_wrapper.hpp>

#include <cugraph/algorithms.hpp>
#include <cugraph/partition_manager.hpp>

#include <raft/comms/comms.hpp>
#include <raft/comms/mpi_comms.hpp>
#include <raft/handle.hpp>
#include <rmm/device_scalar.hpp>
#include <rmm/device_uvector.hpp>

#include <gtest/gtest.h>

#include <random>

struct EigenvectorCentrality_Usecase {
  size_t max_iterations{std::numeric_limits<size_t>::max()};
  bool test_weighted{false};
  bool check_correctness{true};
};

template <typename input_usecase_t>
class Tests_MGEigenvectorCentrality
  : public ::testing::TestWithParam<std::tuple<EigenvectorCentrality_Usecase, input_usecase_t>> {
 public:
  Tests_MGEigenvectorCentrality() {}
  static void SetupTestCase() {}
  static void TearDownTestCase() {}

  virtual void SetUp() {}
  virtual void TearDown() {}

  // Compare the results of running Eigenvector Centrality on multiple GPUs to that of a single-GPU
  // run
  template <typename vertex_t, typename edge_t, typename weight_t>
  void run_current_test(EigenvectorCentrality_Usecase const& eigenvector_usecase,
                        input_usecase_t const& input_usecase)
  {
    // 1. initialize handle

    raft::handle_t handle{};
    HighResClock hr_clock{};

    raft::comms::initialize_mpi_comms(&handle, MPI_COMM_WORLD);
    auto& comm           = handle.get_comms();
    auto const comm_size = comm.get_size();
    auto const comm_rank = comm.get_rank();

    auto row_comm_size = static_cast<int>(sqrt(static_cast<double>(comm_size)));
    while (comm_size % row_comm_size != 0) {
      --row_comm_size;
    }
    cugraph::partition_2d::subcomm_factory_t<cugraph::partition_2d::key_naming_t, vertex_t>
      subcomm_factory(handle, row_comm_size);

    // 2. create MG graph

    if (cugraph::test::g_perf) {
      RAFT_CUDA_TRY(cudaDeviceSynchronize());  // for consistent performance measurement
      handle.get_comms().barrier();
      hr_clock.start();
    }

    auto [mg_graph, d_mg_renumber_map_labels] =
      cugraph::test::construct_graph<vertex_t, edge_t, weight_t, true, true>(
        handle, input_usecase, eigenvector_usecase.test_weighted, true);

    if (cugraph::test::g_perf) {
      RAFT_CUDA_TRY(cudaDeviceSynchronize());  // for consistent performance measurement
      handle.get_comms().barrier();
      double elapsed_time{0.0};
      hr_clock.stop(&elapsed_time);
      std::cout << "MG construct_graph took " << elapsed_time * 1e-6 << " s.\n";
    }

    auto mg_graph_view = mg_graph.view();

    // 3. run MG Eigenvector Centrality

    weight_t constexpr epsilon{1e-6};

    rmm::device_uvector<weight_t> d_mg_centralities(
      mg_graph_view.local_vertex_partition_range_size(), handle.get_stream());

    if (cugraph::test::g_perf) {
      RAFT_CUDA_TRY(cudaDeviceSynchronize());  // for consistent performance measurement
      handle.get_comms().barrier();
      hr_clock.start();
    }

    d_mg_centralities = cugraph::eigenvector_centrality(
      handle,
      mg_graph_view,
      std::optional<raft::device_span<weight_t const>>{},
      // std::make_optional(raft::device_span<weight_t
      // const>{d_mg_centralities.data(), d_mg_centralities.size()}),
      epsilon,
      eigenvector_usecase.max_iterations,
      false);

    if (cugraph::test::g_perf) {
      RAFT_CUDA_TRY(cudaDeviceSynchronize());  // for consistent performance measurement
      handle.get_comms().barrier();
      double elapsed_time{0.0};
      hr_clock.stop(&elapsed_time);
      std::cout << "MG Eigenvector Centrality took " << elapsed_time * 1e-6 << " s.\n";
    }

    // 4. compare SG & MG results

    if (eigenvector_usecase.check_correctness) {
      // 4-1. aggregate MG results

      auto d_mg_aggregate_renumber_map_labels = cugraph::test::device_gatherv(
        handle, (*d_mg_renumber_map_labels).data(), (*d_mg_renumber_map_labels).size());
      auto d_mg_aggregate_centralities =
        cugraph::test::device_gatherv(handle, d_mg_centralities.data(), d_mg_centralities.size());

      if (handle.get_comms().get_rank() == int{0}) {
        // 4-2. Sort MG results by original vertex id
        std::tie(std::ignore, d_mg_aggregate_centralities) = cugraph::test::sort_by_key(
          handle, d_mg_aggregate_renumber_map_labels, d_mg_aggregate_centralities);

        // 4-3. create SG graph
        auto [sg_graph, d_sg_renumber_map_labels] =
          cugraph::test::construct_graph<vertex_t, edge_t, weight_t, true, false>(
            handle, input_usecase, eigenvector_usecase.test_weighted, true);

        auto sg_graph_view = sg_graph.view();

        ASSERT_TRUE(mg_graph_view.number_of_vertices() == sg_graph_view.number_of_vertices());

        // 4-4. run SG Eigenvector Centrality
        rmm::device_uvector<weight_t> d_sg_centralities(sg_graph_view.number_of_vertices(),
                                                        handle.get_stream());

        d_sg_centralities = cugraph::eigenvector_centrality(
          handle,
          sg_graph_view,
          std::optional<raft::device_span<weight_t const>>{},
          // std::make_optional(raft::device_span<weight_t const>{d_sg_centralities.data(),
          // d_sg_centralities.size()}),
          epsilon,
          eigenvector_usecase.max_iterations,
          false);

        std::tie(std::ignore, d_sg_centralities) =
          cugraph::test::sort_by_key(handle, *d_sg_renumber_map_labels, d_sg_centralities);

        // 4-5. compare
        std::vector<weight_t> h_mg_aggregate_centralities(mg_graph_view.number_of_vertices());
        raft::update_host(h_mg_aggregate_centralities.data(),
                          d_mg_aggregate_centralities.data(),
                          d_mg_aggregate_centralities.size(),
                          handle.get_stream());

        std::vector<weight_t> h_sg_centralities(sg_graph_view.number_of_vertices());
        raft::update_host(h_sg_centralities.data(),
                          d_sg_centralities.data(),
                          d_sg_centralities.size(),
                          handle.get_stream());

        handle.sync_stream();

        auto max_centrality =
          *std::max_element(h_mg_aggregate_centralities.begin(), h_mg_aggregate_centralities.end());

        // skip comparison for low Eigenvector Centrality vertices (lowly ranked vertices)
        auto threshold_magnitude = max_centrality * epsilon;

        auto nearly_equal = [epsilon, threshold_magnitude](auto lhs, auto rhs) {
          return std::abs(lhs - rhs) < std::max(std::max(lhs, rhs) * epsilon, threshold_magnitude);
        };

        // FIND DIFFERENCES...
        size_t count_differences{0};
        for (size_t i = 0; i < h_mg_aggregate_centralities.size(); ++i) {
          if (nearly_equal(h_mg_aggregate_centralities[i], h_sg_centralities[i])) {
          } else {
            if (count_differences < 10) {
              std::cout << "unequal [" << i << "] " << h_mg_aggregate_centralities[i]
                        << " != " << h_sg_centralities[i] << std::endl;
            }
            ++count_differences;
          }
        }

        ASSERT_EQ(count_differences, size_t{0})
          << "Eigenvector centrality values do not match with the reference "
             "values.";
      }
    }
  }
};

using Tests_MGEigenvectorCentrality_File =
  Tests_MGEigenvectorCentrality<cugraph::test::File_Usecase>;
using Tests_MGEigenvectorCentrality_Rmat =
  Tests_MGEigenvectorCentrality<cugraph::test::Rmat_Usecase>;

TEST_P(Tests_MGEigenvectorCentrality_File, CheckInt32Int32FloatFloat)
{
  auto param = GetParam();
  run_current_test<int32_t, int32_t, float>(std::get<0>(param), std::get<1>(param));
}

TEST_P(Tests_MGEigenvectorCentrality_Rmat, CheckInt32Int32FloatFloat)
{
  auto param = GetParam();
  run_current_test<int32_t, int32_t, float>(
    std::get<0>(param), override_Rmat_Usecase_with_cmd_line_arguments(std::get<1>(param)));
}

TEST_P(Tests_MGEigenvectorCentrality_Rmat, CheckInt32Int64FloatFloat)
{
  auto param = GetParam();
  run_current_test<int32_t, int64_t, float>(
    std::get<0>(param), override_Rmat_Usecase_with_cmd_line_arguments(std::get<1>(param)));
}

TEST_P(Tests_MGEigenvectorCentrality_Rmat, CheckInt64Int64FloatFloat)
{
  auto param = GetParam();
  run_current_test<int64_t, int64_t, float>(
    std::get<0>(param), override_Rmat_Usecase_with_cmd_line_arguments(std::get<1>(param)));
}

INSTANTIATE_TEST_SUITE_P(
  file_test,
  Tests_MGEigenvectorCentrality_File,
  ::testing::Combine(
    // enable correctness checks
    ::testing::Values(EigenvectorCentrality_Usecase{500, false},
                      EigenvectorCentrality_Usecase{500, true}),
    ::testing::Values(cugraph::test::File_Usecase("test/datasets/karate.mtx"),
                      cugraph::test::File_Usecase("test/datasets/web-Google.mtx"),
                      cugraph::test::File_Usecase("test/datasets/ljournal-2008.mtx"),
                      cugraph::test::File_Usecase("test/datasets/webbase-1M.mtx"))));

INSTANTIATE_TEST_SUITE_P(rmat_small_test,
                         Tests_MGEigenvectorCentrality_Rmat,
                         ::testing::Combine(
                           // enable correctness checks
                           ::testing::Values(EigenvectorCentrality_Usecase{500, false},
                                             EigenvectorCentrality_Usecase{500, true}),
                           ::testing::Values(cugraph::test::Rmat_Usecase(
                             10, 16, 0.57, 0.19, 0.19, 0, false, false, 0, true))));

INSTANTIATE_TEST_SUITE_P(
  rmat_benchmark_test, /* note that scale & edge factor can be overridden in benchmarking (with
                          --gtest_filter to select only the rmat_benchmark_test with a specific
                          vertex & edge type combination) by command line arguments and do not
                          include more than one Rmat_Usecase that differ only in scale or edge
                          factor (to avoid running same benchmarks more than once) */
  Tests_MGEigenvectorCentrality_Rmat,
  ::testing::Combine(
    // disable correctness checks for large graphs
    ::testing::Values(EigenvectorCentrality_Usecase{500, false, false},
                      EigenvectorCentrality_Usecase{500, true, false}),
    ::testing::Values(
      cugraph::test::Rmat_Usecase(20, 32, 0.57, 0.19, 0.19, 0, false, false, 0, true))));

CUGRAPH_MG_TEST_PROGRAM_MAIN()
