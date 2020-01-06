/*
 Shallow Water PK
 
 Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL.
 Amanzi is released under the three-clause BSD License.
 The terms of use and "as is" disclaimer for this license are
 provided in the top-level COPYRIGHT file.
 
 Author: Svetlana Tokareva (tokareva@lanl.gov)
 */

#ifndef AMANZI_SHALLOW_WATER_PK_HH_
#define AMANZI_SHALLOW_WATER_PK_HH_

// TPLs
#include "Epetra_Vector.h"
#include "Epetra_IntVector.h"
#include "Epetra_Import.h"
#include "Teuchos_RCP.hpp"

// Amanzi
#include "CompositeVector.hh"
#include "DenseVector.hh"
#include "Key.hh"
#include "LimiterCell.hh"
#include "PK.hh"
#include "PK_Explicit.hh"
#include "PK_Factory.hh"
#include "PK_Physical.hh"
#include "ReconstructionCell.hh"
#include "State.hh"
#include "Tensor.hh"
#include "Units.hh"
#include "VerboseObject.hh"

namespace Amanzi {
namespace ShallowWater {
    
class ShallowWater_PK : public PK_Physical,
                        public PK_Explicit<Epetra_Vector> {
                            
 public:
                            
    ShallowWater_PK(Teuchos::ParameterList& pk_tree,
                         const Teuchos::RCP<Teuchos::ParameterList>& glist,
                         const Teuchos::RCP<State>& S,
                         const Teuchos::RCP<TreeVector>& soln);

    ShallowWater_PK(const Teuchos::RCP<Teuchos::ParameterList>& glist,
                         Teuchos::RCP<State> S,
                         const std::string& pk_list_name,
                         std::vector<std::string>& component_names);

    ShallowWater_PK() {};
                            
    virtual void Setup(const Teuchos::Ptr<State>& S) {};
    virtual void Initialize(const Teuchos::Ptr<State>& S) {};
    
    virtual double get_dt() {};
    virtual void set_dt(double dt) {};
                     
    // Advance PK by step size dt.
    virtual bool AdvanceStep(double t_old, double t_new, bool reinit=false);
                            
    // Commit any secondary (dependent) variables.
    virtual void CommitStep(double t_old, double t_new, const Teuchos::RCP<State>& S) {};
                            
    // Calculate any diagnostics prior to doing vis
    virtual void CalculateDiagnostics(const Teuchos::RCP<State>& S) {};
    
    virtual std::string name() { return "Shallow water PK"; }
                            
  protected:
    
    Teuchos::RCP<Teuchos::ParameterList> glist_;
    Teuchos::ParameterList ti_list_;
    Teuchos::RCP<TreeVector> soln_;
    Teuchos::RCP<State> S_;
                            
    double dummy_dt;
    int step_count;
                            
  private:
                            
    // factory registration
    static RegisteredPKFactory<ShallowWater_PK> reg_;
                            
};
    
    
} // namespace ShallowWater
} // namespace Amanzi

#endif

