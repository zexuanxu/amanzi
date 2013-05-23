/*
The transport component of the Amanzi code, serial unit tests.
License: BSD
Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <vector>

#include "UnitTest++.h"

#include "Teuchos_ParameterList.hpp"
#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterXMLFileReader.hpp"

#include "MeshFactory.hh"
#include "MeshAudit.hh"

#include "State.hh"
#include "Transport_PK.hh"


/* **************************************************************** 
 * Test Init() procedure in the constructor.
 * ************************************************************* */
TEST(CONSTRUCTOR) {
  using namespace Teuchos;
  using namespace Amanzi;
  using namespace Amanzi::AmanziMesh;
  using namespace Amanzi::AmanziTransport;
  using namespace Amanzi::AmanziGeometry;

  std::cout << "Test: read transport XML file" << endl;
#ifdef HAVE_MPI
  Epetra_MpiComm *comm = new Epetra_MpiComm(MPI_COMM_WORLD);
#else
  Epetra_SerialComm *comm = new Epetra_SerialComm();
#endif

  /* read parameter list */
  ParameterList parameter_list;
  string xmlFileName = "test/transport_mics.xml";
  // DEPRECATED  updateParametersFromXmlFile(xmlFileName, &parameter_list);

  ParameterXMLFileReader xmlreader(xmlFileName);
  parameter_list = xmlreader.getParameters();  
 
  /* create an MSTK mesh framework */
  ParameterList region_list = parameter_list.get<Teuchos::ParameterList>("Regions");
  GeometricModelPtr gm = new GeometricModel(3, region_list, comm);

  FrameworkPreference pref;
  pref.clear();
  pref.push_back(Simple);

  MeshFactory factory(comm);
  factory.preference(pref);
  RCP<Mesh> mesh = factory(0.0,0.0,0.0, 1.0,1.0,1.0, 1, 2, 1, gm); 
 
  //MeshAudit audit(mesh);
  //audit.Verify();


  RCP<Transport_State> TS = rcp(new Transport_State(mesh,2));
  TS->Initialize();

  ParameterList transport_list =  parameter_list.get<Teuchos::ParameterList>("Transport");
  Transport_PK TPK(transport_list, TS);
  TPK.InitPK();
  TPK.PrintStatistics();

  double cfl = TPK.cfl();
  CHECK(0 < cfl && cfl <= 1.0);
 
  delete comm;
}



