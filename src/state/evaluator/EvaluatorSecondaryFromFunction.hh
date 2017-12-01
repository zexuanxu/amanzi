/* -*-  mode: c++; indent-tabs-mode: nil -*- */
/*
  Copyright 2010-201x held jointly, see COPYRIGHT.
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Ethan Coon
*/

//! A secondary variable evaluator which evaluates functions on its dependenecies.

/*!
Uses functions to evaluate arbitrary algebraic functions of its dependencies.

For example, one might write a dependency:

  VARNAME = 0.2 * DEP1 - DEP2 + 3

as:

Example:
..xml:
    <ParameterList name="VARNAME">
      <Parameter name="field evaluator type" type="string" value="secondary variable from function"/>
      <Parameter name="evaluator dependencies" type="Array{string}" value="{DEP1, DEP2}"/>
      <ParameterList name="function">
        <ParameterList name="function-linear">
          <Parameter name="x0" type="Array(double)" value="{0.0,0.0}" />
          <Parameter name="y0" type="double" value="3." />
          <Parameter name="gradient" type="Array(double)" value="{0.2, -1}" />
        </ParameterList>
      </ParameterList>
    </ParameterList>


Note this is not done by region currently, but could easily be extended to do
so if it was found useful.
*/

#ifndef STATE_EVALUATOR_SECONDARY_FROMFUNCTION_HH_
#define STATE_EVALUATOR_SECONDARY_FROMFUNCTION_HH_

#include <string>
#include <vector>

#include "Teuchos_ParameterList.hpp"
#include "Teuchos_RCP.hpp"

#include "State.hh"
#include "EvaluatorSecondary.hh"
#include "Evaluator_Factory.hh"
#include "CompositeVectorSpace.hh"
#include "CompositeVector.hh"

namespace Amanzi {

class Function;

class EvaluatorSecondaryFromFunction :
      public EvaluatorSecondary<CompositeVector,CompositeVectorSpace> {

 public:
  explicit
  EvaluatorSecondaryFromFunction(Teuchos::ParameterList& plist);
  EvaluatorSecondaryFromFunction(const EvaluatorSecondaryFromFunction& other);
  virtual Teuchos::RCP<Evaluator> Clone() const;

 protected:
  // These do the actual work
  virtual void Evaluate_(const State& S, CompositeVector& result);

  // This should get some careful thought of the right strategy.  Punting for now --etc
  virtual void EvaluatePartialDerivative_(const State& S, const Key& wrt_key, CompositeVector& result) {
    result.PutScalar(0.);
  }

 protected:
  Teuchos::RCP<const Function> func_;

 private:
  static Utils::RegisteredFactory<Evaluator,EvaluatorSecondaryFromFunction> fac_;

}; // class

} // namespace Amanzi

#endif