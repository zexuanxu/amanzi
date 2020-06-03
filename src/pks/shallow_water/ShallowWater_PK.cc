/*
 Shallow water PK
 
 Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL.
 Amanzi is released under the three-clause BSD License.
 The terms of use and "as is" disclaimer for this license are
 provided in the top-level COPYRIGHT file.
 
 Author: Svetlana Tokareva (tokareva@lanl.gov)
 */

#include <vector>

// Amanzi::ShallowWater
#include "ShallowWater_PK.hh"

namespace Amanzi {
namespace ShallowWater {
    
    ShallowWater_PK::ShallowWater_PK(Teuchos::ParameterList& pk_tree,
                       const Teuchos::RCP<Teuchos::ParameterList>& glist,
                       const Teuchos::RCP<State>& S,
                       const Teuchos::RCP<TreeVector>& soln) :
    S_(S),
    soln_(soln),
	glist_(glist)
    {
        std::string pk_name = pk_tree.name();
        auto found = pk_name.rfind("->");
        if (found != std::string::npos) pk_name.erase(0, found + 2);
        
        // Create miscellaneous lists.
        Teuchos::RCP<Teuchos::ParameterList> pk_list = Teuchos::sublist(glist, "PKs", true);
        sw_list_ = Teuchos::sublist(pk_list, pk_name, true);
        
        // domain name
        domain_ = sw_list_->template get<std::string>("domain name", "surface");
        
        vo_ = Teuchos::null;
    }

    bool ShallowWater_PK::AdvanceStep(double t_old, double t_new, bool reinit)
    {

    	Comm_ptr_type comm = Amanzi::getDefaultComm();
    	int MyPID = comm->MyPID();

    	double dt = t_new - t_old;

    	bool failed = false;

    	double eps = 1.e-6;

    	// save a copy of primary and conservative fields
    	CompositeVector ponded_depth_tmp(*S_->GetFieldData("surface-ponded_depth", passwd_));
    	CompositeVector velocity_x_tmp(*S_->GetFieldData("surface-velocity-x", passwd_));
    	CompositeVector velocity_y_tmp(*S_->GetFieldData("surface-velocity-y", passwd_));

    	Epetra_MultiVector& B_vec_c = *S_->GetFieldData("surface-bathymetry",passwd_)->ViewComponent("cell");
    	Epetra_MultiVector B_vec_c_tmp(B_vec_c);

    	Epetra_MultiVector& h_vec_c = *S_->GetFieldData("surface-ponded_depth",passwd_)->ViewComponent("cell");
    	Epetra_MultiVector h_vec_c_tmp(h_vec_c);

    	Epetra_MultiVector& ht_vec_c = *S_->GetFieldData("surface-total_depth",passwd_)->ViewComponent("cell");
    	Epetra_MultiVector ht_vec_c_tmp(ht_vec_c);

        Epetra_MultiVector& vx_vec_c = *S_->GetFieldData("surface-velocity-x",passwd_)->ViewComponent("cell");
        Epetra_MultiVector vx_vec_c_tmp(vx_vec_c);

        Epetra_MultiVector& vy_vec_c = *S_->GetFieldData("surface-velocity-y",passwd_)->ViewComponent("cell");
        Epetra_MultiVector vy_vec_c_tmp(vy_vec_c);

        Epetra_MultiVector& qx_vec_c = *S_->GetFieldData("surface-discharge-x",passwd_)->ViewComponent("cell");
		Epetra_MultiVector qx_vec_c_tmp(qx_vec_c);

		Epetra_MultiVector& qy_vec_c = *S_->GetFieldData("surface-discharge-y",passwd_)->ViewComponent("cell");
		Epetra_MultiVector qy_vec_c_tmp(qy_vec_c);

        // distribute data to ghost cells
        S_->GetFieldData("surface-ponded_depth")->ScatterMasterToGhosted("cell");
        S_->GetFieldData("surface-velocity-x")->ScatterMasterToGhosted("cell");
        S_->GetFieldData("surface-velocity-y")->ScatterMasterToGhosted("cell");
        S_->GetFieldData("surface-discharge-x")->ScatterMasterToGhosted("cell");
        S_->GetFieldData("surface-discharge-y")->ScatterMasterToGhosted("cell");

        h_vec_c_tmp  = h_vec_c;
        ht_vec_c_tmp = ht_vec_c;
        vx_vec_c_tmp = vx_vec_c;
        vy_vec_c_tmp = vy_vec_c;
        qx_vec_c_tmp = qx_vec_c;
        qy_vec_c_tmp = qy_vec_c;

        // Shallow water equations have the form
        // U_t + F_x(U) + G_y(U) = S(U)

         std::vector<double> U, U_new;

         // Simplest first-order form
         // U_i^{n+1} = U_i^n - dt/vol * (F_{i+1/2}^n - F_{i-1/2}^n) + dt * S_i

         int ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::Parallel_type::OWNED);
         std::cout << "ncells_owned = " << ncells_owned << std::endl;

         std::cout << "dt = " << dt << std::endl;
         std::cout << "t_new = " << t_new << std::endl;

         U_new.resize(3);

         int c_debug = -10;

      	 for (int c = 0; c < ncells_owned; c++) {

//      		 std::cout << "c = " << c << std::endl;

      		 // mesh sizes
      		 double dx, dy;

      		 Amanzi::AmanziMesh::Entity_ID_List cedges, cfaces, fedges, edcells, fcells;

      		 mesh_->cell_get_edges(c,&cedges);
      		 mesh_->cell_get_faces(c,&cfaces,true);

      		 Amanzi::AmanziMesh::Entity_ID_List adjcells;
			 mesh_->cell_get_face_adj_cells(c, Amanzi::AmanziMesh::Parallel_type::OWNED,&adjcells);
			 unsigned int nadj = adjcells.size();

      		 Amanzi::AmanziGeometry::Point evec(2), normal(2);
      		 double farea;

      		 dx = mesh_->edge_length(0);
      		 dy = mesh_->edge_length(1);

      		 // cell volume
      		 double vol = mesh_->cell_volume(c);

             std::vector<double> FL, FR, FNum, FNum_rot, FS;  // fluxes
             std::vector<double> S;                           // source term
             std::vector<double> UL, UR, U;  			      // data for the fluxes

             UL.resize(3);
             UR.resize(3);

             FS.resize(3);

             FNum.resize(3);

             for (int i = 0; i < 3; i++) FS[i] = 0.;

             for (int f = 0; f < cfaces.size(); f++) {

				 int orientation;
				 normal = mesh_->face_normal(cfaces[f],false,c,&orientation);
				 mesh_->face_get_cells(cfaces[f],Amanzi::AmanziMesh::Parallel_type::OWNED,&fcells);
				 farea = mesh_->face_area(cfaces[f]);
//				 normal *= orientation;
				 normal /= farea;

//				 std::cout << "|n| = " << normal[0]*normal[0] + normal[1]*normal[1] << " " << orientation << std::endl;

				 double vn, vt;

				 Amanzi::AmanziGeometry::Point xcf = mesh_->face_centroid(cfaces[f]);

				 double ht_rec = Reconstruction(xcf[0],xcf[1],c,"surface-total_depth");
				 double B_rec  = Reconstruction(xcf[0],xcf[1],c,"surface-bathymetry");

				 if (ht_rec < B_rec) {
					 ht_rec = ht_vec_c[0][c];
					 B_rec  = B_vec_c[0][c];
				 }
				 double h_rec = ht_rec - B_rec;

			     if (h_rec < 0.) {
			    	 std::cout << "c = " << c << std::endl;
			    	 std::cout << "ht_rec = " << ht_rec << std::endl;
			    	 std::cout << "B_rec  = " << B_rec << std::endl;
			    	 std::cout << "h_rec  = " << h_rec << std::endl;
			    	 exit(0);
			     }

//				 double h_rec = Reconstruction(xcf[0],xcf[1],c,"surface-ponded_depth");
//				 double h_rec = h_vec_c[0][c];

				 double vx_rec = Reconstruction(xcf[0],xcf[1],c,"surface-velocity-x");
//				 double vx_rec = vx_vec_c[0][c];

				 double vy_rec = Reconstruction(xcf[0],xcf[1],c,"surface-velocity-y");
//				 double vy_rec = vy_vec_c[0][c];

				 double qx_rec = Reconstruction(xcf[0],xcf[1],c,"surface-discharge-x");
//				 double qx_rec = qx_vec_c[0][c];

				 double qy_rec = Reconstruction(xcf[0],xcf[1],c,"surface-discharge-y");
//				 double qy_rec = qy_vec_c[0][c];

//				 std::cout << "c = " << c << "/" << ncells_owned << ", h_rec = " << h_rec << std::endl;
//				 std::cout << "xcf = " << xcf << std::endl;

//				 vn =  vx_vec_c[0][c]*normal[0] + vy_vec_c[0][c]*normal[1];
//				 vt = -vx_vec_c[0][c]*normal[1] + vy_vec_c[0][c]*normal[0];

//				 UL[0] = h_vec_c[0][c];
//				 UL[1] = h_vec_c[0][c]*vn;
//				 UL[2] = h_vec_c[0][c]*vt;

//			     vx_rec = qx_rec/h_rec;
//			     vy_rec = qy_rec/h_rec;

			     vx_rec = 2.*h_rec*qx_rec/(h_rec*h_rec + fmax(h_rec*h_rec,eps*eps));
			     vy_rec = 2.*h_rec*qy_rec/(h_rec*h_rec + fmax(h_rec*h_rec,eps*eps));

				 vn =  vx_rec*normal[0] + vy_rec*normal[1];
				 vt = -vx_rec*normal[1] + vy_rec*normal[0];

				 UL[0] = h_rec;
				 UL[1] = h_rec*vn;
				 UL[2] = h_rec*vt;

				 int cn = WhetStone::cell_get_face_adj_cell(*mesh_, c, cfaces[f]);

				 if (cn == -1) {
 					 UR[0] = UL[0];
 					 UR[1] = UL[1];
 					 UR[2] = UL[2];
				 }
				 else {
					 int c_GID = mesh_->GID(c, AmanziMesh::CELL);
 			         int cn_GID = mesh_->GID(cn, AmanziMesh::CELL);

 			         double ht_rec = Reconstruction(xcf[0],xcf[1],cn,"surface-total_depth");
				     double B_rec  = Reconstruction(xcf[0],xcf[1],cn,"surface-bathymetry");

				     if (ht_rec < B_rec) {
						 ht_rec = ht_vec_c[0][cn];
						 B_rec  = B_vec_c[0][cn];
					 }
					 double h_rec = ht_rec - B_rec;

				     if (h_rec < 0.) {
				    	 std::cout << "cn = " << cn << std::endl;
				    	 std::cout << "ht_rec = " << ht_rec << std::endl;
				    	 std::cout << "B_rec  = " << B_rec << std::endl;
				    	 std::cout << "h_rec  = " << h_rec << std::endl;
				    	 exit(0);
				     }

// 					 double h_rec = Reconstruction(xcf[0],xcf[1],cn,"surface-ponded_depth");
// 					 double h_rec = h_vec_c[0][cn];

 					 double vx_rec = Reconstruction(xcf[0],xcf[1],cn,"surface-velocity-x");
// 					 double vx_rec = vx_vec_c[0][cn];

 					 double vy_rec = Reconstruction(xcf[0],xcf[1],cn,"surface-velocity-y");
// 					 double vy_rec = vy_vec_c[0][cn];

				     double qx_rec = Reconstruction(xcf[0],xcf[1],cn,"surface-discharge-x");
//				     double qx_rec = qx_vec_c[0][cn];

				     double qy_rec = Reconstruction(xcf[0],xcf[1],cn,"surface-discharge-y");
//				     double qy_rec = qy_vec_c[0][cn];

// 					std::cout << "cn = " << cn << "/" << ncells_owned << ", h_rec = " << h_rec << std::endl;

//					 vn =  vx_vec_c[0][cn]*normal[0] + vy_vec_c[0][cn]*normal[1];
//					 vt = -vx_vec_c[0][cn]*normal[1] + vy_vec_c[0][cn]*normal[0];

// 					 UR[0] = h_vec_c[0][cn];
// 					 UR[1] = h_vec_c[0][cn]*vn;
// 					 UR[2] = h_vec_c[0][cn]*vt;

//				     vx_rec = qx_rec/h_rec;
//				     vy_rec = qy_rec/h_rec;

				     vx_rec = 2.*h_rec*qx_rec/(h_rec*h_rec + fmax(h_rec*h_rec,eps*eps));
				     vy_rec = 2.*h_rec*qy_rec/(h_rec*h_rec + fmax(h_rec*h_rec,eps*eps));

					 vn =  vx_rec*normal[0] + vy_rec*normal[1];
					 vt = -vx_rec*normal[1] + vy_rec*normal[0];

 					 UR[0] = h_rec;
				     UR[1] = h_rec*vn;
				     UR[2] = h_rec*vt;

				 }

//				 normal[0] /= farea; normal[1] /= farea;

				 // debug
				 if (MyPID == 0) {
					 if (c == c_debug) {

					 for (int i = 0; i < 3; i++) {
						 std::cout << "UL[i] = " << UL[i] << std::endl;
					 }

					 for (int i = 0; i < 3; i++) {
						 std::cout << "UR[i] = " << UR[i] << std::endl;
					 }

					 }
				 }

//				 std::cout << "advance step UL: " << UL[0] << " " << UL[1] << " " << UL[2] << std::endl;
//				 std::cout << "advance step UR: " << UR[0] << " " << UR[1] << " " << UR[2] << std::endl;

				 FNum_rot = NumFlux_x(UL,UR);

				 FNum[0] = FNum_rot[0];
				 FNum[1] = FNum_rot[1]*normal[0] - FNum_rot[2]*normal[1];
				 FNum[2] = FNum_rot[1]*normal[1] + FNum_rot[2]*normal[0];

				 for (int i = 0; i < 3; i++) {
					 FS[i] += FNum[i]*farea;
				 }

             } // faces

             double h, u, v, qx, qy;

             U.resize(3);

             h  = h_vec_c[0][c];
             qx = qx_vec_c[0][c];
             qy = qy_vec_c[0][c];
//             u = qx/h;
//             v = qy/h;
             u = 2.*h*qx/(h*h + fmax(h*h,eps*eps));
             v = 2.*h*qy/(h*h + fmax(h*h,eps*eps));

             U[0] = h;
			 U[1] = h*u;
			 U[2] = h*v;

             S = NumSrc(U,c);

             for (int i = 0; i < 3; i++) {
            	 U_new[i] = U[i] - dt/vol*FS[i] + dt*S[i];
             }

             // debug
             if (MyPID == 0) {
            	 if (c == c_debug) {
            		 for (int i = 0; i < 3; i++) {
            			 std::cout << "U_new[i] = " << U_new[i] << std::endl;
            		 }
            	 }
             }

//             std::cout << "U_new: " << U_new[0] << " " << U_new[1] << " " << U_new[2] << std::endl;

//             h_vec_c_tmp[0][c]  = U_new[0];
//             vx_vec_c_tmp[0][c] = U_new[1]/U_new[0];
//             vy_vec_c_tmp[0][c] = U_new[2]/U_new[0];

//             qx_vec_c_tmp[0][c] = U_new[1];
//             qy_vec_c_tmp[0][c] = U_new[2];

             h  = U_new[0];
             qx = U_new[1];
             qy = U_new[2];

             h_vec_c_tmp[0][c] = h;
//             u = qx/h;
//             v = qy/h;
             u = 2.*h*qx/(h*h + fmax(h*h,eps*eps));
             v = 2.*h*qy/(h*h + fmax(h*h,eps*eps));
             vx_vec_c_tmp[0][c] = u;
             vy_vec_c_tmp[0][c] = v;
             qx_vec_c_tmp[0][c] = h*u;
             qy_vec_c_tmp[0][c] = h*v;

//             std::cout << vx_vec_c_tmp[0][c] << " " << vy_vec_c_tmp[0][c] << std::endl;
//             std::cout << h_vec_c_tmp[0][c] << " " << qx_vec_c_tmp[0][c] << " " << qy_vec_c_tmp[0][c] << std::endl;

//             std::cout << h_vec_c_tmp[0][c] << " " << h_vec_c[0][c] << std::endl;

          	 AmanziGeometry::Point xc = mesh_->cell_centroid(c);

          	 ht_vec_c_tmp[0][c] = h_vec_c_tmp[0][c] + B_vec_c[0][c];

      	 }

      	 h_vec_c  = h_vec_c_tmp;
      	 ht_vec_c = ht_vec_c_tmp;
      	 vx_vec_c = vx_vec_c_tmp;
      	 vy_vec_c = vy_vec_c_tmp;
      	 qx_vec_c = qx_vec_c_tmp;
      	 qy_vec_c = qy_vec_c_tmp;

    	 return failed;

    }

    void ShallowWater_PK::Setup(const Teuchos::Ptr<State>& S)
    {

        // SW conservative variables: (h, hu, hv)
        
        passwd_ = "state";  // owner's password
        
        mesh_ = S->GetMesh(domain_);
        dim_ = mesh_->space_dimension();
        
        // domain name
        domain_ = "surface";
        pressure_key_       = Keys::getKey(domain_, "pressure");
        velocity_x_key_     = Keys::getKey(domain_, "velocity-x");
        velocity_y_key_     = Keys::getKey(domain_, "velocity-y");
        discharge_x_key_    = Keys::getKey(domain_, "discharge-x");
        discharge_y_key_    = Keys::getKey(domain_, "discharge-y");
        ponded_depth_key_   = Keys::getKey(domain_, "ponded_depth");
        total_depth_key_    = Keys::getKey(domain_, "total_depth");
        bathymetry_key_     = Keys::getKey(domain_, "bathymetry");
        myPID_  		    = Keys::getKey(domain_, "PID");

        // primary fields
        
        // -- pressure
        if (!S->HasField(pressure_key_)) {
            S->RequireField(pressure_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
            ->SetComponent("cell", AmanziMesh::CELL, 1);

//            Teuchos::ParameterList elist;
//            elist.set<std::string>("evaluator name", "pressure");
//            pressure_eval_ = Teuchos::rcp(new PrimaryVariableFieldEvaluator(elist));
//            S->SetFieldEvaluator("pressure", pressure_eval_);
        }
        
        
        // ponded_depth_key_
        if (!S->HasField(ponded_depth_key_)) {
            S->RequireField(ponded_depth_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
            ->SetComponent("cell", AmanziMesh::CELL, 1);
        }
        
        // total_depth_key_
		if (!S->HasField(total_depth_key_)) {
			S->RequireField(total_depth_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
			->SetComponent("cell", AmanziMesh::CELL, 1);
		}

        // x velocity
        if (!S->HasField(velocity_x_key_)) {
            S->RequireField(velocity_x_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
            ->SetComponent("cell", AmanziMesh::CELL, 1);
        }
        
        // y velocity
        if (!S->HasField(velocity_y_key_)) {
            S->RequireField(velocity_y_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
            ->SetComponent("cell", AmanziMesh::CELL, 1);
        }

        // x discharge
		if (!S->HasField(discharge_x_key_)) {
			S->RequireField(discharge_x_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
			->SetComponent("cell", AmanziMesh::CELL, 1);
		}

		// y discharge
		if (!S->HasField(discharge_y_key_)) {
			S->RequireField(discharge_y_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
			->SetComponent("cell", AmanziMesh::CELL, 1);
		}

        // bathymetry
		if (!S->HasField(bathymetry_key_)) {
			S->RequireField(bathymetry_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
			->SetComponent("cell", AmanziMesh::CELL, 1);
		}

        // PID
	    if (!S->HasField(myPID_)) {
		    S->RequireField(myPID_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
		    ->SetComponent("cell", AmanziMesh::CELL, 1);
	    }

    }
    
    void ShallowWater_PK::Initialize(const Teuchos::Ptr<State>& S) {  // default

    	int ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::Parallel_type::OWNED);

    	if (!S_->GetField("surface-bathymetry", passwd_)->initialized()) {

    		S_->GetFieldData("surface-bathymetry", passwd_)->PutScalar(0.0);

//            Teuchos::ParameterList  bathymetry_ = glist_->sublist("bathymetry");
//    //        Teuchos::ParameterList& bathymetry_func_ = bathymetry_.sublist("function-smooth-step");
//            Amanzi::FunctionFactory fact;
//            Amanzi::Function *f = fact.Create(bathymetry_);
//
//			Epetra_MultiVector& B_vec_c = *S_->GetFieldData("surface-bathymetry",passwd_)->ViewComponent("cell");
//
//			for (int c = 0; c < ncells_owned; c++) {
//				AmanziGeometry::Point xc = mesh_->cell_centroid(c);
////				B_vec_c[0][c] = Bathymetry(xc[0],xc[1]);
//				std::vector<double> x(1,xc[0]);
//				B_vec_c[0][c] = (*f)(x);
//			}

			S_->GetField("surface-bathymetry", passwd_)->set_initialized();
		}



        if (!S_->GetField("surface-ponded_depth", passwd_)->initialized()) {

//            S_->GetFieldData("surface-ponded_depth", passwd_)->PutScalar(2.0);

        	Epetra_MultiVector& h_vec_c = *S_->GetFieldData("surface-ponded_depth",passwd_)->ViewComponent("cell");
        	Epetra_MultiVector& ht_vec_c = *S_->GetFieldData("surface-total_depth",passwd_)->ViewComponent("cell");
        	Epetra_MultiVector& B_vec_c = *S_->GetFieldData("surface-bathymetry",passwd_)->ViewComponent("cell");

        	S_->GetFieldData("surface-ponded_depth", passwd_)->PutScalar(1.0);

//        	Teuchos::ParameterList  state_ = glist_->sublist("state");
//        	Teuchos::ParameterList  init_cond_ = state_.sublist("initial conditions");
//            Teuchos::ParameterList  ponded_depth_ic_ = init_cond_.sublist("ponded_depth");
//            Amanzi::FunctionFactory fact;
//            Amanzi::Function *f = fact.Create(ponded_depth_ic_);
//
//        	for (int c = 0; c < ncells_owned; c++) {
//        		AmanziGeometry::Point xc = mesh_->cell_centroid(c);
//        		std::vector<double> x(1,xc[0]);
//        		h_vec_c[0][c] = (*f)(x);
////        		if (xc[0] < 1000.) {
////        		   h_vec_c[0][c] = 10.;
////        		}
////			    else {
////			       h_vec_c[0][c] = 0.; //0.0000000001;
////			    }
//        		ht_vec_c[0][c] = h_vec_c[0][c] + B_vec_c[0][c];
//        	}

        	for (int c = 0; c < ncells_owned; c++) {
        		ht_vec_c[0][c] = h_vec_c[0][c] + B_vec_c[0][c];
        	}

        	S_->GetField("surface-ponded_depth", passwd_)->set_initialized();
        	S_->GetField("surface-total_depth", passwd_)->set_initialized();
        }

        if (!S_->GetField("surface-velocity-x", passwd_)->initialized()) {
            S_->GetFieldData("surface-velocity-x", passwd_)->PutScalar(0.0);
            S_->GetField("surface-velocity-x", passwd_)->set_initialized();
        }

        if (!S_->GetField("surface-velocity-y", passwd_)->initialized()) {
            S_->GetFieldData("surface-velocity-y", passwd_)->PutScalar(0.0);
            S_->GetField("surface-velocity-y", passwd_)->set_initialized();
        }

        if (!S_->GetField("surface-discharge-x", passwd_)->initialized()) {
            S_->GetFieldData("surface-discharge-x", passwd_)->PutScalar(0.0);
            S_->GetField("surface-discharge-x", passwd_)->set_initialized();
        }

        if (!S_->GetField("surface-discharge-y", passwd_)->initialized()) {
            S_->GetFieldData("surface-discharge-y", passwd_)->PutScalar(0.0);
            S_->GetField("surface-discharge-y", passwd_)->set_initialized();
        }

        Comm_ptr_type comm = Amanzi::getDefaultComm();
        int MyPID = comm->MyPID();

        if (!S_->GetField("surface-PID", passwd_)->initialized()) {

//            S_->GetFieldData("surface-ponded_depth", passwd_)->PutScalar(2.0);

        	Epetra_MultiVector& PID_c = *S_->GetFieldData("surface-PID",passwd_)->ViewComponent("cell");

        	for (int c = 0; c < ncells_owned; c++) {
			    PID_c[0][c] = MyPID;
        	}

        	S_->GetField("surface-PID", passwd_)->set_initialized();
        }

    }

//==========================================================================
//
// Discretization: numerical fluxes, source terms, etc
//
//==========================================================================

    //--------------------------------------------------------------
    // minmod function
    //--------------------------------------------------------------
    double minmod(double a, double b) {
        double m;

        if (a*b > 0) {
            if (std::fabs(a) < std::fabs(b)) {
                m = a;
            }
            else {
                m = b;
            }
        }
        else {
            m = 0.;
        }

		return m;

    }

    //--------------------------------------------------------------
    // Barth-Jespersen limiter of the gradient
    //--------------------------------------------------------------
    void ShallowWater_PK::BJ_lim(WhetStone::DenseMatrix grad, WhetStone::DenseMatrix& grad_lim, int c, Key field_key_) {

    	std::vector<double> Phi_k;
    	std::vector<double> u_av;
    	double u_av0;
    	double Phi;
    	double umin, umax, uk;
    	double tol = 1.e-12;

    	Amanzi::AmanziGeometry::Point xc(2), xv(2);

    	Amanzi::AmanziMesh::Entity_ID_List cedges, cfaces, cnodes, fedges, edcells, fcells;

		mesh_->cell_get_edges(c,&cedges);
		mesh_->cell_get_faces(c,&cfaces,true);
		mesh_->cell_get_nodes(c,&cnodes);

		Epetra_MultiVector& h_vec_c = *S_->GetFieldData(field_key_,passwd_)->ViewComponent("cell");

		u_av0 = h_vec_c[0][c];

		u_av.resize(cfaces.size());

		for (int f = 0; f < cfaces.size(); f++) {

			int cn = WhetStone::cell_get_face_adj_cell(*mesh_, c, cfaces[f]);

			if (cn == -1) cn = c;

			u_av[f] = h_vec_c[0][cn];

		} // f

		umin = *min_element(u_av.begin(),u_av.end());
		umax = *max_element(u_av.begin(),u_av.end());

//		std::cout << "BJ c = " << c << std::endl;

//		std::cout << "umin = " << umin << ", umax = " << umax << std::endl;

		xc = mesh_->cell_centroid(c);

		Phi_k.resize(cnodes.size());

		for (int k = 0; k < cnodes.size(); k++) {

			mesh_->node_get_coordinates(cnodes[k],&xv);

//		for (int k = 0; k < cfaces.size(); k++) {
//
//			Amanzi::AmanziGeometry::Point xv = mesh_->face_centroid(cfaces[k]);

		    uk = u_av0 + grad(0,0)*(xv[0]-xc[0]) + grad(1,0)*(xv[1]-xc[1]);

            if (uk - u_av0 > 0.) {
//            	std::cout << "(umax-u_av0)/(uk-u_av0) = " << (umax-u_av0)/(uk-u_av0) << std::endl;
//				std::cout << "umax-u_av0 = " << umax-u_av0 << std::endl;
//				std::cout << "uk-u_av0+tol = " << uk-u_av0+tol << std::endl;
//				std::cout << "uk-u_av0 = " << uk-u_av0 << std::endl;
                Phi_k[k] = std::min(1.,(umax-u_av0)/(uk-u_av0));
            }
            else {
                if (uk - u_av0 < 0.) {
//                	std::cout << "(umin-u_av0)/(uk-u_av0) = " << (umin-u_av0)/(uk-u_av0) << std::endl;
//    				std::cout << "umin-u_av0 = " << umin-u_av0 << std::endl;
//    				std::cout << "uk-u_av0+tol = " << uk-u_av0 << std::endl;
//    				std::cout << "uk-u_av0 = " << uk-u_av0 << std::endl;
                    Phi_k[k] = std::min(1.,(umin-u_av0)/(uk-u_av0));
                }
                else {
//                	std::cout << "uk-u_av0 = " << uk-u_av0 << std::endl;
                    Phi_k[k] = 1.;
                }
            }

        } // k

        Phi = *min_element(Phi_k.begin(),Phi_k.end());

        // temporary fix to reduce the gradient
        // for better robustness on wet/dry front
        Phi = 0.1*Phi;

//        for (int k = 0; k < cnodes.size(); k++) {
//        	std::cout << Phi_k[k] << " ";
//        }
//        std::cout << std::endl;

//        std::cout << "Phi = " << Phi << std::endl;

//        Phi = 0.;

        grad_lim = Phi*grad;

    }

    //--------------------------------------------------------------
	// piecewise-linear reconstruction of the function
	//--------------------------------------------------------------
    double ShallowWater_PK::Reconstruction(double x, double y, int c, Key field_key_) {

    	// for Cartesian grids

    	// mesh sizes
		 double dx, dy;

		 Amanzi::AmanziMesh::Entity_ID_List cedges, cfaces, fedges, edcells, fcells;

		 mesh_->cell_get_edges(c,&cedges);
		 mesh_->cell_get_faces(c,&cfaces,true);

		 Amanzi::AmanziMesh::Entity_ID_List adjcells;
		 mesh_->cell_get_face_adj_cells(c, Amanzi::AmanziMesh::Parallel_type::OWNED,&adjcells);
		 unsigned int nadj = adjcells.size();

		 Amanzi::AmanziGeometry::Point evec(2), normal(2);
		 double farea;

		 dx = mesh_->edge_length(0);
		 dy = mesh_->edge_length(1);

		 // cell volume
		 double vol = mesh_->cell_volume(c);

		 std::vector<double> U, Un;

		 U.resize(3);
		 Un.resize(3);

		 WhetStone::DenseMatrix A(cfaces.size(),2), At(2,cfaces.size()), AtA(2,2), InvAtA(2,2);
		 WhetStone::DenseMatrix dudx(2,1), dudx_lim(2,1);
		 WhetStone::DenseMatrix b(cfaces.size(),1);
		 WhetStone::DenseMatrix Atb(2,1);

		 AmanziGeometry::Point xc = mesh_->cell_centroid(c);

		 Epetra_MultiVector& h_vec_c = *S_->GetFieldData(field_key_,passwd_)->ViewComponent("cell");

		 for (int f = 0; f < cfaces.size(); f++) {

//			 std::cout << "f = " << f << std::endl;

			 int cn = WhetStone::cell_get_face_adj_cell(*mesh_, c, cfaces[f]);
//
			 AmanziGeometry::Point xcn = mesh_->cell_centroid(cn);

			 if (cn == -1) {

				 // face centroid
				 Amanzi::AmanziGeometry::Point xcf = mesh_->face_centroid(cfaces[f]);
				 Amanzi::AmanziGeometry::Point dx = xcf-xcn;

				 // face area
				 double farea = mesh_->face_area(cfaces[f]);

				 // normal
				 Amanzi::AmanziGeometry::Point normal(2);
				 int orientation;
				 normal = mesh_->face_normal(cfaces[f],false,c,&orientation);
				 normal /= farea;

				 double l = dx[0]*normal[0] + dx[1]*normal[1];

				 xcn = xc + 2.*l*normal;

			 }

			 if (cn == -1) cn = c;

//			 std::cout << "xc = " << xc << ", xcn = " << xcn << std::endl;

			 A(f,0) = xcn[0] - xc[0];
			 A(f,1) = xcn[1] - xc[1];

//			 std::cout << "A(f,0) = " << A(f,0) << ",  A(f,1) = " <<  A(f,1) << std::endl;

			 b(f,0) = h_vec_c[0][cn] -  h_vec_c[0][c];

//			 std::cout << "b(f,0) = " << b(f,0) << std::endl;

		 }

		 At.Transpose(A);

//		 for (int f = 0; f < cfaces.size(); f++) {
//			 std::cout << "At(0,f) = " << At(0,f) << ",  At(1,f) = " <<  At(1,f) << std::endl;
//		 }

		 AtA.Multiply(At,A,false);

//		 for (int f = 0; f < 2; f++) {
//			 std::cout << "AtA(0,f) = " << AtA(0,f) << ",  AtA(1,f) = " <<  AtA(1,f) << std::endl;
//		 }

		 Atb.Multiply(At,b,false);

//		 for (int f = 0; f < 2; f++) {
//			 std::cout << "Atb(f,0) = " << Atb(f,0) << std::endl;
//		 }

		 AtA.Inverse();

		 InvAtA = AtA;

//		 for (int f = 0; f < 2; f++) {
//			 std::cout << "InvAtA(0,f) = " << InvAtA(0,f) << ",  InvAtA(1,f) = " <<  InvAtA(1,f) << std::endl;
//		 }

		 dudx.Multiply(InvAtA,Atb,false);

//		 std::cout << field_key_ << ", dudx     = " << dudx(0,0) << " " << dudx(1,0) << std::endl;

		 BJ_lim(dudx,dudx_lim,c,field_key_);

//		 std::cout << field_key_ << ", dudx_lim = " << dudx_lim(0,0) << " " << dudx_lim(1,0) << std::endl;

		 double h_rec = h_vec_c[0][c] + dudx_lim(0,0)*(x-xc[0]) + dudx_lim(1,0)*(y-xc[1]);

         return h_rec;

    }

    //--------------------------------------------------------------
    // physical flux in x-direction
    //--------------------------------------------------------------
    std::vector<double> ShallowWater_PK::PhysFlux_x(std::vector<double> U) {
        std::vector<double> F;
        
        F.resize(3);
        
        double h, u, v, qx, qy, g = 9.81;
        double eps = 1.e-6;
        
        // SW conservative variables: (h, hu, hv)
//        h = U[0];
//        u = U[1]/U[0];
//        v = U[2]/U[0];

		h  = U[0];
		qx = U[1];
		qy = U[2];
		u  = 2.*h*qx/(h*h + fmax(h*h,eps*eps));
		v  = 2.*h*qy/(h*h + fmax(h*h,eps*eps));
        
        // Form vector of x-fluxes F(U) = (hu, hu^2 + 1/2 gh^2, huv)
        
        F[0] = h*u;
        F[1] = h*u*u+0.5*g*h*h;
        F[2] = h*u*v;

        return F;
    }
    
    //--------------------------------------------------------------
    // physical flux in y-direction
    //--------------------------------------------------------------
    std::vector<double> ShallowWater_PK::PhysFlux_y(std::vector<double> U) {
        std::vector<double> G;

        G.resize(3);

        double h, u, v, g = 9.81;

        // SW conservative variables: (h, hu, hv)
        h = U[0];
        u = U[1]/U[0];
        v = U[2]/U[0];

        // Form vector of y-fluxes G(U) = (hv, huv, hv^2 + 1/2 gh^2)

        G[0] = h*v;
        G[1] = h*u*v;
        G[2] = h*v*v+0.5*g*h*h;

        return G;
    }

    //--------------------------------------------------------------
    // physical source term
    //--------------------------------------------------------------
    std::vector<double> ShallowWater_PK::PhysSrc(std::vector<double> U) {
		std::vector<double> S;

		S.resize(3);

		double h, u, v, qx, qy, g = 9.81;
		double eps = 1.e-6;

		// SW conservative variables: (h, hu, hv)
//		h = U[0];
//		u = U[1]/U[0];
//		v = U[2]/U[0];

		h  = U[0];
		qx = U[1];
		qy = U[2];
		u  = 2.*h*qx/(h*h + fmax(h*h,eps*eps));
		v  = 2.*h*qy/(h*h + fmax(h*h,eps*eps));

		// Form vector of sources Sr(U) = (0, -ghB_x, -ghB_y)

		double dBathx = 0.0, dBathy = 0.;

		S[0] = 0.;
		S[1] = -g*h*dBathx;
		S[2] = -g*h*dBathy;

		return S;
	}

    //--------------------------------------------------------------
    // numerical flux in x-direction
    // note that the SW system has a rotational invariance property
    //--------------------------------------------------------------
    std::vector<double> ShallowWater_PK::NumFlux_x(std::vector<double> UL,std::vector<double> UR) {

        return NumFlux_x_Rus(UL,UR);
//    	return NumFlux_x_central_upwind(UL,UR);

    }

//    //--------------------------------------------------------------
//    // numerical flux in y-direction
//    //--------------------------------------------------------------
//    std::vector<double> ShallowWater_PK::NumFlux_y(std::vector<double> UL,std::vector<double> UR) {
//        std::vector<double> GL, GR, G;
//
//        double hL, uL, vL, hR, uR, vR, g = 9.81;
//
//		// SW conservative variables: (h, hu, hv)
//		hL = UL[0];
//		uL = UL[1]/UL[0];
//		vL = UL[2]/UL[0];
//
//		hR = UR[0];
//		uR = UR[1]/UR[0];
//		vR = UR[2]/UR[0];
//
//        G.resize(3);
//
//        GL = PhysFlux_y(UL);
//        GR = PhysFlux_y(UR);
//
//        double SL, SR, Smax;
//
//        SL = std::max(uL + std::sqrt(g*hL),vL + std::sqrt(g*hL));
//        SR = std::max(uR + std::sqrt(g*hR),vR + std::sqrt(g*hR));
//
//        Smax = std::max(SL,SR);
//
//        for (int i = 0; i < 3; i++) {
//            G[i] = 0.5*(GL[i]+GR[i]) - 0.5*Smax*(UR[i]-UL[i]);
//        }
//
//        return G;
//    }

    //--------------------------------------------------------------
    // Rusanov numerical flux -- very simple but very diffusive
    //--------------------------------------------------------------
    std::vector<double> ShallowWater_PK::NumFlux_x_Rus(std::vector<double> UL,std::vector<double> UR) {
        std::vector<double> FL, FR, F;

        double hL, uL, vL, hR, uR, vR, qxL, qyL, qxR, qyR, g = 9.81;
        double eps = 1.e-6;

//		// SW conservative variables: (h, hu, hv)
//		hL = UL[0];
//		uL = UL[1]/UL[0];
//		vL = UL[2]/UL[0];
//
//		hR = UR[0];
//		uR = UR[1]/UR[0];
//		vR = UR[2]/UR[0];

		hL  = UL[0];
        qxL = UL[1];
		qyL = UL[2];
		uL  = 2.*hL*qxL/(hL*hL + fmax(hL*hL,eps*eps));
		vL  = 2.*hL*qyL/(hL*hL + fmax(hL*hL,eps*eps));

		hR  = UR[0];
		qxR = UR[1];
		qyR = UR[2];
		uR  = 2.*hR*qxR/(hR*hR + fmax(hR*hR,eps*eps));
		vR  = 2.*hR*qyR/(hR*hR + fmax(hR*hR,eps*eps));

        F.resize(3);

        FL = PhysFlux_x(UL);
        FR = PhysFlux_x(UR);

        double SL, SR, Smax;

        SL = std::max(std::fabs(uL) + std::sqrt(g*hL),std::fabs(vL) + std::sqrt(g*hL));
        SR = std::max(std::fabs(uR) + std::sqrt(g*hR),std::fabs(vR) + std::sqrt(g*hR));

//        double apx = std::max(std::max(std::fabs(uL)+std::sqrt(g*hL),std::fabs(uR)+std::sqrt(g*hR)),0.);
//        double apy = std::max(std::max(std::fabs(vL)+std::sqrt(g*hL),std::fabs(vR)+std::sqrt(g*hR)),0.);
//        double ap  = std::max(apx,apy);
//
//        double amx = std::min(std::min(std::fabs(uL)-std::sqrt(g*hL),std::fabs(uR)-std::sqrt(g*hR)),0.);
//        double amy = std::min(std::min(std::fabs(vL)-std::sqrt(g*hL),std::fabs(vR)-std::sqrt(g*hR)),0.);
//        double am  = std::min(amx,amy);
//
//        SL = am;
//        SR = ap;

        Smax = std::max(SL,SR);

        for (int i = 0; i < 3; i++) {
            F[i] = 0.5*(FL[i]+FR[i]) - 0.5*Smax*(UR[i]-UL[i]);
        }

//        std::cout << "F : " << F[0] << " " << F[1] << " " << F[2] << std::endl;

        return F;
    }

    //--------------------------------------------------------------
    // Central-upwind numerical flux (Kurganov, Acta Numerica 2018)
    //--------------------------------------------------------------
    std::vector<double> ShallowWater_PK::NumFlux_x_central_upwind(std::vector<double> UL,std::vector<double> UR) {
        std::vector<double> FL, FR, F, U_star, dU;

        double hL, uL, vL, hR, uR, vR, qxL, qyL, qxR, qyR, g = 9.81;
        double apx, amx, apy, amy;
        double ap, am;
        double eps = 1.e-6;

		// SW conservative variables: (h, hu, hv)
		hL = UL[0];
		uL = UL[1]/UL[0];
		vL = UL[2]/UL[0];

		hR = UR[0];
		uR = UR[1]/UR[0];
		vR = UR[2]/UR[0];

//		hL  = UL[0];
//      qxL = UL[1];
//		qyL = UL[2];
//		uL  = 2.*hL*qxL/(hL*hL + fmax(hL*hL,eps*eps));
//		vL  = 2.*hL*qyL/(hL*hL + fmax(hL*hL,eps*eps));
//
//		hR  = UR[0];
//		qxR = UR[1];
//		qyR = UR[2];
//		uR  = 2.*hR*qxR/(hR*hR + fmax(hR*hR,eps*eps));
//		vR  = 2.*hR*qyR/(hR*hR + fmax(hR*hR,eps*eps));

        apx = std::max(std::max(std::fabs(uL)+std::sqrt(g*hL),std::fabs(uR)+std::sqrt(g*hR)),0.);
        apy = std::max(std::max(std::fabs(vL)+std::sqrt(g*hL),std::fabs(vR)+std::sqrt(g*hR)),0.);
        ap  = std::max(apx,apy);

        amx = std::min(std::min(std::fabs(uL)-std::sqrt(g*hL),std::fabs(uR)-std::sqrt(g*hR)),0.);
        amy = std::min(std::min(std::fabs(vL)-std::sqrt(g*hL),std::fabs(vR)-std::sqrt(g*hR)),0.);
        am  = std::min(amx,amy);

//        std::cout << "UL: " << UL[0] << " " << UL[1] << " " << UL[2] << std::endl;
//        std::cout << "UR: " << UR[0] << " " << UR[1] << " " << UR[2] << std::endl;

        F.resize(3);

        FL = PhysFlux_x(UL);
        FR = PhysFlux_x(UR);

//        std::cout << "FL: " << FL[0] << " " << FL[1] << " " << FL[2] << std::endl;
//        std::cout << "FR: " << FR[0] << " " << FR[1] << " " << FR[2] << std::endl;

        U_star.resize(3);

        for (int i = 0; i < 3; i++) {
        	U_star[i] = ( ap*UR[i] - am*UL[i] - (FR[i] - FL[i]) ) / (ap - am + eps);
        }

//        std::cout << "ap = " << ap << ", am = " << am << std::endl;
//
//        std::cout << "U_star: " << U_star[0] << " " << U_star[1] << " " << U_star[2] << std::endl;

        dU.resize(3);

        for (int i = 0; i < 3; i++) {
//        	dU[i] = minmod(UR[i]-U_star[i],U_star[i]-UL[i]);
        	dU[i] = 0.;
        }

//        std::cout << "dU: " << dU[0] << " " << dU[1] << " " << dU[2] << std::endl;

        double Smax = std::max(am,ap);

//
//        if (0. < am) {
//        	for (int i = 0; i < 3; i++) F[i] = FL[i];
//        }
//        else {
//        	if (0. > ap) {
//        		for (int i = 0; i < 3; i++) F[i] = FR[i];
//        	}
//        	else {
//                for (int i = 0; i < 3; i++) {
//                    F[i] = ( ap*FL[i] - am*FR[i] + ap*am*(UR[i] - UL[i] - dU[i]) ) / (ap - am + eps);
//        //        	F[i] = 0.5*(FL[i]+FR[i]) - 0.5*Smax*(UR[i]-UL[i]);
//                }
//        	}
//        }

        for (int i = 0; i < 3; i++) {
            F[i] = ( ap*FL[i] - am*FR[i] + ap*am*(UR[i] - UL[i] - dU[i]) ) / (ap - am + eps);
//        	F[i] = 0.5*(FL[i]+FR[i]) - 0.5*Smax*(UR[i]-UL[i]);
        }

//        std::cout << "F : " << F[0] << " " << F[1] << " " << F[2] << std::endl;

        return F;
    }

//    //--------------------------------------------------------------
//    // discretization of the source term (not well-balanced)
//    //--------------------------------------------------------------
//    std::vector<double> ShallowWater_PK::NumSrc(std::vector<double> U, int c) {
//        std::vector<double> S;
//
//        S.resize(3);
//
//        S = PhysSrc(U);
//
//        return S;
//
//    }

    //--------------------------------------------------------------------
	// discretization of the source term (well-balanced for lake at rest)
	//--------------------------------------------------------------------
    std::vector<double> ShallowWater_PK::NumSrc(std::vector<double> U, int c) {
        std::vector<double> S;

        double g = 9.81;

        Epetra_MultiVector& B_vec_c  = *S_->GetFieldData("surface-bathymetry",passwd_)->ViewComponent("cell");
        Epetra_MultiVector& h_vec_c  = *S_->GetFieldData("surface-ponded_depth",passwd_)->ViewComponent("cell");
        Epetra_MultiVector& ht_vec_c = *S_->GetFieldData("surface-total_depth",passwd_)->ViewComponent("cell");

//        double BRx, BLx, BRy, BLy;

        S.resize(3);

        double h = h_vec_c[0][c];

        Amanzi::AmanziMesh::Entity_ID_List cfaces;

        mesh_->cell_get_faces(c,&cfaces,true);

        // cell volume
        double vol = mesh_->cell_volume(c);

        double S1, S2;

        S1 = 0.;
        S2 = 0.;

        for (int f = 0; f < cfaces.size(); f++) {

        	// face area
		    double farea = mesh_->face_area(cfaces[f]);

		    // normal
		    Amanzi::AmanziGeometry::Point normal(2);
		    int orientation;
        	normal = mesh_->face_normal(cfaces[f],false,c,&orientation);
//        	normal *= orientation;
        	normal /= farea;

        	// face centroid
        	Amanzi::AmanziGeometry::Point xcf = mesh_->face_centroid(cfaces[f]);

//        	double B = Bathymetry(xcf[0],xcf[1]);

        	double h_rec  = Reconstruction(xcf[0],xcf[1],c,"surface-ponded_depth");
        	double ht_rec = Reconstruction(xcf[0],xcf[1],c,"surface-total_depth");
        	double B_rec  = Reconstruction(xcf[0],xcf[1],c,"surface-bathymetry");

        	if (ht_rec < B_rec) {
				ht_rec = ht_vec_c[0][c];
				B_rec  = B_vec_c[0][c];
			}

//        	S1 += (-2.*B*h - B*B)*normal[0]*farea;
//        	S2 += (-2.*B*h - B*B)*normal[1]*farea;

//        	S1 += (-2.*B_rec*h_rec - B_rec*B_rec)*normal[0]*farea;
//        	S2 += (-2.*B_rec*h_rec - B_rec*B_rec)*normal[1]*farea;

        	S1 += (-2.*B_rec*ht_rec + B_rec*B_rec)*normal[0]*farea;
        	S2 += (-2.*B_rec*ht_rec + B_rec*B_rec)*normal[1]*farea;

//        	S1 += B*normal[0]*farea;
//        	S2 += B*normal[1]*farea;

//        	if (B != B_rec) {
//        		std::cout << "B = " << B << ", B_rec = " << B_rec << ", c = " << c << std::endl;
//        	}

//        	S1 += B_rec*normal[0]*farea;
//        	S2 += B_rec*normal[1]*farea;

//        	S1 += h*h*normal[0]*farea;
//        	S2 += h*h*normal[1]*farea;

//        	h_rec = ht_rec - B_rec;
//
//        	S1 += h_rec*h_rec*normal[0]*farea;
//        	S2 += h_rec*h_rec*normal[1]*farea;

        }

//        S[0] = 0.;
//        S[1] = 0.5*g/vol*S1;
//        S[2] = 0.5*g/vol*S2;

//        S[0] = 0.;
//        S[1] = -g*h/vol*S1;
//        S[2] = -g*h/vol*S2;

//        std::cout << "S = " << S[1] << " " << S[2] << std::endl;
//
//        std::vector<double> Sphys;
//        Sphys.resize(3);
//        Sphys = PhysSrc(U);
//
//        std::cout << "Sphys = " << Sphys[1] << " " << Sphys[2] << std::endl;

//        S = PhysSrc(U);

//        std::cout << "Sphys = " << S[0] << " " << S[1] << " " << S[2] << std::endl;

        return S;

    }



    //--------------------------------------------------------------
    // calculation of time step from CFL condition
    //--------------------------------------------------------------
    double ShallowWater_PK::get_dt() {

    	double h, u, v, g = 9.81;
		double S, Smax;
		double vol, dx, dx_min;
		double dt;
		double CFL = 0.1;
		double eps = 1.e-6;

    	Epetra_MultiVector& h_vec_c = *S_->GetFieldData("surface-ponded_depth",passwd_)->ViewComponent("cell");
		Epetra_MultiVector& vx_vec_c = *S_->GetFieldData("surface-velocity-x",passwd_)->ViewComponent("cell");
		Epetra_MultiVector& vy_vec_c = *S_->GetFieldData("surface-velocity-y",passwd_)->ViewComponent("cell");

		int ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::Parallel_type::OWNED);

		dt = 1.e10;
		for (int c = 0; c < ncells_owned; c++) {
			h = h_vec_c[0][c];
			u = vx_vec_c[0][c];
			v = vy_vec_c[0][c];
			S = std::max(std::fabs(u) + std::sqrt(g*h),std::fabs(v) + std::sqrt(g*h)) + eps;
			vol = mesh_->cell_volume(c);
			dx = std::sqrt(vol);
			dt = std::min(dt,dx/S);
		}

		Comm_ptr_type comm = Amanzi::getDefaultComm();
		int MyPID = comm->MyPID();

		double dt_min;

		mesh_->get_comm()->MinAll(&dt, &dt_min, 1);

//		dt_min = 0.0001;

		std::cout << "dt = " << dt << ", dt_min = " << dt_min << std::endl;

    	return CFL*dt_min;
    }

}  // namespace ShallowWater
}  // namespace Amanzi
