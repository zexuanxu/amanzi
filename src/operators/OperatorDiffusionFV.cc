/*
  This is the operators component of the Amanzi code. 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Authors: Daniil Svyatskiy (dasvyat@lanl.gov)
           Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <vector>
#include <boost/math/tools/roots.hpp>

#include "OperatorDefs.hh"
#include "OperatorDiffusionFV.hh"
#include "FluxTPFABCfunc.hh"
#include "Op.hh"
#include "Op_Face_Cell.hh"
#include "Operator_Cell.hh"

namespace Amanzi {
namespace Operators {

#define DEBUG_FLAG 0

/* ******************************************************************
* Initialization
****************************************************************** */
void
OperatorDiffusionFV::InitDiffusion_(Teuchos::ParameterList& plist)
{
  // Define stencil for the FV diffusion method.
  local_op_schema_ = OPERATOR_SCHEMA_BASE_FACE | OPERATOR_SCHEMA_DOFS_CELL;

  // create or check the existing Operator
  if (global_op_ == Teuchos::null) {
    // constructor was given a mesh
    global_op_schema_ = OPERATOR_SCHEMA_DOFS_CELL;

    // build the CVS from the global schema
    Teuchos::RCP<CompositeVectorSpace> cvs = Teuchos::rcp(new CompositeVectorSpace());
    cvs->SetMesh(mesh_)->SetGhosted(true);
    cvs->AddComponent("cell", AmanziMesh::CELL, 1);

    global_op_ = Teuchos::rcp(new Operator_Cell(cvs, plist, global_op_schema_));

  } else {
    // constructor was given an Operator
    global_op_schema_ = global_op_->schema();
    mesh_ = global_op_->DomainMap().Mesh();
  }

  // create the local Op and register it with the global Operator
  std::string name = "Diffusion: FACE_CELL";
  local_op_ = Teuchos::rcp(new Op_Face_Cell(name, mesh_));
  global_op_->OpPushBack(local_op_);
  
  // // scaled constraint -- enables zero rel perm
  // scaled_constraint_ = plist.get<bool>("scaled constraint equation", false);

  // upwind options
  std::string uwname = plist.get<std::string>("upwind method", "none");
  if (uwname == "standard") {
    upwind_ = OPERATOR_UPWIND_FLUX;
  } else if (uwname == "artificial diffusion") {  
    upwind_ = OPERATOR_UPWIND_AMANZI_ARTIFICIAL_DIFFUSION;
  } else if (uwname == "divk") {  
    upwind_ = OPERATOR_UPWIND_AMANZI_DIVK;
  } else if (uwname == "second-order") {  
    upwind_ = OPERATOR_UPWIND_AMANZI_SECOND_ORDER;
  } else if (uwname == "none") {
    upwind_ = OPERATOR_UPWIND_NONE;  // cell-centered scheme.
  } else {
    ASSERT(false);
  }

  // Do we need to exclude the primary terms?
  exclude_primary_terms_ = plist.get<bool>("exclude primary terms", false);
  
  // Do we need to calculate Newton correction terms?
  newton_correction_ = OPERATOR_DIFFUSION_JACOBIAN_NONE;
  std::string jacobian = plist.get<std::string>("newton correction", "none");
  if (jacobian == "true jacobian") {
    newton_correction_ = OPERATOR_DIFFUSION_JACOBIAN_TRUE;
  } else if (jacobian == "approximate jacobian") {
    newton_correction_ = OPERATOR_DIFFUSION_JACOBIAN_APPROXIMATE;
  }
  if (newton_correction_ != OPERATOR_DIFFUSION_JACOBIAN_NONE) {
    std::string name = "Diffusion: FACE_CELL Jacobian terms";
    jac_op_ = Teuchos::rcp(new Op_Face_Cell(name, mesh_));
    global_op_->OpPushBack(jac_op_);
  }  

  // mesh info
  ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  nnodes_owned = mesh_->num_entities(AmanziMesh::NODE, AmanziMesh::OWNED);

  ncells_wghost = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::USED);
  nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);
  nnodes_wghost = mesh_->num_entities(AmanziMesh::NODE, AmanziMesh::USED);

  // solution-independent data
  CompositeVectorSpace cvs;
  cvs.SetMesh(mesh_);
  cvs.SetGhosted(true);
  cvs.SetComponent("face", AmanziMesh::FACE, 1);
  transmissibility_ = Teuchos::rcp(new CompositeVector(cvs, true));

  g_.set(mesh_->space_dimension(), 0.0);
}


/* ******************************************************************
* Setup methods: scalar coefficients
****************************************************************** */
void
OperatorDiffusionFV::Setup(const Teuchos::RCP<std::vector<WhetStone::Tensor> >& K,
                           double rho, double mu)
{
  K_ = K;
  rho_ = rho;
  mu_ = mu;
  scalar_rho_mu_ = true;

  if (gravity_ && !gravity_term_.get())
      gravity_term_ = Teuchos::rcp(new CompositeVector(*transmissibility_));
  
  ComputeTransmissibility_();
}


/* ******************************************************************
* Setup methods: vector coefficients
****************************************************************** */
void
OperatorDiffusionFV::Setup(const Teuchos::RCP<std::vector<WhetStone::Tensor> >& K,
                           const Teuchos::RCP<const CompositeVector>& rho,
                           const Teuchos::RCP<const CompositeVector>& mu)
{
  K_ = K;
  rho_cv_ = rho;
  mu_cv_ = mu;
  scalar_rho_mu_ = false;

  if (gravity_ && !gravity_term_.get())
      gravity_term_ = Teuchos::rcp(new CompositeVector(*transmissibility_));

  ComputeTransmissibility_();
}


/* ******************************************************************
* Setup methods: krel and deriv -- must be called after calling a
* setup with K abs
****************************************************************** */
void
OperatorDiffusionFV::Setup(const Teuchos::RCP<const CompositeVector>& k,
                           const Teuchos::RCP<const CompositeVector>& dkdp)
{
  k_ = k;
  dkdp_ = dkdp;
  if (k_ != Teuchos::null) {
    ASSERT(k_->HasComponent("face"));
    // NOTE: it seems that Amanzi passes in a cell based kr which is then
    // ignored, and assumed = 1.  This seems dangerous to me. --etc
    //    ASSERT(!k_->HasComponent("cell"));
  }
  if (dkdp_ != Teuchos::null) {
    ASSERT(dkdp_->HasComponent("cell"));
    ASSERT(dkdp_->HasComponent("face"));
  }

}


/* ******************************************************************
* Populate face-based matrices.
****************************************************************** */
void
OperatorDiffusionFV::UpdateMatrices(const Teuchos::Ptr<const CompositeVector>& flux,
        const Teuchos::Ptr<const CompositeVector>& u)
{
  if (!exclude_primary_terms_) {
    const Epetra_MultiVector& trans_face = *transmissibility_->ViewComponent("face", true);
    const std::vector<int>& bc_model = bc_->bc_model();
    WhetStone::DenseMatrix null_matrix;

    // preparing upwind data
    Teuchos::RCP<const Epetra_MultiVector> k_face = Teuchos::null;
    if (k_ != Teuchos::null) {
      k_face = k_->ViewComponent("face", true);
    }

    // updating matrix blocks
    AmanziMesh::Entity_ID_List cells, faces;
    std::vector<int> dirs;

    for (int f = 0; f != nfaces_owned; ++f) {
      mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
      int ncells = cells.size();

      WhetStone::DenseMatrix Aface(ncells, ncells);
      Aface = 0.;

      if (bc_model[f] != OPERATOR_BC_NEUMANN){
        double tij = trans_face[0][f] * (k_face.get() ? (*k_face)[0][f] : 1.);
        for (int i = 0; i != ncells; ++i) {
          Aface(i, i) = tij;
          for (int j = i + 1; j != ncells; ++j) {
            Aface(i, j) = -tij;
            Aface(j, i) = -tij;
          }
        }
      }

      local_op_->matrices[f] = Aface;
    }

    // populating right-hand side
    if (gravity_) {
      const Epetra_MultiVector& gravity_face = *gravity_term_->ViewComponent("face", true);
      Epetra_MultiVector& rhs_cell = *global_op_->rhs()->ViewComponent("cell");

      for (int c = 0; c != ncells_owned; ++c) {
        mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
        int nfaces = faces.size();

        for (int n = 0; n != nfaces; ++n) {
          int f = faces[n];
          if (bc_model[f] == OPERATOR_BC_NEUMANN) continue;
          rhs_cell[0][c] -= dirs[n] * gravity_face[0][f] * (k_face.get() ? (*k_face)[0][f] : 1.0);
        }
      }
    }
  }

  // Add derivatives to the matrix (Jacobian in this case)
  if (newton_correction_ == OPERATOR_DIFFUSION_JACOBIAN_TRUE && u.get()) {
    AnalyticJacobian_(*u);
  }
}


/* ******************************************************************
* Special implementation of boundary conditions.
**********************************;******************************** */
void OperatorDiffusionFV::ApplyBCs(bool primary)
{
  const Epetra_MultiVector& trans_face = *transmissibility_->ViewComponent("face", true);
  Epetra_MultiVector* gravity_face;
  if (gravity_) 
      gravity_face = &*gravity_term_->ViewComponent("face", true);

  const std::vector<int>& bc_model = bc_->bc_model();
  const std::vector<double>& bc_value = bc_->bc_value();

  Teuchos::RCP<const Epetra_MultiVector> k_face = Teuchos::null;
  if (k_ != Teuchos::null) {
    k_face = k_->ViewComponent("face", true);
  }
  Epetra_MultiVector& rhs_cell = *global_op_->rhs()->ViewComponent("cell");

  AmanziMesh::Entity_ID_List cells;

  for (int f = 0; f < nfaces_wghost; f++) {
    if (bc_model[f] != OPERATOR_BC_NONE) {
      mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
      int c = cells[0];

      if (bc_model[f] == OPERATOR_BC_DIRICHLET) {
        rhs_cell[0][c] += bc_value[f] * trans_face[0][f] * (k_face.get() ? (*k_face)[0][f] : 1.);
      } else if (bc_model[f] == OPERATOR_BC_NEUMANN) {
        rhs_cell[0][c] -= bc_value[f] * mesh_->face_area(f);
        // trans_face[0][f] = 0.0;
        if (gravity_) (*gravity_face)[0][f] = 0.0;
      }
    }
  }
}


/* ******************************************************************
* Calculate flux from cell-centered data
****************************************************************** */
void OperatorDiffusionFV::UpdateFlux(
    const CompositeVector& solution, CompositeVector& darcy_mass_flux)
{
  const Epetra_MultiVector& trans_face = *transmissibility_->ViewComponent("face", true);
  const Epetra_MultiVector& gravity_face = *gravity_term_->ViewComponent("face", true);

  const std::vector<int>& bc_model = bc_->bc_model();
  const std::vector<double>& bc_value = bc_->bc_value();

  solution.ScatterMasterToGhosted("cell");

  const Epetra_MultiVector& Krel_face = *k_->ViewComponent("face", true);
  const Epetra_MultiVector& p = *solution.ViewComponent("cell", true);
  Epetra_MultiVector& flux = *darcy_mass_flux.ViewComponent("face", true);

  AmanziMesh::Entity_ID_List cells, faces;
  std::vector<int> dirs;

  std::vector<int> flag(nfaces_wghost, 0);

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];

      if (bc_model[f] == OPERATOR_BC_DIRICHLET) {
	double value = bc_value[f];
	flux[0][f] = dirs[n] * trans_face[0][f] * (p[0][c] - value) + gravity_face[0][f];
	flux[0][f] *= Krel_face[0][f];

      } else if (bc_model[f] == OPERATOR_BC_NEUMANN) {
	double value = bc_value[f];
	double area = mesh_->face_area(f);
	flux[0][f] = value*area;

      } else {
	if (f < nfaces_owned && !flag[f]) {
	  mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
	  if (cells.size() <= 1) {
	    Errors::Message msg("Flow PK: These boundary conditions are not supported by FV.");
	    Exceptions::amanzi_throw(msg);
	  }
          int c1 = cells[0];
          int c2 = cells[1];
          if (c == c1) {
            flux[0][f] = dirs[n] * trans_face[0][f] * (p[0][c1] - p[0][c2]) + gravity_face[0][f];
          } else {
            flux[0][f] = dirs[n] * trans_face[0][f] * (p[0][c2] - p[0][c1]) + gravity_face[0][f];
          }	    
          flux[0][f] *= Krel_face[0][f];
          flag[f] = 1;
	}
      }
    }
  }
}



/* ******************************************************************
* Computation the part of the Jacobian which depends on derivatives 
* of the relative permeability wrt to capillary pressure. They must
* be added to the existing matrix structure.
****************************************************************** */
void OperatorDiffusionFV::AnalyticJacobian_(const CompositeVector& u)
{
  const std::vector<int>& bc_model = bc_->bc_model();
  const std::vector<double>& bc_value = bc_->bc_value();

  u.ScatterMasterToGhosted("cell");
  const Epetra_MultiVector& uc = *u.ViewComponent("cell", true);

  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  AmanziMesh::Entity_ID_List cells, faces;
  std::vector<int> dirs;

  double k_rel[2], dkdp[2], pres[2], dist;

  const Epetra_MultiVector& dKdP_cell = *dkdp_->ViewComponent("cell");
  const Epetra_MultiVector& dKdP_face = *dkdp_->ViewComponent("face", true);

  std::vector<int> flag(nfaces_owned, 0);
  for (int c = 0; c != ncells_owned; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int i = 0; i != nfaces; ++i) {
      int f = faces[i];
      if (f < nfaces_owned && !flag[f]) {
        mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
        int mcells = cells.size();

        WhetStone::DenseMatrix Aface(mcells, mcells);

        int face_dir;   	
        // face_dir is equal to 1 if normal direction points from cells[0],
        // otherwise face_dir is -1
        if (cells[0] == c) face_dir = dirs[i];
        else face_dir = -dirs[i];

        for (int n = 0; n < mcells; n++) {
          int c1 = cells[n];
          pres[n] = uc[0][c1];
          dkdp[n] = dKdP_cell[0][c1];
        }

        if (mcells == 1) {
          dkdp[0] = dKdP_face[0][f];
        }

        // const AmanziGeometry::Point& normal = mesh_->face_normal(f, false, cells[0], &dir);
        ComputeJacobianLocal_(mcells, f, face_dir, upwind_,
                bc_model[f], bc_value[f], pres, dkdp, Aface);

        jac_op_->matrices[f] = Aface;
        flag[f] = 1;
      }
    }
  }
}


/* ******************************************************************
* Computation of a local submatrix of the analytical Jacobian 
* (its nonlinear part) on face f.
****************************************************************** */
void OperatorDiffusionFV::ComputeJacobianLocal_(
    int mcells, int f, int face_dir, int Krel_method,
    int bc_model_f, double bc_value_f,
    double *pres, double *dkdp_cell,
    WhetStone::DenseMatrix& Jpp)
{
  const Epetra_MultiVector& trans_face = *transmissibility_->ViewComponent("face", true);
  const Epetra_MultiVector& gravity_face = *gravity_term_->ViewComponent("face", true);

  double dKrel_dp[2];
  double dpres;

  if (mcells == 2) {
    dpres = pres[0] - pres[1];  // + grn;
    if (Krel_method == OPERATOR_UPWIND_CONSTANT_VECTOR) {  // Define K 
      double cos_angle = face_dir * gravity_face[0][f] / (rho_ * mesh_->face_area(f));
      if (cos_angle > OPERATOR_UPWIND_RELATIVE_TOLERANCE) {  // Upwind
        dKrel_dp[0] = dkdp_cell[0];
        dKrel_dp[1] = 0.0;
      } else if (cos_angle < -OPERATOR_UPWIND_RELATIVE_TOLERANCE) {  // Upwind
        dKrel_dp[0] = 0.0;
        dKrel_dp[1] = dkdp_cell[1];
      } else if (fabs(cos_angle) < OPERATOR_UPWIND_RELATIVE_TOLERANCE) {  // Upwind
        dKrel_dp[0] = 0.5 * dkdp_cell[0];
        dKrel_dp[1] = 0.5 * dkdp_cell[1];
      }
    } else if (Krel_method == OPERATOR_UPWIND_FLUX) {
      double flux0to1;
      flux0to1 = trans_face[0][f] * dpres + face_dir * gravity_face[0][f];
      if (flux0to1  > OPERATOR_UPWIND_RELATIVE_TOLERANCE) {  // Upwind
        dKrel_dp[0] = dkdp_cell[0];
        dKrel_dp[1] = 0.0;
      } else if (flux0to1 < -OPERATOR_UPWIND_RELATIVE_TOLERANCE) {  // Upwind
        dKrel_dp[0] = 0.0;
        dKrel_dp[1] = dkdp_cell[1];
      } else if (fabs(flux0to1) < OPERATOR_UPWIND_RELATIVE_TOLERANCE) {  // Upwind
        dKrel_dp[0] = 0.5 * dkdp_cell[0];
        dKrel_dp[1] = 0.5 * dkdp_cell[1];
      }
    } else if (Krel_method == OPERATOR_UPWIND_ARITHMETIC_AVERAGE) {
      dKrel_dp[0] = 0.5 * dkdp_cell[0];
      dKrel_dp[1] = 0.5 * dkdp_cell[1];
    } else {
      ASSERT(0);
    }

    Jpp(0, 0) = (trans_face[0][f] * dpres + face_dir * gravity_face[0][f]) * dKrel_dp[0];
    Jpp(0, 1) = (trans_face[0][f] * dpres + face_dir * gravity_face[0][f]) * dKrel_dp[1];
    Jpp(1, 0) = -Jpp(0, 0);
    Jpp(1, 1) = -Jpp(0, 1);

  } else if (mcells == 1) {
    if (bc_model_f == OPERATOR_BC_DIRICHLET) {                   
      pres[1] = bc_value_f;
      dpres = pres[0] - pres[1];  // + grn;
      Jpp(0, 0) = (trans_face[0][f] * dpres + face_dir * gravity_face[0][f]) * dkdp_cell[0];
      //Jpp(0, 0) = 0.0;
      //std::cout<<"Local J dkdp_cell[0] "<<f<<"  "<<dkdp_cell[0]<<" "<<bc_value_f<<" "<<pres[0]<<" "<<"\n";
    } else {
      Jpp(0, 0) = 0.0;
    }
  }
}


/* ******************************************************************
* Compute transmissibilities on faces 
****************************************************************** */
void OperatorDiffusionFV::ComputeTransmissibility_()
{
  const Epetra_MultiVector& trans_face = *transmissibility_->ViewComponent("face", true);

  transmissibility_->PutScalar(0.0);

  AmanziGeometry::Point gravity(g_ * rho_);

  AmanziMesh::Entity_ID_List cells;
  AmanziGeometry::Point a_dist, a[2];
  double h[2], perm[2], beta[2], trans_f;

  for (int f = 0; f < nfaces_owned; f++) {
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    int ncells = cells.size();

    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    double area = mesh_->face_area(f);

    if (ncells == 2) {
      a_dist = mesh_->cell_centroid(cells[1]) - mesh_->cell_centroid(cells[0]);
    } else if (ncells == 1) {    
      a_dist = xf - mesh_->cell_centroid(cells[0]);
    } 

    a_dist *= 1.0 / norm(a_dist);

    for (int i = 0; i < ncells; i++) {
      int c = cells[i];
      a[i] = xf - mesh_->cell_centroid(c);
      h[i] = norm(a[i]);
      double s = area / h[i];
      perm[i] = (rho_ / mu_) * (((*K_)[c] * a[i]) * normal) * s;

      double dxn = a[i] * normal;
      beta[i] = fabs(perm[i] / dxn);
    }

    // double grav = (gravity * normal) / area;
    double dir = copysign(1.0, normal * a_dist);
    double grav = (gravity * a_dist) * dir;
    trans_f = 0.0;

    if (ncells == 2) {
      grav *= (h[0] + h[1]);
      trans_f = (beta[0] * beta[1]) / (beta[0] + beta[1]);
    } else if (ncells == 1) {    
      grav *= h[0];
      trans_f = beta[0];
    } 

    trans_face[0][f] = trans_f;
    if (gravity_) {
      Epetra_MultiVector& gravity_face = *gravity_term_->ViewComponent("face", true);
      gravity_face[0][f] = trans_face[0][f] * grav;
    }
  }

#ifdef HAVE_MPI
  transmissibility_->ScatterMasterToGhosted("face", true);
  if (gravity_) {
    gravity_term_->ScatterMasterToGhosted("face", true);
  }
#endif
}

}  // namespace Operators
}  // namespace Amanzi


