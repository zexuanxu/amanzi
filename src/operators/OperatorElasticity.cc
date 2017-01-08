/*
  Operators 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <vector>

// TPLs
#include "Epetra_Vector.h"

// Amanzi
#include "errors.hh"
#include "MatrixFE.hh"
#include "mfd3d_elasticity.hh"
#include "PreconditionerFactory.hh"
#include "SuperMap.hh"
#include "WhetStoneDefs.hh"

// Operators
#include "Op.hh"
#include "Op_Cell_Schema.hh"
#include "OperatorDefs.hh"
#include "Operator_Schema.hh"
#include "OperatorElasticity.hh"
#include "Schema.hh"

namespace Amanzi {
namespace Operators {

/* ******************************************************************
* Initialization of the operator, scalar coefficient.
****************************************************************** */
void OperatorElasticity::SetTensorCoefficient(
    const Teuchos::RCP<std::vector<WhetStone::Tensor> >& K)
{
  K_ = K;
}


/* ******************************************************************
* Calculate elemental matrices.
****************************************************************** */
void OperatorElasticity::UpdateMatrices()
{
  WhetStone::MFD3D_Elasticity mfd(mesh_);
  AmanziMesh::Entity_ID_List nodes;
  int d = mesh_->space_dimension();

  WhetStone::Tensor Kc(mesh_->space_dimension(), 1);
  Kc(0, 0) = 1.0;
  
  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_nodes(c, &nodes);
    int nnodes = nodes.size();
    int nfaces = mesh_->cell_get_num_faces(c);
    int ndofs = d * nnodes + nfaces;

    WhetStone::DenseMatrix Acell(ndofs, ndofs);
    if (K_.get()) Kc = (*K_)[c];
    mfd.StiffnessMatrixNode2Face1(c, Kc, Acell);

    local_op_->matrices[c] = Acell;
  }
}


/* ******************************************************************
* Apply boundary conditions to the local matrices. We always zero-out
* matrix rows for essential test BCs. As to trial BCs, there are
* options: (a) eliminate or not, (b) if eliminate, then put 1 on
* the diagonal or not.
****************************************************************** */
void OperatorElasticity::ApplyBCs(bool primary, bool eliminate)
{
  Teuchos::RCP<BCs> bc_f, bc_v;
  for (auto bc = bcs_trial_.begin(); bc != bcs_trial_.end(); ++bc) {
    if ((*bc)->type() == OPERATOR_BC_TYPE_FACE) {
      bc_f = *bc;
      ApplyBCs_Face_(bc_f.ptr(), primary, eliminate);
    } else if ((*bc)->type() == OPERATOR_BC_TYPE_NODE) {
      bc_v = *bc;
      ApplyBCs_Node_(bc_v.ptr(), primary, eliminate);
    }
  }
}


/* ******************************************************************
* Apply BCs on cell operators
****************************************************************** */
void OperatorElasticity::ApplyBCs_Face_(const Teuchos::Ptr<BCs>& bc_f,
                                        bool primary, bool eliminate)
{
  AmanziMesh::Entity_ID_List faces, cells;
  std::vector<int> dirs;

  global_op_->rhs()->PutScalarGhosted(0.0);
  Epetra_MultiVector& rhs_face = *global_op_->rhs()->ViewComponent("face", true);

  int nn(0), nm(0);
  for (int c = 0; c != ncells_owned; ++c) {
    bool flag(true);
    WhetStone::DenseMatrix& Acell = local_op_->matrices[c];

    if (bc_f != Teuchos::null) {
      const std::vector<int>& bc_model = bc_f->bc_model();
      const std::vector<double>& bc_value = bc_f->bc_value();

      mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
      int nfaces = faces.size();

      // essential conditions for test functions
      for (int n = 0; n != nfaces; ++n) {
        int f = faces[n];
        if (bc_model[f] == OPERATOR_BC_DIRICHLET) {
          if (flag) {  // make a copy of elemental matrix
            local_op_->matrices_shadow[c] = Acell;
            flag = false;
          }
          for (int m = 0; m < Acell.NumCols(); m++) Acell(n, m) = 0.0;
        }
      }

      for (int n = 0; n != nfaces; ++n) {
        int f = faces[n];
        double value = bc_value[f];

        if (bc_model[f] == OPERATOR_BC_DIRICHLET) {
          if (flag) {  // make a copy of cell-based matrix
            local_op_->matrices_shadow[c] = Acell;
            flag = false;
          }
     
          if (eliminate) {
            for (int m = 0; m < nfaces; m++) {
              rhs_face[0][faces[m]] -= Acell(m, n) * value;
              Acell(m, n) = 0.0;
            }
          }

          if (primary) {
            rhs_face[0][f] = value;
            // Acell(n, n) = 1.0 / face_get_cells[f];
          }
        }
      }
    }
  } 

  global_op_->rhs()->GatherGhostedToMaster("face", Add);
}


/* ******************************************************************
* Put here stuff that has to be done in constructor.
****************************************************************** */
void OperatorElasticity::InitElasticity_(Teuchos::ParameterList& plist)
{
  // Read schema for the mimetic discretization method.
  Teuchos::ParameterList& schema_list = plist.sublist("schema");

  std::vector<std::string> name;
  if (schema_list.isParameter("location")) {
    name = plist.get<Teuchos::Array<std::string> > ("location").toVector();
  } else {
    Errors::Message msg;
    msg << "OperatorElasticity: schema->location is missing.";
    Exceptions::amanzi_throw(msg);
  }

  std::vector<std::string> type;
  if (plist.isParameter("type")) {
    type = plist.get<Teuchos::Array<std::string> > ("type").toVector();
  } else {
    Errors::Message msg;
    msg << "OperatorElasticity: schema->type is missing.";
    Exceptions::amanzi_throw(msg);
  }

  std::vector<int> ndofs;
  if (plist.isParameter("number")) {
    ndofs = plist.get<Teuchos::Array<int> > ("number").toVector();
  } else {
    Errors::Message msg;
    msg << "OperatorElasticity: schema->number is missing.";
    Exceptions::amanzi_throw(msg);
  }

  // Create schema and save it.
  Schema my_schema;
  my_schema.SetBase(OPERATOR_SCHEMA_BASE_CELL);
  for (int i = 0; i < name.size(); i++) {
    my_schema.AddItem(my_schema.StringToLocation(name[i]), SCHEMA_DOFS_SCALAR, ndofs[i]);
  }
  my_schema.Finalize(mesh_);

  // create or check the existing Operator
  local_schema_row_ = my_schema;
  local_schema_row_ = my_schema;

  if (global_op_ == Teuchos::null) {
    global_schema_col_ = my_schema;
    global_schema_row_ = my_schema;

    // build the CVS from the global schema
    Teuchos::RCP<CompositeVectorSpace> cvs = Teuchos::rcp(new CompositeVectorSpace());
    cvs->SetMesh(mesh_)->SetGhosted(true);

    for (int i = 0; i < name.size(); i++) {
      cvs->AddComponent(name[i], my_schema.StringToMeshID(name[i]), ndofs[i]);
    }

    global_op_ = Teuchos::rcp(new Operator_Schema(cvs, cvs, plist, my_schema, my_schema));
  } else {
    // constructor was given an Operator
    global_schema_col_ = global_op_->schema_col();
    global_schema_row_ = global_op_->schema_row();
    mesh_ = global_op_->DomainMap().Mesh();
  }

  // create the local Op and register it with the global Operator
  local_op_ = Teuchos::rcp(new Op_Cell_Schema(my_schema, my_schema, mesh_));
  global_op_->OpPushBack(local_op_);
  
  // mesh info
  ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  nedges_owned = mesh_->num_entities(AmanziMesh::EDGE, AmanziMesh::OWNED);

  ncells_wghost = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::USED);
  nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);
  nedges_wghost = mesh_->num_entities(AmanziMesh::EDGE, AmanziMesh::USED);

  K_ = Teuchos::null;
}

}  // namespace Operators
}  // namespace Amanzi
