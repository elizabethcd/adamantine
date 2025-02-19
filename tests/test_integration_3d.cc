/* Copyright (c) 2016 - 2023, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#define BOOST_TEST_MODULE Integration_3D

#include "../application/adamantine.hh"

#include <filesystem>
#include <fstream>

#include "main.cc"

namespace utf = boost::unit_test;

BOOST_AUTO_TEST_CASE(integration_3D, *utf::tolerance(0.1))
{
  MPI_Comm communicator = MPI_COMM_WORLD;

  std::vector<adamantine::Timer> timers;
  initialize_timers(communicator, timers);

  // Read the input.
  std::string const filename = "demo_316_short_anisotropic.info";
  adamantine::ASSERT_THROW(std::filesystem::exists(filename) == true,
                           "The file " + filename + " does not exist.");
  boost::property_tree::ptree database;
  boost::property_tree::info_parser::read_info(filename, database);

  auto [temperature, displacement] =
      run<3, dealii::MemorySpace::Host>(communicator, database, timers);

  int num_ranks = 0;
  MPI_Comm_size(communicator, &num_ranks);

  // Limits for a weak non-pointwise check
  double max_expected = 500.0;
  double min_expected = 285.0;

  if (num_ranks == 1)
  {
    std::ifstream gold_file("integration_3d_gold.txt");

    for (unsigned int i = 0; i < temperature.locally_owned_size(); ++i)
    {
      BOOST_CHECK(temperature.local_element(i) > min_expected);
      BOOST_CHECK(temperature.local_element(i) < max_expected);

      double gold_value = -1.;
      gold_file >> gold_value;
      BOOST_TEST(temperature.local_element(i) == gold_value);
    }
  }
  else if (num_ranks == 2)
  {
    int rank = 0;
    MPI_Comm_rank(communicator, &rank);

    // To write the gold file
    /*
    std::ofstream gold_file_writer("integration_3d_gold_" +
                                   std::to_string(rank) + ".txt");
    for (unsigned int i = 0; i < temperature.locally_owned_size(); ++i)
      gold_file_writer << temperature.local_element(i) << " ";

    gold_file_writer.close();
    */

    std::ifstream gold_file("integration_3d_gold_" + std::to_string(rank) +
                            ".txt");

    for (unsigned int i = 0; i < temperature.locally_owned_size(); ++i)
    {
      BOOST_CHECK(temperature.local_element(i) > min_expected);
      BOOST_CHECK(temperature.local_element(i) < max_expected);

      double gold_value = -1.;
      gold_file >> gold_value;
      BOOST_TEST(temperature.local_element(i) == gold_value);
    }
  }
  else
  {
    // Only the weaker non-pointwise check for more than 2 cores
    for (unsigned int i = 0; i < temperature.locally_owned_size(); ++i)
    {
      BOOST_CHECK(temperature.local_element(i) > min_expected);
      BOOST_CHECK(temperature.local_element(i) < max_expected);
    }
  }
}
