#ifndef AMANZI_OBSERVABLESOLUTE_HH
#define AMANZI_OBSERVABLESOLUTE_HH


#include "Observable.hh"

namespace Amanzi{


  class ObservableSolute : public virtual Observable{
  public:
    ObservableSolute(std::string variable,
                     std::string region,
                     std::string functional,
                     Teuchos::ParameterList& plist,
                     Teuchos::ParameterList& units_plist,
                     Teuchos::RCP<AmanziMesh::Mesh> mesh);

    virtual void ComputeObservation(State& S, double* value, double* volume);
    virtual int ComputeRegionSize();
    void RegisterComponentNames(std::vector<std::string> comp_names, int num_liquid, int tcc_index){
      comp_names_ = comp_names;
      num_liquid_ = num_liquid;
      tcc_index_ = tcc_index;
    }

  protected:
    bool obs_boundary_, obs_planar_;
    std::vector<double> vofs_;
    AmanziGeometry::Point reg_normal_;
    Teuchos::Array<std::string> comp_names_;
    int num_liquid_, tcc_index_;   
  };

  ObservableSolute::ObservableSolute(std::string variable,
                                     std::string region,
                                     std::string functional,
                                     Teuchos::ParameterList& plist,
                                     Teuchos::ParameterList& units_plist,
                                     Teuchos::RCP<AmanziMesh::Mesh> mesh):
    Observable(variable, region, functional, plist, units_plist, mesh) {};

  int ObservableSolute::ComputeRegionSize(){

    

    // check if observation is planar
    obs_planar_ = false;

    Teuchos::RCP<const AmanziGeometry::GeometricModel> gm_ptr = mesh_ -> geometric_model();
    Teuchos::RCP<const AmanziGeometry::Region> reg_ptr = gm_ptr->FindRegion(region_);
    
    if (reg_ptr->type() == AmanziGeometry::POLYGON) {
      Teuchos::RCP<const AmanziGeometry::RegionPolygon> poly_reg =
	Teuchos::rcp_static_cast<const AmanziGeometry::RegionPolygon>(reg_ptr);
      reg_normal_ = poly_reg->normal();
      obs_planar_ = true;
    } else if (reg_ptr->type() == AmanziGeometry::PLANE) {
      Teuchos::RCP<const AmanziGeometry::RegionPlane> plane_reg =
	Teuchos::rcp_static_cast<const AmanziGeometry::RegionPlane>(reg_ptr);
      reg_normal_ = plane_reg->normal();
      obs_planar_ = true;
    }

    std::string solute_var = comp_names_[tcc_index_] + " volumetric flow rate";

    if (variable_ == solute_var || variable_ == "aqueous mass flow rate" || variable_ == "aqueous volumetric flow rate") {  // flux needs faces
      region_size_ = mesh_->get_set_size(region_,
                                         Amanzi::AmanziMesh::FACE,
                                         Amanzi::AmanziMesh::OWNED);
      entity_ids_.resize(region_size_);
      mesh_->get_set_entities_and_vofs(region_,
                                       AmanziMesh::FACE, AmanziMesh::OWNED,
                                       &entity_ids_, 
                                       &vofs_);
      obs_boundary_ = true;
      for (int i = 0; i != region_size_; ++i) {
        int f = entity_ids_[i];
        Amanzi::AmanziMesh::Entity_ID_List cells;
        mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
        if (cells.size() == 2) {
          obs_boundary_ = false;
          break;
        }
      }
    } else { // all others need cells
      region_size_ = mesh_->get_set_size(region_,
                                            AmanziMesh::CELL, AmanziMesh::OWNED);    
      entity_ids_.resize(region_size_);
      mesh_->get_set_entities_and_vofs(region_,
                                       AmanziMesh::CELL, AmanziMesh::OWNED,
                                       &entity_ids_, &vofs_);
    }
      
    // find global meshblocksize
    int dummy = region_size_; 
    int global_mesh_block_size(0);
    mesh_->get_comm()->SumAll(&dummy, &global_mesh_block_size, 1);
      

    return global_mesh_block_size;

  }


  void ObservableSolute::ComputeObservation(State& S, double* value, double* volume){

    Errors::Message msg;
    int dim = mesh_ -> space_dimension();

    if (!S.HasField("total_component_concentration")) {  // bail out if this field is not yet created
      //Teuchos::OSTab tab = vo_->getOSTab();
      msg << "Field \"total_component_concentration\" does not exist, skipping it.";
      Exceptions::amanzi_throw(msg);
    }

    const Epetra_MultiVector& ws = *S.GetFieldData("saturation_liquid")->ViewComponent("cell");
    const Epetra_MultiVector& tcc = *S.GetFieldData("total_component_concentration")->ViewComponent("cell");
    const Epetra_MultiVector& porosity = *S.GetFieldData("porosity")->ViewComponent("cell");    

    if (variable_ == comp_names_[tcc_index_] + " aqueous concentration") { 
      for (int i = 0; i < region_size_; i++) {
        int c = entity_ids_[i];
        double factor = porosity[0][c] * ws[0][c] * mesh_->cell_volume(c);
        factor *= units_.concentration_factor();

        *value += tcc[tcc_index_][c] * factor;
        *volume += factor;
      }

    } else if (variable_ == comp_names_[tcc_index_] + " gaseous concentration") { 
      for (int i = 0; i < region_size_; i++) {
        int c = entity_ids_[i];
        double factor = porosity[0][c] * (1.0 - ws[0][c]) * mesh_->cell_volume(c);
        factor *= units_.concentration_factor();

        *value += tcc[tcc_index_][c] * factor;
        *volume += factor;
      }

    } else if (variable_ == comp_names_[tcc_index_] + " volumetric flow rate") {
      const Epetra_MultiVector& darcy_flux = *S.GetFieldData("darcy_flux")->ViewComponent("face");
      Amanzi::AmanziMesh::Entity_ID_List cells;

      if (obs_boundary_) { // observation is on a boundary set
        for (int i = 0; i != region_size_; ++i) {
          int f = entity_ids_[i];
          mesh_->face_get_cells(f, Amanzi::AmanziMesh::USED, &cells);

          int sign, c = cells[0];
          const AmanziGeometry::Point& face_normal = mesh_->face_normal(f, false, c, &sign);
          double area = mesh_->face_area(f);
          double factor = units_.concentration_factor();

          *value += std::max(0.0, sign * darcy_flux[0][f]) * tcc[tcc_index_][c] * factor;
          *volume += area * factor;
        }

      } else if (obs_planar_) {  // observation is on an interior planar set
        for (int i = 0; i != region_size_; ++i) {
          int f = entity_ids_[i];
          mesh_->face_get_cells(f, Amanzi::AmanziMesh::USED, &cells);

          int csign, c = cells[0];
          const AmanziGeometry::Point& face_normal = mesh_->face_normal(f, false, c, &csign);
          if (darcy_flux[0][f] * csign < 0) c = cells[1];

          double area = mesh_->face_area(f);
          double sign = (reg_normal_ * face_normal) * csign / area;
          double factor = units_.concentration_factor();
    
          *value += sign * darcy_flux[0][f] * tcc[tcc_index_][c] * factor;
          *volume += area * factor;
        }

      } else {
        msg << "Observations of \"SOLUTE volumetric flow rate\""
            << " is only possible for Polygon, Plane and Boundary side sets";
        Exceptions::amanzi_throw(msg);
      }
    } else {
      msg << "Cannot make an observation for solute variable \"" << variable_ << "\"";
      Exceptions::amanzi_throw(msg);
    }    
  }
 

}

#endif