/*
  Time Integration

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Ethan Coon

  Virtual interface for timestep adaptation.
*/

#ifndef AMANZI_TIMESTEP_CONTROLLER_HH_
#define AMANZI_TIMESTEP_CONTROLLER_HH_

#include "Teuchos_RCP.hpp"

namespace Amanzi {

class TimestepController {

public:
  // virtual destructor
  virtual ~TimestepController() {};

  // single method for timestep control
  virtual double get_timestep(double dt, int iterations) = 0;
};

}  // namespace Amanzi

#endif
