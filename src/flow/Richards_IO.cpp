/*
This is the flow component of the Amanzi code. 

Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
Amanzi is released under the three-clause BSD License. 
The terms of use and "as is" disclaimer for this license are 
provided in the top-level COPYRIGHT file.

Authors: Neil Carlson (nnc@lanl.gov), 
         Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <set>
#include <vector>
#include <string>

#include "Teuchos_ParameterList.hpp"

#include "WRM_vanGenuchten.hpp"
#include "WRM_BrooksCorey.hpp"
#include "WRM_fake.hpp"
#include "Richards_PK.hpp"

namespace Amanzi {
namespace AmanziFlow {

/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
void Richards_PK::ProcessParameterList()
{
  Errors::Message msg;

  // create verbosity list if it does not exist
  if (! rp_list_.isSublist("VerboseObject")) {
    Teuchos::ParameterList verbosity_list;
    verbosity_list.set<std::string>("Verbosity Level", "none");
    rp_list_.set("VerboseObject", verbosity_list);
  }

  // extract verbosity level
  Teuchos::ParameterList verbosity_list = rp_list_.get<Teuchos::ParameterList>("VerboseObject");
  std::string verbosity_name = verbosity_list.get<std::string>("Verbosity Level");
  ProcessStringVerbosity(verbosity_name, &verbosity);

  // Process main one-line options (not sublists)
  std::string krel_method_name = rp_list_.get<string>("relative permeability");
  ProcessStringRelativePermeability(krel_method_name, &Krel_method);
 
  atm_pressure = rp_list_.get<double>("atmospheric pressure", 101325.0);
 
  string mfd3d_method_name = rp_list_.get<string>("discretization method", "optimized mfd");
  ProcessStringMFD3D(mfd3d_method_name, &mfd3d_method_); 

  // Create the BC objects.
  Teuchos::RCP<Teuchos::ParameterList>
      bc_list = Teuchos::rcp(new Teuchos::ParameterList(rp_list_.sublist("boundary conditions", true)));
  FlowBCFactory bc_factory(mesh_, bc_list);

  bc_pressure = bc_factory.createPressure();
  bc_head = bc_factory.createStaticHead(atm_pressure, rho, gravity_);
  bc_flux = bc_factory.createMassFlux();
  bc_seepage = bc_factory.createSeepageFace();

  ValidateBoundaryConditions(bc_pressure, bc_head, bc_flux);

  double time = T_physics;
  bc_pressure->Compute(time);
  bc_head->Compute(time);
  bc_flux->Compute(time);
  bc_seepage->Compute(time);

  // Create water retention models
  if (! rp_list_.isSublist("Water retention models")) {
    msg << "There is no Water retention models list";
    Exceptions::amanzi_throw(msg);
  }
  Teuchos::ParameterList& vG_list = rp_list_.sublist("Water retention models");

  int nblocks = 0;  // Find out how many WRM entries there are.
  for (Teuchos::ParameterList::ConstIterator i = vG_list.begin(); i != vG_list.end(); i++) {
    if (vG_list.isSublist(vG_list.name(i))) {
      nblocks++;
    } else {
      msg << "Richards Problem: water retention model contains an entry that is not a sublist.";
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

        double m = wrm_list.get<double>("van Genuchten m");
        double alpha = wrm_list.get<double>("van Genuchten alpha");
        double l = wrm_list.get<double>("van Genuchten l", 0.5);
        double sr = wrm_list.get<double>("residual saturation");
        double pc0 = wrm_list.get<double>("regularization interval", 0.0);
        std::string krel_function = wrm_list.get<std::string>("relative permeability model", "Mualem");
        VerifyWRMparameters(m, alpha, sr, pc0);
        VerifyStringMualemBurdine(krel_function);

        WRM[iblock] = Teuchos::rcp(new WRM_vanGenuchten(region, m, l, alpha, sr, krel_function, pc0));

      } else if (wrm_list.get<string>("Water retention model") == "Brooks Corey") {
        std::string region = wrm_list.get<std::string>("Region");  // associated mesh block

        double lambda = wrm_list.get<double>("Brooks Corey lambda");
        double alpha = wrm_list.get<double>("Brooks Corey alpha");
        double l = wrm_list.get<double>("Brooks Corey l", 0.5);
        double sr = wrm_list.get<double>("residual saturation");
        double pc0 = wrm_list.get<double>("regularization interval", 0.0);
        std::string krel_function = wrm_list.get<std::string>("relative permeability model", "Mualem");
        VerifyWRMparameters(lambda, alpha, sr, pc0);
        VerifyStringMualemBurdine(krel_function);

        WRM[iblock] = Teuchos::rcp(new WRM_BrooksCorey(region, lambda, l, alpha, sr, krel_function, pc0));

      } else if (wrm_list.get<string>("Water retention model") == "fake") {
        std::string region = wrm_list.get<std::string>("Region");  // associated mesh block
        WRM[iblock] = Teuchos::rcp(new WRM_fake(region));

      } else {
        msg << "Richards Problem: unknown water retention model.";
        Exceptions::amanzi_throw(msg);
      }
      iblock++;
    }
  }

  // Time integrator for period I, temporary called initial guess initialization
  if (rp_list_.isSublist("initial guess pseudo time integrator")) {
    Teuchos::ParameterList& igs_list = rp_list_.sublist("initial guess pseudo time integrator");

    ti_specs_igs_.initialize_with_darcy = igs_list.get<bool>("initialize with darcy", false);
    ti_specs_igs_.clip_saturation = igs_list.get<double>("clipping saturation value", 0.6);

    std::string ti_method_name = igs_list.get<string>("time integration method", "none");
    ProcessStringTimeIntegration(ti_method_name, &ti_method_igs);
    ProcessSublistTimeIntegration(igs_list, ti_method_name, ti_specs_igs_);

    preconditioner_name_igs_ = FindStringPreconditioner(igs_list);
    std::string linear_solver_name = FindStringLinearSolver(igs_list);
    ProcessStringLinearSolver(linear_solver_name, &max_itrs, &convergence_tol);

    ProcessStringErrorOptions(igs_list, &error_control_igs_);
  }

  // Time integrator for period II, temporary called steady-state time integrator
  if (rp_list_.isSublist("steady state time integrator")) {
    Teuchos::ParameterList& sss_list = rp_list_.sublist("steady state time integrator");

    ti_specs_sss_.initialize_with_darcy = sss_list.get<bool>("initialize with darcy", false);
    ti_specs_sss_.clip_saturation = sss_list.get<double>("clipping saturation value", 0.6);

    std::string ti_method_name = sss_list.get<string>("time integration method", "none");
    ProcessStringTimeIntegration(ti_method_name, &ti_method_sss);
    ProcessSublistTimeIntegration(sss_list, ti_method_name, ti_specs_sss_);

    preconditioner_name_sss_ = FindStringPreconditioner(sss_list);
    std::string linear_solver_name = FindStringLinearSolver(sss_list);
    ProcessStringLinearSolver(linear_solver_name, &max_itrs, &convergence_tol);

    ProcessStringErrorOptions(sss_list, &error_control_sss_);

  } else if (verbosity >= FLOW_VERBOSITY_LOW) {
    printf("Richards Problem: there is no sublist for steady-state calculations.\n");
  }

  // Time integrator for period III, called transient time integrator
  if (rp_list_.isSublist("transient time integrator")) {
    Teuchos::ParameterList& trs_list = rp_list_.sublist("transient time integrator");

    string ti_method_name = trs_list.get<string>("time integration method", "none");
    ProcessStringTimeIntegration(ti_method_name, &ti_method_trs);
    ProcessSublistTimeIntegration(trs_list, ti_method_name, ti_specs_trs_);

    preconditioner_name_trs_ = FindStringPreconditioner(trs_list);
    std::string linear_solver_name = FindStringLinearSolver(trs_list);
    ProcessStringLinearSolver(linear_solver_name, &max_itrs, &convergence_tol);

    ProcessStringErrorOptions(trs_list, &error_control_trs_);

  } else if (verbosity >= FLOW_VERBOSITY_LOW) {
    printf("Warning: Richards Problem has no sublist <transient time integration>.\n");
  }

  // allowing developer to use non-standard simulation modes
  if (! rp_list_.isParameter("developer access granted")) AnalysisTI_Specs();

  // optional debug output
  CalculateWRMcurves(rp_list_);
}


/* ****************************************************************
* Process time integration sublist.
**************************************************************** */
void Richards_PK::ProcessSublistTimeIntegration(
    Teuchos::ParameterList& list, const std::string name, TI_Specs& ti_specs)
{
  Errors::Message msg;

  if (list.isSublist(name)) {
    Teuchos::ParameterList& tmp_list = list.sublist(name);
    ti_specs.atol = tmp_list.get<double>("absolute error tolerance", FLOW_TI_ABSOLUTE_TOLERANCE);
    ti_specs.rtol = tmp_list.get<double>("relative error tolerance", FLOW_TI_RELATIVE_TOLERANCE);
    ti_specs.residual_tol = tmp_list.get<double>("convergence tolerance", FLOW_TI_NONLINEAR_RESIDUAL_TOLERANCE);
    ti_specs.max_itrs = tmp_list.get<int>("maximum number of iterations", FLOW_TI_MAX_ITERATIONS);

    ti_specs.T0 = tmp_list.get<double>("start time", 0.0);
    ti_specs.T1 = tmp_list.get<double>("end time", 0.0);
    ti_specs.dT0 = tmp_list.get<double>("initial time step", AmanziFlow::FLOW_INITIAL_DT);
    ti_specs.dTmax = tmp_list.get<double>("maximum time step", AmanziFlow::FLOW_MAXIMUM_DT);

  } else {
    msg << "Richards PK: specified time integration sublist does not exist.";
    Exceptions::amanzi_throw(msg);
  }
}


/* ****************************************************************
* Process string for error control options
**************************************************************** */
void Richards_PK::ProcessStringErrorOptions(Teuchos::ParameterList& list, int* control)
{
  *control = 0;
  if (list.isParameter("error control options")){
    std::vector<std::string> options;
    options = list.get<Teuchos::Array<std::string> >("error control options").toVector();

    for (int i=0; i < options.size(); i++) {
      if (options[i] == "pressure") {
        *control += FLOW_TI_ERROR_CONTROL_PRESSURE;
      } else if (options[i] == "saturation") {
        *control += FLOW_TI_ERROR_CONTROL_SATURATION;
      } else if (options[i] == "residual") {
        *control += FLOW_TI_ERROR_CONTROL_RESIDUAL;
      }
    }
  }
}


/* ****************************************************************
* Process string for the relative permeability
**************************************************************** */
void Richards_PK::ProcessStringRelativePermeability(const std::string name, int* method)
{
  Errors::Message msg;
  if (name == "upwind with gravity") {
    *method = AmanziFlow::FLOW_RELATIVE_PERM_UPWIND_GRAVITY;
  } else if (name == "cell centered") {
    *method = AmanziFlow::FLOW_RELATIVE_PERM_CENTERED;
  } else if (name == "upwind with Darcy flux") {
    *method = AmanziFlow::FLOW_RELATIVE_PERM_UPWIND_DARCY_FLUX;
  } else if (name == "arithmetic mean") {
    *method = AmanziFlow::FLOW_RELATIVE_PERM_ARITHMETIC_MEAN;
  } else {
    msg << "Richards Problem: unknown relative permeability method has been specified.";
    Exceptions::amanzi_throw(msg);
  }
}


/* ****************************************************************
* Verify string for the relative permeability model.
**************************************************************** */
void Richards_PK::VerifyStringMualemBurdine(const std::string name)
{
  Errors::Message msg;
  if (name != "Mualem" && name != "Burdine") {
    msg << "Richards PK: supported relative permeability models are Mualem and Burdine.";
    Exceptions::amanzi_throw(msg);
  }
}


/* ****************************************************************
* Verify string for the relative permeability model.
**************************************************************** */
void Richards_PK::VerifyWRMparameters(double m, double alpha, double sr, double pc0)
{
  Errors::Message msg;
  if (m < 0.0 || alpha < 0.0 || sr < 0.0 || pc0 < 0.0) {
    msg << "Richards PK: Negative parameter in one of the water retention models.";
    Exceptions::amanzi_throw(msg);    
  }
  if (sr > 1.0) {
    msg << "Richards PK: residual saturation is greater than 1.";
    Exceptions::amanzi_throw(msg);    
  }
}


/* ****************************************************************
* Process string for the time integration method.
**************************************************************** */
void Richards_PK::ProcessStringTimeIntegration(const std::string name, int* method)
{
  Errors::Message msg;
  if (name == "Picard") {
    *method = AmanziFlow::FLOW_TIME_INTEGRATION_PICARD;
  } else if (name == "backward Euler") {
    *method = AmanziFlow::FLOW_TIME_INTEGRATION_BACKWARD_EULER;
  } else if (name == "BDF1") {
    *method = AmanziFlow::FLOW_TIME_INTEGRATION_BDF1;
  } else if (name == "BDF2") {
    *method = AmanziFlow::FLOW_TIME_INTEGRATION_BDF2;
  } else {
    msg << "Richards PK: unknown time integration method has been specified.";
    Exceptions::amanzi_throw(msg);
  }
}


/* ****************************************************************
* Process string for the linear solver.
**************************************************************** */
void Richards_PK::ProcessStringLinearSolver(
    const std::string name, int* max_itrs, double* convergence_tol)
{
  Errors::Message msg;

  if (! solver_list_.isSublist(name)) {
    msg << "Richards PK: linear solver does not exist for a time integrator.";
    Exceptions::amanzi_throw(msg);
  }

  Teuchos::ParameterList& tmp_list = solver_list_.sublist(name);
  *max_itrs = tmp_list.get<int>("maximum number of iterations", 100);
  *convergence_tol = tmp_list.get<double>("error tolerance", 1e-12);
}


/* ****************************************************************
* Find string for the preconditoner.
**************************************************************** */
std::string Richards_PK::FindStringPreconditioner(const Teuchos::ParameterList& list)
{   
  Errors::Message msg;
  std::string name; 

  if (list.isParameter("preconditioner")) {
    name = list.get<string>("preconditioner");
  } else {
    msg << "Richards PK: steady state time integrator does not define <preconditioner>.";
    Exceptions::amanzi_throw(msg);
  }

  if (! preconditioner_list_.isSublist(name)) {
    msg << "Richards PK: steady state preconditioner does not exist.";
    Exceptions::amanzi_throw(msg);
  }
  return name;
}


/* ****************************************************************
* Find string for the preconditoner.
**************************************************************** */
std::string Richards_PK::FindStringLinearSolver(const Teuchos::ParameterList& list)
{   
  Errors::Message msg;
  std::string name; 

  if (list.isParameter("linear solver")) {
    name = list.get<string>("linear solver");
  } else {
    msg << "Richards PK: steady state time integrator does not define <linear solver>.";
    Exceptions::amanzi_throw(msg);
  }

  if (! solver_list_.isSublist(name)) {
    msg << "Richards PK: steady state linear solver does not exist.";
    Exceptions::amanzi_throw(msg);
  }
  return name;
}


/* ****************************************************************
* Find string for the preconditoner.
**************************************************************** */
void Richards_PK::CalculateWRMcurves(Teuchos::ParameterList& list)
{
  if (MyPID == 0 && verbosity >= FLOW_VERBOSITY_MEDIUM) {
    if (list.isParameter("calculate WRM curves")) {
      std::printf("Richards PK: saving WRM curves in file wrm_curves.txt\n");
      ofstream ofile;
      ofile.open("wrm_curves.txt");

      std::vector<double> spe;
      spe = list.get<Teuchos::Array<double> >("calculate WRM curves").toVector();

      for (double pc = spe[0]; pc < spe[2]; pc += spe[1]) {
        ofile << pc << " ";
        for (int mb = 0; mb < WRM.size(); mb++) {
          double krel = WRM[mb]->k_relative(pc);
          ofile << krel << " ";
        }
        ofile << endl;
      }
      ofile.close();
    }
  }
}


/* ****************************************************************
* Analyzin time integration specs for logical consistency
**************************************************************** */
void Richards_PK::AnalysisTI_Specs()
{
  Errors::Message msg;
  if (ti_specs_igs_.initialize_with_darcy) {
    if (ti_specs_sss_.initialize_with_darcy || ti_specs_trs_.initialize_with_darcy) { 
      msg << "Richards PK: cannot re-initialize Darcy solver without developer password.";
      Exceptions::amanzi_throw(msg);
    }
  }
}


/* ****************************************************************
* Printing information about Flow status 
**************************************************************** */
void Richards_PK::PrintStatistics() const
{
  if (! MyPID && verbosity > 0) {
    cout << "Flow PK:" << endl;
    cout << "  Verbosity level = " << verbosity << endl;
    cout << "  Enable internal tests = " << (internal_tests ? "yes" : "no")  << endl;
    cout << "  Upwind = " << ((Krel_method == FLOW_RELATIVE_PERM_UPWIND_GRAVITY) ? "gravity" : "other") << endl;
  }
}

}  // namespace AmanziFlow
}  // namespace Amanzi


