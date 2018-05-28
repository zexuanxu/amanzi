/*
  Operators

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <vector>

// Amanzi
#include "CoordinateSystems.hh"
#include "MFD3DFactory.hh"
#include "NumericalIntegration.hh"
#include "Polynomial.hh"

// Operators
#include "Op.hh"
#include "Op_Cell_Schema.hh"
#include "Op_Face_Schema.hh"
#include "OperatorDefs.hh"
#include "Operator_Schema.hh"
#include "PDE_DiffusionDG.hh"

namespace Amanzi {
namespace Operators {

/* ******************************************************************
* Initialization
****************************************************************** */
void PDE_DiffusionDG::Init_(Teuchos::ParameterList& plist)
{
  // create the local Op and register it with the global Operator
  Schema my_schema;
  Teuchos::ParameterList& schema_list = plist.sublist("schema");
  my_schema.Init(schema_list, mesh_);

  local_schema_col_ = my_schema;
  local_schema_row_ = my_schema;

  // create or check the existing Operator
  if (global_op_ == Teuchos::null) {
    global_schema_col_ = my_schema;
    global_schema_row_ = my_schema;

    // build the CVS from the global schema
    int nk = my_schema.begin()->num;
    Teuchos::RCP<CompositeVectorSpace> cvs = Teuchos::rcp(new CompositeVectorSpace());
    cvs->SetMesh(mesh_)->SetGhosted(true);
    cvs->AddComponent("cell", AmanziMesh::CELL, nk);

    global_op_ = Teuchos::rcp(new Operator_Schema(cvs, plist, my_schema));

  } else {
    // constructor was given an Operator
    global_schema_col_ = global_op_->schema_col();
    global_schema_row_ = global_op_->schema_row();
    mesh_ = global_op_->DomainMap().Mesh();
  }

  // create local operators
  // -- stiffness
  local_op_ = Teuchos::rcp(new Op_Cell_Schema(my_schema, my_schema, mesh_));
  global_op_->OpPushBack(local_op_);

  // -- continuity terms
  Schema tmp_schema;
  Teuchos::ParameterList schema_copy = schema_list;
  schema_copy.set<std::string>("base", "face");
  tmp_schema.Init(schema_copy, mesh_);

  jump_up_op_ = Teuchos::rcp(new Op_Face_Schema(tmp_schema, tmp_schema, mesh_));
  global_op_->OpPushBack(jump_up_op_);

  jump_pu_op_ = Teuchos::rcp(new Op_Face_Schema(tmp_schema, tmp_schema, mesh_));
  global_op_->OpPushBack(jump_pu_op_);

  // -- stability jump term
  penalty_op_ = Teuchos::rcp(new Op_Face_Schema(tmp_schema, tmp_schema, mesh_));
  global_op_->OpPushBack(penalty_op_);

  // parameters
  // -- discretization details
  method_ = plist.get<std::string>("method");
  method_order_ = plist.get<int>("method order", 0);
  matrix_ = plist.get<std::string>("matrix type");
}


/* ******************************************************************
* Setup methods: scalar coefficients
****************************************************************** */
void PDE_DiffusionDG::SetProblemCoefficients(
   const std::shared_ptr<std::vector<WhetStone::Tensor> >& Kc,
   const std::shared_ptr<std::vector<double> >& Kf) 
{
  Kc_ = Kc;
  Kf_ = Kf;
}


/* ******************************************************************
* Populate face-based 2x2 matrices on interior faces and 1x1 matrices
* on boundary faces.
****************************************************************** */
void PDE_DiffusionDG::UpdateMatrices(const Teuchos::Ptr<const CompositeVector>& flux,
                                     const Teuchos::Ptr<const CompositeVector>& u)
{
  WhetStone::DG_Modal dg(method_order_, mesh_, "natural");
  WhetStone::DenseMatrix Acell, Aface;

  int d = mesh_->space_dimension();
  double Kf(1.0);
  AmanziMesh::Entity_ID_List cells;
  
  WhetStone::Tensor Kc1(d, 1), Kc2(d, 1);
  Kc1(0, 0) = 1.0;
  Kc2(0, 0) = 1.0;

  for (int c = 0; c != ncells_owned; ++c) {
    if (Kc_.get()) Kc1 = (*Kc_)[c];
    dg.StiffnessMatrix(c, Kc1, Acell);
    local_op_->matrices[c] = Acell;
  }

  for (int f = 0; f != nfaces_owned; ++f) {
    if (Kf_.get()) Kf = (*Kf_)[f];
    dg.FaceMatrixPenalty(f, Kf, Aface);
    penalty_op_->matrices[f] = Aface;
  }

  for (int f = 0; f != nfaces_owned; ++f) {
    mesh_->face_get_cells(f, AmanziMesh::Parallel_type::ALL, &cells);
    if (Kc_.get()) {
      Kc1 = (*Kc_)[cells[0]]; 
      if (cells.size() > 1) Kc2 = (*Kc_)[cells[1]]; 
    }
    dg.FaceMatrixJump(f, Kc1, Kc2, Aface);
    Aface *= -1.0;
    jump_up_op_->matrices[f] = Aface;

    Aface.Transpose();
    jump_pu_op_->matrices[f] = Aface;
  }
}


/* ******************************************************************
* Apply boundary conditions to the local matrices.
****************************************************************** */
void PDE_DiffusionDG::ApplyBCs(bool primary, bool eliminate, bool leading_op)
{
  const std::vector<int>& bc_model = bcs_trial_[0]->bc_model();
  const std::vector<std::vector<double> >& bc_value = bcs_trial_[0]->bc_value_vector();
  int nk = bc_value[0].size();

  Epetra_MultiVector& rhs_c = *global_op_->rhs()->ViewComponent("cell", true);

  AmanziMesh::Entity_ID_List cells;

  int dir, d = mesh_->space_dimension();
  std::vector<AmanziGeometry::Point> tau(d - 1);

  // create integration object for all mesh cells
  WhetStone::NumericalIntegration numi(mesh_, false);

  for (int f = 0; f != nfaces_owned; ++f) {
    if (bc_model[f] == OPERATOR_BC_DIRICHLET ||
        bc_model[f] == OPERATOR_BC_NEUMANN) {
      mesh_->face_get_cells(f, AmanziMesh::Parallel_type::ALL, &cells);
      int c = cells[0];

      const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
      const AmanziGeometry::Point& normal = mesh_->face_normal(f, false, c, &dir);

      // set polynomial with Dirichlet data
      WhetStone::DenseVector coef(nk);
      for (int i = 0; i < nk; ++i) {
        coef(i) = bc_value[f][i];
      }

      WhetStone::Polynomial pf(d, method_order_); 
      pf.SetPolynomialCoefficients(coef);
      pf.set_origin(xf);

      // convert boundary polynomial to space polynomial
      pf.ChangeOrigin(mesh_->cell_centroid(c));
      numi.ChangeBasisRegularToNatural(c, pf);

      // extract coefficients and update right-hand side 
      WhetStone::DenseMatrix& Pcell = penalty_op_->matrices[f];
      int nrows = Pcell.NumRows();
      int ncols = Pcell.NumCols();

      WhetStone::DenseVector v(nrows), pv(ncols), jv(ncols);
      pf.GetPolynomialCoefficients(v);

      if (bc_model[f] == OPERATOR_BC_DIRICHLET) {
        WhetStone::DenseMatrix& Jcell = jump_up_op_->matrices[f];
        Pcell.Multiply(v, pv, false);
        Jcell.Multiply(v, jv, false);

        for (int i = 0; i < ncols; ++i) {
          rhs_c[i][c] += pv(i) + jv(i);
        }
      } else if (bc_model[f] == OPERATOR_BC_NEUMANN) {
        WhetStone::DenseMatrix& Jcell = jump_pu_op_->matrices[f];
        Jcell.Multiply(v, jv, false);

        for (int i = 0; i < ncols; ++i) {
          rhs_c[i][c] -= jv(i);
        }

        Pcell.PutScalar(0.0);
        Jcell.PutScalar(0.0);
        jump_up_op_->matrices[f].PutScalar(0.0);
      }
    }
  } 
}


/* ******************************************************************
* Calculate mass flux from cell-centered data
****************************************************************** */
void PDE_DiffusionDG::UpdateFlux(const Teuchos::Ptr<const CompositeVector>& solution,
                                 const Teuchos::Ptr<CompositeVector>& flux)
{
}

}  // namespace Operators
}  // namespace Amanzi


