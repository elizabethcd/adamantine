/* Copyright (c) 2016 - 2022, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#include <MaterialProperty.hh>
#include <MemoryBlock.hh>
#include <MemoryBlockView.hh>

#include <deal.II/base/aligned_vector.h>
#include <deal.II/base/array_view.h>
#include <deal.II/base/cuda.h>
#include <deal.II/base/point.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/types.h>
#include <deal.II/base/vectorization.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping.h>
#include <deal.II/grid/manifold.h>
#include <deal.II/hp/fe_values.h>
#include <deal.II/hp/q_collection.h>
#include <deal.II/lac/la_parallel_vector.h>

#include <boost/algorithm/string/split.hpp>
#include <boost/optional.hpp>

#include <algorithm>
#include <type_traits>

namespace adamantine
{
namespace internal
{
template <int dim>
void compute_average(
    unsigned int const n_q_points, unsigned int const dofs_per_cell,
    dealii::DoFHandler<dim> const &mp_dof_handler,
    dealii::DoFHandler<dim> const &temperature_dof_handler,
    dealii::hp::FEValues<dim> &hp_fe_values,
    dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host> const
        &temperature,
    dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host>
        &temperature_average)
{
  std::vector<dealii::types::global_dof_index> mp_dof_indices(1);
  std::vector<dealii::types::global_dof_index> enth_dof_indices(dofs_per_cell);
  auto mp_cell = mp_dof_handler.begin_active();
  auto mp_end_cell = mp_dof_handler.end();
  auto enth_cell = temperature_dof_handler.begin_active();
  for (; mp_cell != mp_end_cell; ++enth_cell, ++mp_cell)
  {
    ASSERT(mp_cell->is_locally_owned() == enth_cell->is_locally_owned(),
           "Internal Error");
    if ((mp_cell->is_locally_owned()) && (enth_cell->active_fe_index() == 0))
    {
      hp_fe_values.reinit(enth_cell);
      dealii::FEValues<dim> const &fe_values =
          hp_fe_values.get_present_fe_values();
      mp_cell->get_dof_indices(mp_dof_indices);
      dealii::types::global_dof_index const mp_dof_index = mp_dof_indices[0];
      enth_cell->get_dof_indices(enth_dof_indices);
      double volume = 0.;
      for (unsigned int q = 0; q < n_q_points; ++q)
        for (unsigned int i = 0; i < dofs_per_cell; ++i)
        {
          volume += fe_values.shape_value(i, q) * fe_values.JxW(q);
          temperature_average[mp_dof_index] +=
              fe_values.shape_value(i, q) * temperature[enth_dof_indices[i]] *
              fe_values.JxW(q);
        }
      temperature_average[mp_dof_index] /= volume;
    }
  }
}

template <typename MemorySpaceType>
double get_value(MemoryBlock<double, MemorySpaceType> const &memory_block,
                 unsigned int i, unsigned int j)
{
  MemoryBlockView<double, MemorySpaceType> memory_block_view(memory_block);
  return memory_block_view(i, j);
}

#ifdef __CUDACC__
template <int dim>
void compute_average(
    unsigned int const n_q_points, unsigned int const dofs_per_cell,
    dealii::DoFHandler<dim> const &mp_dof_handler,
    dealii::DoFHandler<dim> const &temperature_dof_handler,
    dealii::hp::FEValues<dim> &hp_fe_values,
    dealii::LA::distributed::Vector<double, dealii::MemorySpace::CUDA> const
        &temperature,
    dealii::LA::distributed::Vector<double, dealii::MemorySpace::CUDA>
        &temperature_average)
{
  dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host>
      temperature_host(temperature.get_partitioner());
  temperature_host.import(temperature, dealii::VectorOperation::insert);
  dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host>
      temperature_average_host(temperature_average.get_partitioner());
  temperature_average_host = 0.;
  compute_average(n_q_points, dofs_per_cell, mp_dof_handler,
                  temperature_dof_handler, hp_fe_values, temperature_host,
                  temperature_average_host);

  temperature_average.import(temperature_average_host,
                             dealii::VectorOperation::insert);
}

template <>
double get_value<dealii::MemorySpace::CUDA>(
    MemoryBlock<double, dealii::MemorySpace::CUDA> const &memory_block,
    unsigned int i, unsigned int j)
{
  MemoryBlock<double, dealii::MemorySpace::Host> memory_block_host(
      memory_block);
  MemoryBlockView<double, dealii::MemorySpace::Host> memory_block_host_view(
      memory_block_host);
  return memory_block_host_view(i, j);
}
#endif
} // namespace internal

template <int dim, typename MemorySpaceType>
MaterialProperty<dim, MemorySpaceType>::MaterialProperty(
    MPI_Comm const &communicator,
    dealii::parallel::distributed::Triangulation<dim> const &tria,
    boost::property_tree::ptree const &database)
    : _communicator(communicator), _fe(0), _mp_dof_handler(tria)
{
  // Because deal.II cannot easily attach data to a cell. We store the state
  // of the material in distributed::Vector. This allows to use deal.II to
  // compute the new state after refinement of the mesh. However, this
  // requires to use another DoFHandler.
  reinit_dofs();

  // Set the material state to the state defined in the geometry.
  set_initial_state();

  // Fill the _properties map
  fill_properties(database);
}

template <int dim, typename MemorySpaceType>
double MaterialProperty<dim, MemorySpaceType>::get_cell_value(
    typename dealii::Triangulation<dim>::active_cell_iterator const &cell,
    StateProperty prop) const
{
  unsigned int property = static_cast<unsigned int>(prop);
  auto const mp_dof_index = get_dof_index(cell);

  // FIXME this is extremely slow on CUDA but this function should not exist in
  // the first place
  return internal::get_value(_property_values, property, mp_dof_index);
}

template <int dim, typename MemorySpaceType>
double MaterialProperty<dim, MemorySpaceType>::get_cell_value(
    typename dealii::Triangulation<dim>::active_cell_iterator const &cell,
    Property prop) const
{
  dealii::types::material_id material_id = cell->material_id();
  unsigned int property = static_cast<unsigned int>(prop);

  // FIXME this is extremely slow on CUDA but this function should not exist in
  // the first place
  return internal::get_value(_properties, material_id, property);
}

template <int dim, typename MemorySpaceType>
double MaterialProperty<dim, MemorySpaceType>::get_mechanical_property(
    typename dealii::Triangulation<dim>::active_cell_iterator const &cell,
    StateProperty prop) const
{
  unsigned int property =
      static_cast<unsigned int>(prop) - g_n_thermal_state_properties;
  ASSERT(property < g_n_mechanical_state_properties,
         "Unknown mechanical property requested.");
  MemoryBlockView<double, dealii::MemorySpace::Host>
      mechanical_properties_host_view(_mechanical_properties_host);
  return mechanical_properties_host_view(cell->material_id(), property);
}

template <int dim, typename MemorySpaceType>
double MaterialProperty<dim, MemorySpaceType>::get_state_ratio(
    typename dealii::Triangulation<dim>::active_cell_iterator const &cell,
    MaterialState material_state) const
{
  auto const mp_dof_index = get_dof_index(cell);
  auto const mat_state = static_cast<unsigned int>(material_state);

  // FIXME this is extremely slow on CUDA but this function should not exist in
  // the first place
  return internal::get_value(_state, mat_state, mp_dof_index);
}

template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::reinit_dofs()
{
  _mp_dof_handler.distribute_dofs(_fe);

  // Initialize _dofs_map
  _dofs_map.clear();
  unsigned int i = 0;
  std::vector<dealii::types::global_dof_index> mp_dof(1);
  for (auto cell :
       dealii::filter_iterators(_mp_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    cell->get_dof_indices(mp_dof);
    _dofs_map[mp_dof[0]] = i;
    ++i;
  }

  _state.reinit(g_n_material_states, _dofs_map.size());
#ifdef ADAMANTINE_DEBUG
  if constexpr (std::is_same_v<MemorySpaceType, dealii::MemorySpace::Host>)
  {
    MemoryBlockView<double, MemorySpaceType> state_view(_state);
    for_each(MemorySpaceType{}, g_n_material_states,
             [=](int i) mutable
             {
               for (unsigned int j = 0; j < _dofs_map.size(); ++j)
                 state_view(i, j) =
                     std::numeric_limits<double>::signaling_NaN();
               ;
             });
  }
#endif
}

template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::update(
    dealii::DoFHandler<dim> const &temperature_dof_handler,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &temperature)
{
  auto temperature_average =
      compute_average_temperature(temperature_dof_handler, temperature);
  _property_values.reinit(g_n_thermal_state_properties, _dofs_map.size());
  _property_values.set_zero();

  std::vector<dealii::types::global_dof_index> mp_dofs;
  std::vector<dealii::types::material_id> material_ids;
  for (auto cell :
       dealii::filter_iterators(_mp_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    std::vector<dealii::types::global_dof_index> mp_dof(1);
    cell->get_dof_indices(mp_dof);
    mp_dofs.push_back(_dofs_map.at(mp_dof[0]));
    material_ids.push_back(cell->material_id());
  }

  unsigned int const material_ids_size = material_ids.size();
  MemoryBlock<dealii::types::material_id, MemorySpaceType> material_ids_block(
      material_ids);
  MemoryBlockView<dealii::types::material_id, MemorySpaceType>
      material_ids_view(material_ids_block);
  MemoryBlock<dealii::types::global_dof_index, MemorySpaceType> mp_dofs_block(
      mp_dofs);
  MemoryBlockView<dealii::types::global_dof_index, MemorySpaceType>
      mp_dofs_view(mp_dofs_block);

  double *temperature_average_local = temperature_average.get_values();

  MemoryBlockView<double, MemorySpaceType> state_property_polynomials_view(
      _state_property_polynomials);
  MemoryBlockView<double, MemorySpaceType> properties_view(_properties);
  MemoryBlockView<double, MemorySpaceType> state_view(_state);
  MemoryBlockView<double, MemorySpaceType> property_values_view(
      _property_values);
  MemoryBlockView<double, MemorySpaceType> state_property_tables_view(
      _state_property_tables);

  bool use_table = _use_table;
  for_each(
      MemorySpaceType{}, material_ids_size,
      [=] ADAMANTINE_HOST_DEV(int i)
      {
        unsigned int constexpr liquid =
            static_cast<unsigned int>(MaterialState::liquid);
        unsigned int constexpr powder =
            static_cast<unsigned int>(MaterialState::powder);
        unsigned int constexpr solid =
            static_cast<unsigned int>(MaterialState::solid);
        unsigned int constexpr prop_solidus =
            static_cast<unsigned int>(Property::solidus);
        unsigned int constexpr prop_liquidus =
            static_cast<unsigned int>(Property::liquidus);

        dealii::types::material_id material_id = material_ids_view(i);
        double const solidus = properties_view(material_id, prop_solidus);
        double const liquidus = properties_view(material_id, prop_liquidus);
        unsigned int const dof = mp_dofs_view(i);

        // First determine the ratio of liquid.
        double liquid_ratio = -1.;
        double powder_ratio = -1.;
        double solid_ratio = -1.;
        if (temperature_average_local[dof] < solidus)
          liquid_ratio = 0.;
        else if (temperature_average_local[dof] > liquidus)
          liquid_ratio = 1.;
        else
          liquid_ratio =
              (temperature_average_local[dof] - solidus) / (liquidus - solidus);
        // Because the powder can only become liquid, the solid can only
        // become liquid, and the liquid can only become solid, the ratio of
        // powder can only decrease.
        powder_ratio = std::min(1. - liquid_ratio, state_view(powder, dof));
        // Use max to make sure that we don't create matter because of
        // round-off.
        solid_ratio = std::max(1 - liquid_ratio - powder_ratio, 0.);

        // Update the value
        state_view(liquid, dof) = liquid_ratio;
        state_view(powder, dof) = powder_ratio;
        state_view(solid, dof) = solid_ratio;

        if (use_table)
        {
          for (unsigned int property = 0;
               property < g_n_thermal_state_properties; ++property)
          {
            for (unsigned int material_state = 0;
                 material_state < g_n_material_states; ++material_state)
            {
              property_values_view(property, dof) +=
                  state_view(material_state, dof) *
                  compute_property_from_table(
                      state_property_tables_view, material_id, material_state,
                      property, temperature_average_local[dof]);
            }
          }
        }
        else
        {
          for (unsigned int property = 0;
               property < g_n_thermal_state_properties; ++property)
          {
            for (unsigned int material_state = 0;
                 material_state < g_n_material_states; ++material_state)
            {
              for (unsigned int i = 0; i <= polynomial_order; ++i)
              {
                property_values_view(property, dof) +=
                    state_view(material_state, dof) *
                    state_property_polynomials_view(material_id, material_state,
                                                    property, i) *
                    std::pow(temperature_average_local[dof], i);
              }
            }
          }
        }

        // If we are in the mushy state, i.e., part liquid part solid, we need
        // to modify the rho C_p to take into account the latent heat.
        if ((liquid_ratio > 0.) && (liquid_ratio < 1.))
        {
          unsigned int const specific_heat_prop =
              static_cast<unsigned int>(StateProperty::specific_heat);
          unsigned int const latent_heat_prop =
              static_cast<unsigned int>(Property::latent_heat);
          for (unsigned int material_state = 0;
               material_state < g_n_material_states; ++material_state)
          {
            property_values_view(specific_heat_prop, dof) +=
                state_view(material_state, dof) *
                properties_view(material_id, latent_heat_prop) /
                (liquidus - solidus);
          }
        }

        // The radiation heat transfer coefficient is not a real material
        // property but it is derived from other material properties: h_rad =
        // emissitivity * stefan-boltzmann constant * (T + T_infty) (T^2 +
        // T^2_infty).
        unsigned int const emissivity_prop =
            static_cast<unsigned int>(StateProperty::emissivity);
        unsigned int const radiation_heat_transfer_coef_prop =
            static_cast<unsigned int>(
                StateProperty::radiation_heat_transfer_coef);
        unsigned int const radiation_temperature_infty_prop =
            static_cast<unsigned int>(Property::radiation_temperature_infty);
        double const T = temperature_average_local[dof];
        double const T_infty =
            properties_view(material_id, radiation_temperature_infty_prop);
        double const emissivity = property_values_view(emissivity_prop, dof);
        property_values_view(radiation_heat_transfer_coef_prop, dof) =
            emissivity * Constant::stefan_boltzmann * (T + T_infty) *
            (T * T + T_infty * T_infty);
      });
}

// TODO When we can get rid of this function, we can remove
// StateProperty::radiation_heat_transfer_coef
template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::
    update_boundary_material_properties(
        dealii::DoFHandler<dim> const &temperature_dof_handler,
        dealii::LA::distributed::Vector<double, MemorySpaceType> const
            &temperature)
{
  auto temperature_average =
      compute_average_temperature(temperature_dof_handler, temperature);
  _property_values.reinit(g_n_thermal_state_properties, _dofs_map.size());
  _property_values.set_zero();

  std::vector<dealii::types::global_dof_index> mp_dof(1);
  // We don't need to loop over all the active cells. We only need to loop over
  // the cells at the boundary and at the interface with FE_Nothing. However, to
  // do this we need to use the temperature_dof_handler instead of the
  // _mp_dof_handler.
  MemoryBlockView<double, MemorySpaceType> state_property_polynomials_view(
      _state_property_polynomials);
  MemoryBlockView<double, MemorySpaceType> properties_view(_properties);
  MemoryBlockView<double, MemorySpaceType> state_view(_state);
  MemoryBlockView<double, MemorySpaceType> property_values_view(
      _property_values);
  MemoryBlockView<double, MemorySpaceType> state_property_tables_view(
      _state_property_tables);
  for (auto cell :
       dealii::filter_iterators(_mp_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    dealii::types::material_id material_id = cell->material_id();

    cell->get_dof_indices(mp_dof);
    unsigned int const dof = _dofs_map.at(mp_dof[0]);
    if (_use_table)
    {
      // We only care about properties that are used to compute the boundary
      // condition. So we start at 3.
      for (unsigned int property = 3; property < g_n_thermal_state_properties;
           ++property)
      {
        for (unsigned int material_state = 0;
             material_state < g_n_material_states; ++material_state)
        {
          property_values_view(property, dof) +=
              state_view(material_state, dof) *
              compute_property_from_table(
                  state_property_tables_view, material_id, material_state,
                  property, temperature_average.local_element(dof));
        }
      }
    }
    else
    {
      // We only care about properties that are used to compute the boundary
      // condition. So we start at 3.
      for (unsigned int property = 3; property < g_n_thermal_state_properties;
           ++property)
      {
        for (unsigned int material_state = 0;
             material_state < g_n_material_states; ++material_state)
        {
          for (unsigned int i = 0; i <= polynomial_order; ++i)
          {
            property_values_view(property, dof) +=
                state_view(material_state, dof) *
                state_property_polynomials_view(material_id, material_state,
                                                property, i) *
                std::pow(temperature_average.local_element(dof), i);
          }
        }
      }
    }

    // The radiation heat transfer coefficient is not a real material property
    // but it is derived from other material properties:
    // h_rad = emissitivity * stefan-boltzmann constant * (T + T_infty) (T^2 +
    // T^2_infty).
    unsigned int const emissivity_prop =
        static_cast<unsigned int>(StateProperty::emissivity);
    unsigned int const radiation_heat_transfer_coef_prop =
        static_cast<unsigned int>(StateProperty::radiation_heat_transfer_coef);
    unsigned int const radiation_temperature_infty_prop =
        static_cast<unsigned int>(Property::radiation_temperature_infty);
    double const T = temperature_average.local_element(dof);
    double const T_infty =
        properties_view(material_id, radiation_temperature_infty_prop);
    double const emissivity = property_values_view(emissivity_prop, dof);
    property_values_view(radiation_heat_transfer_coef_prop, dof) =
        emissivity * Constant::stefan_boltzmann * (T + T_infty) *
        (T * T + T_infty * T_infty);
  }
}

template <int dim, typename MemorySpaceType>
dealii::VectorizedArray<double>
MaterialProperty<dim, MemorySpaceType>::compute_material_property(
    StateProperty state_property, dealii::types::material_id const *material_id,
    dealii::VectorizedArray<double> const *state_ratios,
    dealii::VectorizedArray<double> const &temperature,
    dealii::AlignedVector<dealii::VectorizedArray<double>> const
        &temperature_powers) const
{
  dealii::VectorizedArray<double> value = 0.0;
  unsigned int const property_index = static_cast<unsigned int>(state_property);
  MemoryBlockView<double, MemorySpaceType> state_property_polynomials_view(
      _state_property_polynomials);
  MemoryBlockView<double, MemorySpaceType> state_property_tables_view(
      _state_property_tables);

  if (_use_table)
  {
    for (unsigned int material_state = 0; material_state < g_n_material_states;
         ++material_state)
    {
      for (unsigned int n = 0; n < dealii::VectorizedArray<double>::size(); ++n)
      {
        const dealii::types::material_id m_id = material_id[n];

        value[n] += state_ratios[material_state][n] *
                    compute_property_from_table(state_property_tables_view,
                                                m_id, material_state,
                                                property_index, temperature[n]);
      }
    }
  }
  else
  {
    for (unsigned int material_state = 0; material_state < g_n_material_states;
         ++material_state)
    {
      for (unsigned int n = 0; n < dealii::VectorizedArray<double>::size(); ++n)
      {
        dealii::types::material_id m_id = material_id[n];

        for (unsigned int i = 0; i <= polynomial_order; ++i)
        {
          value[n] += state_ratios[material_state][n] *
                      state_property_polynomials_view(m_id, material_state,
                                                      property_index, i) *
                      temperature_powers[i][n];
        }
      }
    }
  }

  return value;
}

template <int dim, typename MemorySpaceType>
ADAMANTINE_HOST_DEV double
MaterialProperty<dim, MemorySpaceType>::compute_material_property(
    StateProperty state_property, dealii::types::material_id const material_id,
    double const *state_ratios, double temperature) const
{
  double value = 0.0;
  unsigned int const property_index = static_cast<unsigned int>(state_property);
  MemoryBlockView<double, MemorySpaceType> state_property_polynomials_view(
      _state_property_polynomials);
  MemoryBlockView<double, MemorySpaceType> state_property_tables_view(
      _state_property_tables);

  if (_use_table)
  {
    for (unsigned int material_state = 0; material_state < g_n_material_states;
         ++material_state)
    {
      const dealii::types::material_id m_id = material_id;

      value += state_ratios[material_state] *
               compute_property_from_table(state_property_tables_view, m_id,
                                           material_state, property_index,
                                           temperature);
    }
  }
  else
  {
    for (unsigned int material_state = 0; material_state < g_n_material_states;
         ++material_state)
    {
      dealii::types::material_id m_id = material_id;

      for (unsigned int i = 0; i <= polynomial_order; ++i)
      {
        value += state_ratios[material_state] *
                 state_property_polynomials_view(m_id, material_state,
                                                 property_index, i) *
                 std::pow(temperature, i);
      }
    }
  }

  return value;
}

template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::set_state(
    dealii::Table<2, dealii::VectorizedArray<double>> const &liquid_ratio,
    dealii::Table<2, dealii::VectorizedArray<double>> const &powder_ratio,
    std::map<typename dealii::DoFHandler<dim>::cell_iterator,
             std::pair<unsigned int, unsigned int>> &cell_it_to_mf_cell_map,
    dealii::DoFHandler<dim> const &dof_handler)
{
  auto const powder_state = static_cast<unsigned int>(MaterialState::powder);
  auto const liquid_state = static_cast<unsigned int>(MaterialState::liquid);
  auto const solid_state = static_cast<unsigned int>(MaterialState::solid);
  std::vector<dealii::types::global_dof_index> mp_dof(1.);

  MemoryBlockView<double, MemorySpaceType> state_view(_state);
  for (auto const &cell :
       dealii::filter_iterators(dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    typename dealii::Triangulation<dim>::active_cell_iterator cell_tria(cell);
    auto mp_dof_index = get_dof_index(cell_tria);
    auto const &mf_cell_vector = cell_it_to_mf_cell_map[cell];
    unsigned int const n_q_points = dof_handler.get_fe().tensor_degree() + 1;
    double liquid_ratio_sum = 0.;
    double powder_ratio_sum = 0.;
    for (unsigned int q = 0; q < n_q_points; ++q)
    {
      liquid_ratio_sum +=
          liquid_ratio(mf_cell_vector.first, q)[mf_cell_vector.second];
      powder_ratio_sum +=
          powder_ratio(mf_cell_vector.first, q)[mf_cell_vector.second];
    }
    state_view(liquid_state, mp_dof_index) = liquid_ratio_sum / n_q_points;
    state_view(powder_state, mp_dof_index) = powder_ratio_sum / n_q_points;
    state_view(solid_state, mp_dof_index) =
        std::max(1. - state_view(liquid_state, mp_dof_index) -
                     state_view(powder_state, mp_dof_index),
                 0.);
  }
}

#ifdef __CUDACC__
template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::set_state_device(
    MemoryBlock<double, MemorySpaceType> const &liquid_ratio,
    MemoryBlock<double, MemorySpaceType> const &powder_ratio,
    std::map<typename dealii::DoFHandler<dim>::cell_iterator,
             std::vector<unsigned int>> const &_cell_it_to_mf_pos,
    dealii::DoFHandler<dim> const &dof_handler)
{
  // Create a mapping between the matrix free dofs and material property dofs
  std::vector<dealii::types::global_dof_index> mp_dof(1.);
  unsigned int const n_q_points = dof_handler.get_fe().tensor_degree() + 1;
  MemoryBlock<unsigned int, dealii::MemorySpace::Host> mapping_host(
      _state.extent(1), n_q_points);
  MemoryBlockView<unsigned, dealii::MemorySpace::Host> mapping_host_view(
      mapping_host);
  MemoryBlock<double, dealii::MemorySpace::Host> mp_dof_host_block(
      _state.extent(1));
  MemoryBlockView<double, dealii::MemorySpace::Host> mp_dof_host_view(
      mp_dof_host_block);
  // We only loop over the part of the domain which has material, i.e., not over
  // FE_Nothing cell. This is because _cell_it_to_mf_pos does not exist for
  // FE_Nothing cells. However, we have set the state of the material on the
  // entire domain. This is not a problem since that state is unchanged and does
  // not need to be updated.
  unsigned int cell_i = 0;
  for (auto const &cell : dealii::filter_iterators(
           dof_handler.active_cell_iterators(),
           dealii::IteratorFilters::ActiveFEIndexEqualTo(0, true)))
  {
    typename dealii::Triangulation<dim>::active_cell_iterator cell_tria(cell);
    auto mp_dof_index = get_dof_index(cell_tria);
    auto const &mf_cell_vector = _cell_it_to_mf_pos.at(cell);
    for (unsigned int q = 0; q < n_q_points; ++q)
    {
      mapping_host_view(cell_i, q) = mf_cell_vector[q];
    }
    mp_dof_host_view(cell_i) = mp_dof_index;
    ++cell_i;
  }

  MemoryBlock<unsigned int, dealii::MemorySpace::CUDA> mapping(mapping_host);
  MemoryBlockView<unsigned, dealii::MemorySpace::CUDA> mapping_view(mapping);
  MemoryBlockView<double, dealii::MemorySpace::CUDA> liquid_ratio_view(
      liquid_ratio);
  MemoryBlockView<double, dealii::MemorySpace::CUDA> powder_ratio_view(
      powder_ratio);
  MemoryBlock<double, dealii::MemorySpace::CUDA> mp_dof_block(
      mp_dof_host_block);
  MemoryBlockView<double, dealii::MemorySpace::CUDA> mp_dof_view(mp_dof_block);
  MemoryBlockView<double, dealii::MemorySpace::CUDA> state_view(_state);
  auto const powder_state = static_cast<unsigned int>(MaterialState::powder);
  auto const liquid_state = static_cast<unsigned int>(MaterialState::liquid);
  auto const solid_state = static_cast<unsigned int>(MaterialState::solid);
  for_each(MemorySpaceType{}, cell_i,
           [=] ADAMANTINE_HOST_DEV(int i) mutable
           {
             double liquid_ratio_sum = 0.;
             double powder_ratio_sum = 0.;
             for (unsigned int q = 0; q < n_q_points; ++q)
             {
               liquid_ratio_sum += liquid_ratio_view(mapping_view(i, q));
               powder_ratio_sum += powder_ratio_view(mapping_view(i, q));
             }
             state_view(liquid_state, mp_dof_view(i)) =
                 liquid_ratio_sum / n_q_points;
             state_view(powder_state, mp_dof_view(i)) =
                 powder_ratio_sum / n_q_points;
             state_view(solid_state, mp_dof_view(i)) =
                 std::max(1. - state_view(liquid_state, mp_dof_view(i)) -
                              state_view(powder_state, mp_dof_view(i)),
                          0.);
           });
}
#endif

template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::set_initial_state()
{
  // Set the material state to the one defined by the user_index
  std::vector<dealii::types::global_dof_index> mp_dofs;
  std::vector<unsigned int> user_indices;
  for (auto cell :
       dealii::filter_iterators(_mp_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    std::vector<dealii::types::global_dof_index> mp_dof(1);
    cell->get_dof_indices(mp_dof);
    mp_dofs.push_back(_dofs_map.at(mp_dof[0]));
    user_indices.push_back(cell->user_index());
  }

  MemoryBlock<dealii::types::global_dof_index, MemorySpaceType> mp_dofs_block(
      mp_dofs);
  MemoryBlockView<dealii::types::global_dof_index, MemorySpaceType>
      mp_dofs_view(mp_dofs_block);
  MemoryBlock<unsigned int, MemorySpaceType> user_indices_block(user_indices);
  MemoryBlockView<unsigned int, MemorySpaceType> user_indices_view(
      user_indices_block);

  _state.set_zero();
  MemoryBlockView<double, MemorySpaceType> state_view(_state);
  for_each(MemorySpaceType{}, user_indices.size(),
           [=] ADAMANTINE_HOST_DEV(int i) mutable
           { state_view(user_indices_view(i), mp_dofs_view(i)) = 1.; });
}

template <int dim, typename MemorySpaceType>
void MaterialProperty<dim, MemorySpaceType>::fill_properties(
    boost::property_tree::ptree const &database)
{
  // PropertyTreeInput materials.property_format
  std::string property_format = database.get<std::string>("property_format");

  _use_table = (property_format == "table");
  // PropertyTreeInput materials.n_materials
  unsigned int const n_materials = database.get<unsigned int>("n_materials");
  // Find all the material_ids being used.
  std::vector<dealii::types::material_id> material_ids;
  for (dealii::types::material_id id = 0;
       id < dealii::numbers::invalid_material_id; ++id)
  {
    if (database.count("material_" + std::to_string(id)) != 0)
      material_ids.push_back(id);
    if (material_ids.size() == n_materials)
      break;
  }

  // When using the polynomial format we allocate one contiguous block of
  // memory. Thus, the largest material_id should be as small as possible
  unsigned int const n_material_ids =
      *std::max_element(material_ids.begin(), material_ids.end()) + 1;
  _properties.reinit(n_material_ids, g_n_properties);
  MemoryBlock<double, dealii::MemorySpace::Host> properties_host(_properties);

  MemoryBlock<double, dealii::MemorySpace::Host> state_property_tables_host;
  MemoryBlock<double, dealii::MemorySpace::Host>
      state_property_polynomials_host;
  if (_use_table)
  {
    _state_property_tables.reinit(n_material_ids, g_n_material_states,
                                  g_n_thermal_state_properties, table_size, 2);
    state_property_tables_host.reinit(n_material_ids, g_n_material_states,
                                      g_n_thermal_state_properties, table_size,
                                      2);
    state_property_tables_host.set_zero();
    // Mechanical properties only exist for the solid state
    _mechanical_properties_tables_host.reinit(
        n_material_ids, g_n_mechanical_state_properties, table_size, 2);
    _mechanical_properties_tables_host.set_zero();
  }
  else
  {
    _state_property_polynomials.reinit(n_material_ids + 1, g_n_material_states,
                                       g_n_thermal_state_properties,
                                       polynomial_order + 1);
    state_property_polynomials_host.reinit(
        n_material_ids + 1, g_n_material_states, g_n_thermal_state_properties,
        polynomial_order + 1);
    state_property_polynomials_host.set_zero();
    // Mechanical properties only exist for the solid state
    _mechanical_properties_polynomials_host.reinit(
        n_material_ids + 1, g_n_mechanical_state_properties,
        polynomial_order + 1);
    _mechanical_properties_polynomials_host.set_zero();
  }

  MemoryBlockView<double, dealii::MemorySpace::Host> properties_host_view(
      properties_host);
  MemoryBlockView<double, dealii::MemorySpace::Host>
      state_property_tables_host_view(state_property_tables_host);
  MemoryBlockView<double, dealii::MemorySpace::Host>
      state_property_polynomials_host_view(state_property_polynomials_host);
  MemoryBlockView<double, dealii::MemorySpace::Host>
      mechanical_property_tables_host_view(_mechanical_properties_tables_host);
  MemoryBlockView<double, dealii::MemorySpace::Host>
      mechanical_property_polynomials_host_view(
          _mechanical_properties_polynomials_host);
  for (auto const material_id : material_ids)
  {
    // Get the material property tree.
    boost::property_tree::ptree const &material_database =
        database.get_child("material_" + std::to_string(material_id));
    // For each material, loop over the possible states.
    for (unsigned int state = 0; state < g_n_material_states; ++state)
    {
      // The state may or may not exist for the material.
      boost::optional<boost::property_tree::ptree const &> state_database =
          material_database.get_child_optional(material_state_names[state]);
      if (state_database)
      {
        // For each state, loop over the possible properties.
        for (unsigned int p = 0; p < g_n_state_properties; ++p)
        {
          // The property may or may not exist for that state
          boost::optional<std::string> const property =
              state_database.get().get_optional<std::string>(
                  state_property_names[p]);
          // If the property exists, put it in the map. If the property does not
          // exist, we have a nullptr.
          if (property)
          {
            // Remove blank spaces
            std::string property_string = property.get();
            property_string.erase(
                std::remove_if(property_string.begin(), property_string.end(),
                               [](unsigned char x) { return std::isspace(x); }),
                property_string.end());
            if (_use_table)
            {
              std::vector<std::string> parsed_property;
              boost::split(parsed_property, property_string,
                           [](char c) { return c == ';'; });
              unsigned int const parsed_property_size = parsed_property.size();
              ASSERT_THROW(parsed_property_size <= table_size,
                           "Too many coefficients, increase the table size");
              for (unsigned int i = 0; i < parsed_property_size; ++i)
              {
                std::vector<std::string> t_v;
                boost::split(t_v, parsed_property[i],
                             [](char c) { return c == ','; });
                ASSERT(t_v.size() == 2, "Error reading material property.");
                if (p < g_n_thermal_state_properties)
                {
                  state_property_tables_host_view(material_id, state, p, i, 0) =
                      std::stod(t_v[0]);
                  state_property_tables_host_view(material_id, state, p, i, 1) =
                      std::stod(t_v[1]);
                }
                else
                {
                  if (state == static_cast<unsigned int>(MaterialState::solid))
                  {
                    mechanical_property_tables_host_view(
                        material_id, p - g_n_thermal_state_properties, i, 0) =
                        std::stod(t_v[0]);
                    mechanical_property_tables_host_view(
                        material_id, p - g_n_thermal_state_properties, i, 1) =
                        std::stod(t_v[1]);
                  }
                }
              }
              // fill the rest  with the last value
              for (unsigned int i = parsed_property_size; i < table_size; ++i)
              {
                if (p < g_n_thermal_state_properties)
                {
                  state_property_tables_host_view(material_id, state, p, i, 0) =
                      state_property_tables_host_view(material_id, state, p,
                                                      i - 1, 0);
                  state_property_tables_host_view(material_id, state, p, i, 1) =
                      state_property_tables_host_view(material_id, state, p,
                                                      i - 1, 1);
                }
                else
                {
                  if (state == static_cast<unsigned int>(MaterialState::solid))
                  {
                    mechanical_property_tables_host_view(
                        material_id, p - g_n_thermal_state_properties, i, 0) =
                        mechanical_property_tables_host_view(
                            material_id, p - g_n_thermal_state_properties,
                            i - 1, 0);
                    mechanical_property_tables_host_view(
                        material_id, p - g_n_thermal_state_properties, i, 1) =
                        mechanical_property_tables_host_view(
                            material_id, p - g_n_thermal_state_properties,
                            i - 1, 1);
                  }
                }
              }
            }
            else
            {
              std::vector<std::string> parsed_property;
              boost::split(parsed_property, property_string,
                           [](char c) { return c == ','; });
              unsigned int const parsed_property_size = parsed_property.size();
              ASSERT_THROW(
                  parsed_property_size <= polynomial_order,
                  "Too many coefficients, increase the polynomial order");
              for (unsigned int i = 0; i < parsed_property_size; ++i)
              {
                if (p < g_n_thermal_state_properties)
                {
                  state_property_polynomials_host_view(
                      material_id, state, p, i) = std::stod(parsed_property[i]);
                }
                else
                {
                  mechanical_property_polynomials_host_view(
                      material_id, p - g_n_thermal_state_properties, i) =
                      std::stod(parsed_property[i]);
                }
              }
            }
          }
        }
      }
    }

    // Check for the properties that are associated to a material but that
    // are independent of an individual state. These properties are duplicated
    // for every state.
    for (unsigned int p = 0; p < g_n_properties; ++p)
    {
      // The property may or may not exist for that state
      boost::optional<double> const property =
          material_database.get_optional<double>(property_names[p]);
      // If the property exists, put it in the map. If the property does not
      // exist, we use the largest possible value. This is useful if the
      // liquidus and the solidus are not set.
      properties_host_view(material_id, p) =
          property ? property.get() : std::numeric_limits<double>::max();
    }
  }

  // FIXME for now we assume that the mechanical properties are independent of
  // the temperature.
  _mechanical_properties_host.reinit(n_material_ids,
                                     g_n_mechanical_state_properties);
  if (_use_table)
  {
    // We only read the first element
    MemoryBlockView<double, dealii::MemorySpace::Host>
        mechanical_properties_host_view(_mechanical_properties_host);
    MemoryBlockView<double, dealii::MemorySpace::Host>
        mechanical_properties_tables_host_view(
            _mechanical_properties_tables_host);
    for (unsigned int i = 0; i < n_material_ids; ++i)
    {
      for (unsigned int j = 0; j < g_n_mechanical_state_properties; ++j)
      {
        mechanical_properties_host_view(i, j) =
            mechanical_properties_tables_host_view(i, j, 0, 1);
      }
    }
  }
  else
  {
    // We only read the first element
    MemoryBlockView<double, dealii::MemorySpace::Host>
        mechanical_properties_host_view(_mechanical_properties_host);
    MemoryBlockView<double, dealii::MemorySpace::Host>
        mechanical_properties_polynomials_host_view(
            _mechanical_properties_polynomials_host);
    for (unsigned int i = 0; i < n_material_ids; ++i)
    {
      for (unsigned int j = 0; j < g_n_mechanical_state_properties; ++j)
      {
        mechanical_properties_host_view(i, j) =
            mechanical_properties_polynomials_host_view(i, j, 0);
      }
    }
  }

  // Copy the data
  deep_copy(_state_property_polynomials, state_property_polynomials_host);
  deep_copy(_state_property_tables, state_property_tables_host);
  deep_copy(_properties, properties_host);
  _properties_view.reinit(_properties);
}

// We need to compute the average temperature on the cell because we need the
// material properties to be uniform over the cell. If there aren't then we have
// problems with the weak form discretization.
template <int dim, typename MemorySpaceType>
dealii::LA::distributed::Vector<double, MemorySpaceType>
MaterialProperty<dim, MemorySpaceType>::compute_average_temperature(
    dealii::DoFHandler<dim> const &temperature_dof_handler,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &temperature)
    const
{
  // TODO: this should probably done in a matrix-free fashion.
  // The triangulation is the same for both DoFHandler
  dealii::LA::distributed::Vector<double, MemorySpaceType> temperature_average(
      _mp_dof_handler.locally_owned_dofs(), temperature.get_mpi_communicator());
  temperature.update_ghost_values();
  temperature_average = 0.;
  dealii::hp::FECollection<dim> const &fe_collection =
      temperature_dof_handler.get_fe_collection();
  dealii::hp::QCollection<dim> q_collection;
  q_collection.push_back(dealii::QGauss<dim>(fe_collection.max_degree() + 1));
  q_collection.push_back(dealii::QGauss<dim>(1));
  dealii::hp::FEValues<dim> hp_fe_values(
      fe_collection, q_collection,
      dealii::UpdateFlags::update_values |
          dealii::UpdateFlags::update_quadrature_points |
          dealii::UpdateFlags::update_JxW_values);
  unsigned int const n_q_points = q_collection.max_n_quadrature_points();
  unsigned int const dofs_per_cell = fe_collection.max_dofs_per_cell();
  internal::compute_average(n_q_points, dofs_per_cell, _mp_dof_handler,
                            temperature_dof_handler, hp_fe_values, temperature,
                            temperature_average);

  return temperature_average;
}

template <int dim, typename MemorySpaceType>
ADAMANTINE_HOST_DEV double
MaterialProperty<dim, MemorySpaceType>::compute_property_from_table(
    MemoryBlockView<double, MemorySpaceType> const &state_property_tables_view,
    unsigned int const material_id, unsigned int const material_state,
    unsigned int const property, double const temperature)
{
  if (temperature <=
      state_property_tables_view(material_id, material_state, property, 0, 0))
  {
    return state_property_tables_view(material_id, material_state, property, 0,
                                      1);
  }
  else
  {
    unsigned int i = 0;
    unsigned int const size = state_property_tables_view.extent(3);
    for (; i < size; ++i)
    {
      if (temperature < state_property_tables_view(material_id, material_state,
                                                   property, i, 0))
      {
        break;
      }
    }

    if (i >= size - 1)
    {
      return state_property_tables_view(material_id, material_state, property,
                                        size - 1, 1);
    }
    else
    {
      auto tempertature_i = state_property_tables_view(
          material_id, material_state, property, i, 0);
      auto tempertature_im1 = state_property_tables_view(
          material_id, material_state, property, i - 1, 0);
      auto property_i = state_property_tables_view(material_id, material_state,
                                                   property, i, 1);
      auto property_im1 = state_property_tables_view(
          material_id, material_state, property, i - 1, 1);
      return property_im1 + (temperature - tempertature_im1) *
                                (property_i - property_im1) /
                                (tempertature_i - tempertature_im1);
    }
  }
}

} // namespace adamantine
