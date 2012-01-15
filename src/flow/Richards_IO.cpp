/*
This is the flow component of the Amanzi code. 
License: BSD
Authors: Neil Carlson (nnc@lanl.gov), 
         Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include "WRM_vanGenuchten.hpp"
#include "WRM_analytic.hpp"
#include "Richards_PK.hpp"

namespace Amanzi {
namespace AmanziFlow {

/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
void Richards_PK::processParameterList()
{
  Teuchos::ParameterList preconditioner_list;
  preconditioner_list = rp_list.get<Teuchos::ParameterList>("Diffusion Preconditioner");

  max_itrs = preconditioner_list.get<int>("Max Iterations");
  err_tol = preconditioner_list.get<double>("Error Tolerance");

  flag_upwind = rp_list.get<bool>("Upwind relative permeability", true);
  atm_pressure = rp_list.get<double>("Atmospheric pressure");

  // Create the BC objects.
  Teuchos::RCP<Teuchos::ParameterList> 
      bc_list = Teuchos::rcp(new Teuchos::ParameterList(rp_list.sublist("boundary conditions", true)));
  FlowBCFactory bc_factory(mesh_, bc_list);

  bc_pressure = bc_factory.createPressure();
  bc_head = bc_factory.createStaticHead(atm_pressure, rho, -gravity[dim - 1]);  // We need |g| here.
  bc_flux = bc_factory.createMassFlux();

  validate_boundary_conditions(bc_pressure, bc_head, bc_flux);  

  double time = (standalone_mode) ? T_internal : T_physical;
  bc_pressure->Compute(time);
  bc_head->Compute(time);
  bc_flux->Compute(time);

  // Create water retention models
  if (!rp_list.isSublist("Water retention models")) {
    Errors::Message m("There is no Water retention models list");
    Exceptions::amanzi_throw(m);
  }
  Teuchos::ParameterList& vG_list = rp_list.sublist("Water retention models");

  int nblocks = 0;  // Find out how many WRM entries there are.
  for (Teuchos::ParameterList::ConstIterator i = vG_list.begin(); i != vG_list.end(); i++) {
    if (vG_list.isSublist(vG_list.name(i))) {
      nblocks++;
    } else {
      Errors::Message msg("Water retention models sublist contains an entry that is not a sublist.");
      Exceptions::amanzi_throw(msg);
    }
  }

  WRM.resize(nblocks);
  
  int iblock = 0;
  for (Teuchos::ParameterList::ConstIterator i = vG_list.begin(); i != vG_list.end(); i++) {
    if (vG_list.isSublist(vG_list.name(i))) {
      Teuchos::ParameterList& wrm_list = vG_list.sublist(vG_list.name(i));

      if (wrm_list.get<string>("Water retention model") == "van Genuchten") {
        std::string region = wrm_list.get<std::string>("Region");  // associated mesh block

        double vG_m = wrm_list.get<double>("van Genuchten m");
        double vG_alpha = wrm_list.get<double>("van Genuchten alpha");
        double vG_sr = wrm_list.get<double>("van Genuchten residual saturation");
	      
        WRM[iblock] = Teuchos::rcp(new WRM_vanGenuchten(region, vG_m, vG_alpha, vG_sr, atm_pressure));
      } 
      else if (wrm_list.get<string>("Water retention model") == "analytic") {
        std::string region = wrm_list.get<std::string>("Region");  // associated mesh block
        WRM[iblock] = Teuchos::rcp(new WRM_analytic(region));
      }
      iblock++;
    }
  }

  // Steady state solution
  Teuchos::ParameterList& steady_state_list = rp_list.sublist("Steady state solution");

  string method_name = steady_state_list.get<string>("method", "Picard");
  if (method_name == "Picard") {
    method_sss = FLOW_STEADY_STATE_PICARD;
  } else if (method_name == "backward Euler") {
    method_sss = FLOW_STEADY_STATE_BACKWARD_EULER;
  } else if (method_name == "BDF2") {
    method_sss = FLOW_STEADY_STATE_BDF2; 
  }

  max_itrs_sss = steady_state_list.get<int>("max iterations", FLOW_STEADY_STATE_MAX_ITERATIONS);
  err_tol_sss = steady_state_list.get<double>("solver tolerance", FLOW_STEADY_STATE_TOLERANCE);

  dT0 = steady_state_list.get<double>("Initial time step", FLOW_STEADY_STATE_INITIAL_DT);
}


/* ****************************************************************
* Printing information about Flow status 
**************************************************************** */
void Richards_PK::print_statistics() const
{
  if (!MyPID && verbosity > 0) {
    cout << "Flow PK:" << endl;
    cout << "    Execution mode = " << (standalone_mode ? "standalone" : "MPC") << endl;
    cout << "    Verbosity level = " << verbosity_level << endl;
    cout << "    Enable internal tests = " << (internal_tests ? "yes" : "no")  << endl;
    cout << "    Upwind = " << (flag_upwind ? "yes" : "no") << endl;
  }
}
 

}  // namespace AmanziFlow
}  // namespace Amanzi


