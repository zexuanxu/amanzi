/*
  This is the mimetic discretization component of the Amanzi code. 

  Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Release name: ara-to.
  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <cmath>
#include <vector>

#include "Mesh.hh"
#include "Point.hh"
#include "errors.hh"

#include "DenseMatrix.hh"
#include "tensor.hh"
#include "mfd3d_electromagnetics.hh"

namespace Amanzi {
namespace WhetStone {

/* ******************************************************************
* Efficient implementation is possible in 2D. Hence, we fork the code.
****************************************************************** */
int MFD3D_Electromagnetics::L2consistency(int c, const Tensor& T,
                                          DenseMatrix& N, DenseMatrix& Mc)
{
  int ok, d = mesh_->space_dimension();
  if (d == 2) {
    ok = L2consistency2D_(c, T, N, Mc);
  } else {
    ok = L2consistency3D_(c, T, N, Mc);
  }

  return ok;
}


/* ******************************************************************
* Consistency condition for the mass matrix in electromagnetics.
* Only the upper triangular part of Mc = R (R^T N)^{-1} R^T is 
* calculated. Here R^T N = |c| T.
****************************************************************** */
int MFD3D_Electromagnetics::L2consistency2D_(int c, const Tensor& T,
                                             DenseMatrix& N, DenseMatrix& Mc)
{
  int d(2);
  Entity_ID_List faces;
  std::vector<int> dirs;

  mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
  int nfaces = faces.size();
  if (nfaces != N.NumRows()) return nfaces;  // matrix was not reshaped

  AmanziGeometry::Point v1(d), v2(d);
  const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
  double volume = mesh_->cell_volume(c);

  Tensor Tinv(T);
  Tinv.Inverse();
  if (Tinv.rank() == 2) {
    Tinv(0, 1) = -Tinv(0, 1);
    Tinv(1, 0) = -Tinv(1, 0);

    double tmp = Tinv(0, 0);
    Tinv(0, 0) = Tinv(1, 1);  
    Tinv(1, 1) = tmp;
  }

  for (int i = 0; i < nfaces; i++) {
    int f = faces[i];
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    double a1 = mesh_->face_area(f);
    v2 = Tinv * (xf - xc);

    for (int j = i; j < nfaces; j++) {
      f = faces[j];
      const AmanziGeometry::Point& yf = mesh_->face_centroid(f);
      double a2 = mesh_->face_area(f);

      v1 = yf - xc;
      Mc(i, j) = (v1 * v2) * (a1 * a2) / volume;
    }
  }

  // Rows of matrix N are oriented tangent vectors. Since N goes to 
  // the Gramm-Schmidt orthogonalizetion procedure, we can skip scaling
  // tensorial factor T.
  for (int i = 0; i < nfaces; i++) {
    int f = faces[i];
    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    double a = mesh_->face_area(f);

    for (int k = 0; k < d; k++) {
      a = -a;
      N(i, k) = normal[1 - k] * dirs[i] / a; 
    }
  }

  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Consistency condition for the mass matrix in electromagnetics.
* Only the upper triangular part of Mc = R (R^T N)^{-1} R^T is 
* calculated. Here R^T N = |c| T.
****************************************************************** */
int MFD3D_Electromagnetics::L2consistency3D_(int c, const Tensor& T,
                                             DenseMatrix& N, DenseMatrix& Mc)
{
  int n1, n2, d(3);
  Entity_ID_List edges, faces;
  std::vector<int> fdirs, edirs, map;

  mesh_->cell_get_faces_and_dirs(c, &faces, &fdirs);
  int nfaces = faces.size();

  AmanziGeometry::Point v1(d), v2(d), v3(d), tau(d), p1(d), p2(d);
  AmanziGeometry::Point vv[3];

  // To calculate matrix R, we re-use matrix N
  N.PutScalar(0.0);

  const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
  double volume = mesh_->cell_volume(c);

  for (int i = 0; i < nfaces; ++i) {
    int f = faces[i];
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    double area = mesh_->face_area(f);

    v1 = xc - xf; 
 
    double a1 = normal * v1;
    for (int k = 0; k < d; ++k) {
      vv[k].set(normal[k] * v1);
      vv[k][k] -= a1;
    }

    mesh_->face_get_edges_and_dirs(f, &edges, &edirs);
    int nedges = edges.size();

    mesh_->face_to_cell_edge_map(f, c, &map);

    for (int m = 0; m < nedges; ++m) {
      int e = edges[m];
      mesh_->edge_get_nodes(e, &n1, &n2);
      mesh_->node_get_coordinates(n1, &p1);
      mesh_->node_get_coordinates(n2, &p2);
 
      v3 = ((p1 + p2) / 2) - xf;

      double len = mesh_->edge_length(e);
      len /= 2.0 * area * area * fdirs[i] * edirs[m];

      for (int k = 0; k < d; ++k) {
        N(map[m], k) -= len * ((vv[k]^v3) * normal);
      }
    }
  }

  // calculate Mc = R (R^T N)^{-1} R^T 
  Tensor Tinv(T);
  Tinv.Inverse();

  mesh_->cell_get_edges(c, &edges);
  int nedges = edges.size();

  for (int i = 0; i < nedges; i++) {
    for (int k = 0; k < d; ++k) v1[k] = N(i, k);
    v2 = Tinv * v1;

    for (int j = i; j < nedges; j++) {
      for (int k = 0; k < d; ++k) v3[k] = N(j, k);
      Mc(i, j) = (v2 * v3) / volume;
    }
  }

  // Rows of matrix N are simply tangents. Since N goes to the
  // Gramm-Schmidt orthogonalizetion procedure, we can skip scaling
  // tensorial factor T.
  for (int i = 0; i < nedges; i++) {
    int e = edges[i];
    const AmanziGeometry::Point& tau = mesh_->edge_vector(e);
    double len = mesh_->edge_length(e);
    for (int k = 0; k < d; ++k) N(i, k) = tau[k] / len;
  }

  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Efficient implementation is possible in 2D. Hence, we fork the code.
****************************************************************** */
int MFD3D_Electromagnetics::L2consistencyInverse(
    int c, const Tensor& T, DenseMatrix& R, DenseMatrix& Wc)
{
  int ok, d = mesh_->space_dimension();
  if (d == 2) {
    ok = L2consistencyInverse2D_(c, T, R, Wc);
  } else {
    ok = L2consistencyInverse3D_(c, T, R, Wc);
  }

  return ok;
}


/* ******************************************************************
* Consistency condition for inverse of the mass matrix in the space
* of edge-based functions. Only the upper triangular part of matrix 
* Wc = N (N^T R)^{-1} N^T is calculated. Here N^T R = |c| T.
****************************************************************** */
int MFD3D_Electromagnetics::L2consistencyInverse2D_(
    int c, const Tensor& T, DenseMatrix& R, DenseMatrix& Wc)
{
  int d(2);
  Entity_ID_List faces;
  std::vector<int> dirs;

  mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
  int nfaces = faces.size();
  if (nfaces != R.NumRows()) return nfaces;  // matrix was not reshaped

  AmanziGeometry::Point v1(d);
  const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
  double volume = mesh_->cell_volume(c);

  // Since N is scaled by T, N = N0 * T, we use tensor T in the
  // inverse L2 consistency term.
  Tensor Trot(T);
  if (Trot.rank() == 2) {
    Trot(0, 1) = -Trot(0, 1);
    Trot(1, 0) = -Trot(1, 0);

    double tmp = Trot(0, 0);
    Trot(0, 0) = Trot(1, 1);  
    Trot(1, 1) = tmp;
  }

  for (int i = 0; i < nfaces; i++) {
    int f = faces[i];
    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    double a1 = mesh_->face_area(f);

    v1 = Trot * normal;

    for (int j = i; j < nfaces; j++) {
      f = faces[j];
      const AmanziGeometry::Point& v2 = mesh_->face_normal(f);
      double a2 = mesh_->face_area(f);
      Wc(i, j) = (v1 * v2) / (a1 * a2 * dirs[i] * dirs[j] * volume);
    }
  }

  // matrix R
  for (int i = 0; i < nfaces; i++) {
    int f = faces[i];
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    double len = mesh_->face_area(f);

    v1 = xf - xc;
    for (int k = 0; k < d; k++) {
      len = -len;
      R(i, k) = v1[1 - k] * len;
    }
  }

  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Consistency condition for inverse of the mass matrix in 
* electromagnetics. Only the upper triangular part of matrix 
* Wc = N (N^T R)^{-1} N^T is calculated. Here N^T R = |c| T.
****************************************************************** */
int MFD3D_Electromagnetics::L2consistencyInverse3D_(
    int c, const Tensor& T, DenseMatrix& R, DenseMatrix& Wc)
{
  int n1, n2, d(3);
  Entity_ID_List edges, faces;
  std::vector<int> fdirs, edirs, map;

  mesh_->cell_get_faces_and_dirs(c, &faces, &fdirs);
  int nfaces = faces.size();

  AmanziGeometry::Point v1(d), v2(d), v3(d), tau(d), p1(d), p2(d);
  AmanziGeometry::Point vv[3];

  // Since the matrix N is the matrix of scaled tangent vextors,
  // N = N0 T, we use the tensor T.
  const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
  double volume = mesh_->cell_volume(c);

  mesh_->cell_get_edges(c, &edges);
  int nedges = edges.size();

  for (int i = 0; i < nedges; i++) {
    int e1 = edges[i];
    const AmanziGeometry::Point& v1 = mesh_->edge_vector(e1);
    double a1 = mesh_->edge_length(e1);
    v3 = T * v1;

    for (int j = i; j < nedges; j++) {
      int e2 = edges[j];
      const AmanziGeometry::Point& v2 = mesh_->edge_vector(e2);
      double a2 = mesh_->edge_length(e2);
      Wc(i, j) = (v2 * v3) / (a1 * a2 * volume);
    }
  }

  // Calculate matrix R
  R.PutScalar(0.0);

  for (int i = 0; i < nfaces; ++i) {
    int f = faces[i];
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    double area = mesh_->face_area(f);

    v1 = xc - xf; 
 
    double a1 = normal * v1;
    for (int k = 0; k < d; ++k) {
      vv[k].set(normal[k] * v1);
      vv[k][k] -= a1;
    }

    mesh_->face_get_edges_and_dirs(f, &edges, &edirs);
    int nedges = edges.size();

    mesh_->face_to_cell_edge_map(f, c, &map);

    for (int m = 0; m < nedges; ++m) {
      int e = edges[m];
      mesh_->edge_get_nodes(e, &n1, &n2);
      mesh_->node_get_coordinates(n1, &p1);
      mesh_->node_get_coordinates(n2, &p2);
 
      v3 = ((p1 + p2) / 2) - xf;

      double len = mesh_->edge_length(e);
      len /= 2.0 * area * area * fdirs[i] * edirs[m];

      for (int k = 0; k < d; ++k) {
        R(map[m], k) -= len * ((vv[k]^v3) * normal);
      }
    }
  }

  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Efficient implementation is possible in 2D. Hence, we fork the code.
****************************************************************** */
int MFD3D_Electromagnetics::H1consistency(int c, const Tensor& T,
                                          DenseMatrix& N, DenseMatrix& Ac)
{
  int ok, d = mesh_->space_dimension();
  if (d == 2) {
    ok = H1consistency2D_(c, T, N, Ac);
  } else {
    ok = H1consistency3D_(c, T, N, Ac);
  }
  return ok;
}


/* ******************************************************************
* Stiffness matrix for edge-based discretization.
****************************************************************** */
int MFD3D_Electromagnetics::H1consistency2D_(int c, const Tensor& T,
                                             DenseMatrix& N, DenseMatrix& Ac)
{
  int d(2);
  Entity_ID_List faces;
  std::vector<int> fdirs;

  mesh_->cell_get_faces_and_dirs(c, &faces, &fdirs);
  int nfaces = faces.size();

  // calculate Ac = R (R^T N)^{+} R^T
  double T00 = T(0, 0);
  const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
  double volume = mesh_->cell_volume(c);

  for (int i = 0; i < nfaces; ++i) {
    int f = faces[i];
    double a1 = mesh_->face_area(f);

    for (int j = i; j < nfaces; ++j) {
      f = faces[j];
      double a2 = mesh_->face_area(f);
      Ac(i, j) = a1 * a2 / (T00 * fdirs[i] * fdirs[j] * volume);
    }
  }

  // Matrix N(:, 1:2) are simply tangents
  for (int i = 0; i < nfaces; i++) {
    int f = faces[i];
    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    double len = mesh_->face_area(f);

    for (int k = 0; k < d; ++k) {
      len = -len;
      N(i, k) = normal[1 - k] / len;
    }

    N(i, d) = (xf - xc) * normal / len; 
  }
}


/* ******************************************************************
* Stiffness matrix for edge-based discretization.
****************************************************************** */
int MFD3D_Electromagnetics::H1consistency3D_(int c, const Tensor& T,
                                             DenseMatrix& N, DenseMatrix& Ac)
{
  int n1, n2, d(3);

  Entity_ID_List edges, faces;
  std::vector<int> fdirs, edirs, map;

  mesh_->cell_get_faces_and_dirs(c, &faces, &fdirs);
  int nfaces = faces.size();

  // To calculate matrix R, we re-use matrix N
  N.PutScalar(0.0);

  AmanziGeometry::Point v1(d), v2(d), v3(d), p1(d), p2(d);

  const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
  double volume = mesh_->cell_volume(c);

  for (int i = 0; i < nfaces; ++i) {
    int f = faces[i];
    const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
    const AmanziGeometry::Point& normal = mesh_->face_normal(f);
    double area = mesh_->face_area(f);

    v1 = xc - xf; 
 
    mesh_->face_get_edges_and_dirs(f, &edges, &edirs);
    int nedges = edges.size();

    mesh_->face_to_cell_edge_map(f, c, &map);

    for (int m = 0; m < nedges; ++m) {
      int e = edges[m];

      double len = mesh_->edge_length(e);
      len *= 2 * fdirs[i] * edirs[m];

      for (int k = 0; k < d; ++k) {
        N(map[m], k) += len * v1[k];
      }
    }
  }
  
  // calculate Ac = R (R^T N)^{+} R^T
  mesh_->cell_get_edges(c, &edges);
  int nedges = edges.size();

  for (int i = 0; i < nedges; i++) {
    for (int k = 0; k < d; ++k) v1[k] = N(i, k);
    v2 = T * v1;

    for (int j = i; j < nedges; j++) {
      for (int k = 0; k < d; ++k) v3[k] = N(j, k);
      Ac(i, j) = (v2 * v3) / (4 * volume);
    }
  }

  // Matrix N(:, 1:3) are simply tangents
  for (int i = 0; i < nedges; i++) {
    int e = edges[i];
    const AmanziGeometry::Point& tau = mesh_->edge_vector(e);
    double len = mesh_->edge_length(e);

    for (int k = 0; k < d; ++k) N(i, k) = tau[k] / len;

    mesh_->edge_get_nodes(e, &n1, &n2);
    mesh_->node_get_coordinates(n1, &p1);
    mesh_->node_get_coordinates(n2, &p2);
 
    v1 = ((p1 + p2) / 2) - xc;
    v2 = v1^tau;

    for (int k = 0; k < d; ++k) {
      N(i, k + d) = v2[k] / len;
    }
  }

  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Matrix matrix for edge-based discretization.
****************************************************************** */
int MFD3D_Electromagnetics::MassMatrix(int c, const Tensor& T, DenseMatrix& M)
{
  int d = mesh_->space_dimension();
  int nrows = M.NumRows();

  DenseMatrix N(nrows, d);
  DenseMatrix Mc(nrows, nrows);

  int ok = L2consistency(c, T, N, Mc);
  if (ok) return WHETSTONE_ELEMENTAL_MATRIX_WRONG;

  StabilityScalar(c, N, Mc, M);
  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Mass matrix optimized for sparsity.
****************************************************************** */
int MFD3D_Electromagnetics::MassMatrixOptimized(int c, const Tensor& T, DenseMatrix& M)
{
  int d = mesh_->space_dimension();
  int nrows = M.NumRows();

  DenseMatrix N(nrows, d);
  DenseMatrix Mc(nrows, nrows);

  int ok = L2consistency(c, T, N, Mc);
  if (ok) return WHETSTONE_ELEMENTAL_MATRIX_WRONG;

  ok = StabilityOptimized(T, N, Mc, M);
  return ok;
}


/* ******************************************************************
* Inverse of the mass matrix for edge-based discretization.
****************************************************************** */
int MFD3D_Electromagnetics::MassMatrixInverse(int c, const Tensor& T, DenseMatrix& W)
{
  int d = mesh_->space_dimension();
  int nrows = W.NumRows();

  DenseMatrix R(nrows, d);
  DenseMatrix Wc(nrows, nrows);

  int ok = L2consistencyInverse(c, T, R, Wc);
  if (ok) return WHETSTONE_ELEMENTAL_MATRIX_WRONG;

  StabilityScalar(c, R, Wc, W);
  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Inverse of matrix matrix for edge-based discretization optimized
* for starcity.
****************************************************************** */
int MFD3D_Electromagnetics::MassMatrixInverseOptimized(
    int c, const Tensor& T, DenseMatrix& W)
{
  int d = mesh_->space_dimension();
  int nrows = W.NumRows();

  DenseMatrix R(nrows, d);
  DenseMatrix Wc(nrows, nrows);

  int ok = L2consistencyInverse(c, T, R, Wc);
  if (ok) return WHETSTONE_ELEMENTAL_MATRIX_WRONG;

  ok = StabilityOptimized(T, R, Wc, W);
  return ok;
}


/* ******************************************************************
* Stiffness matrix: the standard algorithm.
****************************************************************** */
int MFD3D_Electromagnetics::StiffnessMatrix(int c, const Tensor& T, DenseMatrix& A)
{
  int d = mesh_->space_dimension();
  int nedges = A.NumRows();

  int nd = d * (d + 1) / 2;
  DenseMatrix N(nedges, nd);
  DenseMatrix Ac(nedges, nedges);

  int ok = H1consistency(c, T, N, Ac);
  if (ok) return WHETSTONE_ELEMENTAL_MATRIX_WRONG;

  StabilityScalar(c, N, Ac, A);
  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* Stiffness matrix optimized for sparsity.
****************************************************************** */
int MFD3D_Electromagnetics::StiffnessMatrixOptimized(
    int c, const Tensor& T, DenseMatrix& A)
{
  int d = mesh_->space_dimension();
  int nedges = A.NumRows();

  int nd = d * (d + 1) / 2;
  DenseMatrix N(nedges, nd);
  DenseMatrix Ac(nedges, nedges);

  int ok = H1consistency(c, T, N, Ac);
  if (ok) return WHETSTONE_ELEMENTAL_MATRIX_WRONG;

  ok = StabilityOptimized(T, N, Ac, A);
  return ok;
}

}  // namespace WhetStone
}  // namespace Amanzi


