/* Copyright (c) 2022 - 2023, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#include <MechanicalPhysics.hh>
#include <instantiation.hh>

#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_nothing.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/trilinos_precondition.h>
#include <deal.II/numerics/vector_tools.h>

namespace adamantine
{
template <int dim, typename MemorySpaceType>
MechanicalPhysics<dim, MemorySpaceType>::MechanicalPhysics(
    MPI_Comm const &communicator, unsigned int fe_degree,
    Geometry<dim> &geometry,
    MaterialProperty<dim, MemorySpaceType> &material_properties,
    std::vector<double> reference_temperatures)
    : _geometry(geometry), _material_properties(material_properties),
      _dof_handler(_geometry.get_triangulation())
{
  // Create the FECollection
  _fe_collection.push_back(
      dealii::FESystem<dim>(dealii::FE_Q<dim>(fe_degree) ^ dim));
  _fe_collection.push_back(
      dealii::FESystem<dim>(dealii::FE_Nothing<dim>() ^ dim));

  // Create the QCollection
  _q_collection.push_back(dealii::QGauss<dim>(fe_degree + 1));
  _q_collection.push_back(dealii::QGauss<dim>(1));

  // Solve the mechanical problem only on the part of the domain that has solid
  // material.
  for (auto const &cell :
       dealii::filter_iterators(_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    if (_material_properties.get_state_ratio(cell, MaterialState::solid) > 0.99)
    {
      cell->set_active_fe_index(0);
    }
    else
    {
      cell->set_active_fe_index(1);
    }
  }

  // Create the mechanical operator
  _mechanical_operator =
      std::make_unique<MechanicalOperator<dim, MemorySpaceType>>(
          communicator, _material_properties, reference_temperatures);
}

template <int dim, typename MemorySpaceType>
void MechanicalPhysics<dim, MemorySpaceType>::setup_dofs(
    std::vector<std::shared_ptr<BodyForce<dim>>> const &body_forces)
{
  _dof_handler.distribute_dofs(_fe_collection);
  dealii::IndexSet locally_relevant_dofs;
  dealii::DoFTools::extract_locally_relevant_dofs(_dof_handler,
                                                  locally_relevant_dofs);
  _affine_constraints.clear();
  _affine_constraints.reinit(locally_relevant_dofs);
  dealii::DoFTools::make_hanging_node_constraints(_dof_handler,
                                                  _affine_constraints);
  // TODO For now only Dirichlet boundary condition
  // FIXME For now this is only a Dirichlet boundary condition. It is also
  // manually set to be what is the bottom face for a dealii hyper-rectangle. We
  // need to decide how we want to expose BC control to the user more generally
  // (including for user-supplied meshes).
  dealii::VectorTools::interpolate_boundary_values(
      _dof_handler, 4, dealii::Functions::ZeroFunction<dim>(dim),
      _affine_constraints);
  _affine_constraints.close();

  _mechanical_operator->reinit(_dof_handler, _affine_constraints, _q_collection,
                               body_forces);
}

template <int dim, typename MemorySpaceType>
void MechanicalPhysics<dim, MemorySpaceType>::setup_dofs(
    dealii::DoFHandler<dim> const &thermal_dof_handler,
    dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host> const
        &temperature,
    std::vector<bool> const &has_melted,
    std::vector<std::shared_ptr<BodyForce<dim>>> const &body_forces)
{
  _mechanical_operator->update_temperature(thermal_dof_handler, temperature,
                                           has_melted);
  // Update the active fe indices
  for (auto const &cell :
       dealii::filter_iterators(_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    if (_material_properties.get_state_ratio(cell, MaterialState::solid) > 0.99)
    {
      // Only enable the cell if it is also enabled for the thermal simulation
      // Get the thermal DoFHandler cell iterator
      dealii::DoFCellAccessor<dim, dim, false> thermal_cell(
          &(_dof_handler.get_triangulation()), cell->level(), cell->index(),
          &thermal_dof_handler);
      cell->set_active_fe_index(thermal_cell.active_fe_index());
    }
    else
    {
      cell->set_active_fe_index(1);
    }
  }
  setup_dofs(body_forces);
}

template <int dim, typename MemorySpaceType>
dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host>
MechanicalPhysics<dim, MemorySpaceType>::solve()
{
  dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host> solution(
      _mechanical_operator->rhs().get_partitioner());

  unsigned int const max_iter = _dof_handler.n_dofs() / 10;
  double const tol = 1e-12 * _mechanical_operator->rhs().l2_norm();
  dealii::SolverControl solver_control(max_iter, tol);
  dealii::SolverCG<
      dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host>>
      cg(solver_control);
  // TODO Use better preconditioner
  dealii::TrilinosWrappers::PreconditionSSOR preconditioner;
  preconditioner.initialize(_mechanical_operator->system_matrix());
  cg.solve(_mechanical_operator->system_matrix(), solution,
           _mechanical_operator->rhs(), preconditioner);
  _affine_constraints.distribute(solution);

  return solution;
}

} // namespace adamantine

INSTANTIATE_DIM_HOST(MechanicalPhysics)
#ifdef ADAMANTINE_HAVE_CUDA
INSTANTIATE_DIM_DEVICE(MechanicalPhysics)
#endif
