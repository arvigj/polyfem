#include "UIState.hpp"

#include "Mesh2D.hpp"
#include "Mesh3D.hpp"

#include "AssemblerUtils.hpp"

#include "ElasticProblem.hpp"

#include "LinearSolver.hpp"
#include "EdgeSampler.hpp"

#include <igl/per_face_normals.h>
#include <igl/per_corner_normals.h>
#include <igl/read_triangle_mesh.h>
#include <igl/triangle/triangulate.h>
#include <igl/copyleft/tetgen/tetrahedralize.h>
#include <igl/per_vertex_normals.h>
#include <igl/png/writePNG.h>
#include <igl/Timer.h>
#include <igl/serialize.h>

#ifdef IGL_VIEWER_WITH_NANOGUI
#include <nanogui/formhelper.h>
#include <nanogui/screen.h>
#endif

#include <cstdlib>
#include <fstream>


// ... or using a custom callback
  //       viewer_.ngui->addVariable<bool>("bool",[&](bool val) {
  //     boolVariable = val; // set
  // },[&]() {
  //     return boolVariable; // get
  // });


using namespace Eigen;

int offscreen_screenshot(igl::viewer::Viewer &viewer, const std::string &path);

void add_spheres(igl::viewer::Viewer &viewer0, const Eigen::MatrixXd &P, double radius) {
	Eigen::MatrixXd V = viewer0.data.V, VS, VN;
	Eigen::MatrixXi F = viewer0.data.F, FS;
	igl::read_triangle_mesh(POLYFEM_MESH_PATH "sphere.ply", VS, FS);

	Eigen::RowVector3d minV = VS.colwise().minCoeff();
	Eigen::RowVector3d maxV = VS.colwise().maxCoeff();
	VS.rowwise() -= minV + 0.5 * (maxV - minV);
	VS /= (maxV - minV).maxCoeff();
	VS *= 2.0 * radius;

	Eigen::MatrixXd C = viewer0.data.F_material_ambient.leftCols(3);
	C *= 10;

	int nv = V.rows();
	int nf = 0;
	V.conservativeResize(V.rows() + P.rows() * VS.rows(), V.cols());
	F.conservativeResize(nf + P.rows() * FS.rows(), F.cols());
	C.conservativeResize(C.rows() + P.rows() * FS.rows(), C.cols());
	for (int i = 0; i < P.rows(); ++i) {
		V.middleRows(nv, VS.rows()) = VS.rowwise() + P.row(i);
		F.middleRows(nf, FS.rows()) = FS.array() + nv;
		C.middleRows(nf, FS.rows()).rowwise() = Eigen::RowVector3d(142, 68, 173)/255.;
		nv += VS.rows();
		nf += FS.rows();
	}

	igl::per_corner_normals(V, F, 20.0, VN);

	C = Eigen::RowVector3d(142, 68, 173)/255.;

	igl::viewer::Viewer viewer;
	viewer.data.set_mesh(V, F);
	// viewer.data.add_points(P, Eigen::Vector3d(0,1,1).transpose());
	viewer.data.set_normals(VN);
	viewer.data.set_face_based(false);
	viewer.data.set_colors(C);
	viewer.data.lines = viewer0.data.lines;
	viewer.core.show_lines = false;
#ifndef __APPLE__
	viewer.core.line_width = 10;
#endif
	viewer.core.background_color.setOnes();
	viewer.core.set_rotation_type(igl::viewer::ViewerCore::RotationType::ROTATION_TYPE_TRACKBALL);

	#ifdef IGL_VIEWER_WITH_NANOGUI
	viewer.callback_init = [&](igl::viewer::Viewer& viewer_) {
		viewer_.ngui->addButton("Save screenshot", [&] {
			// Allocate temporary buffers
			Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> R(6400, 4000);
			Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> G(6400, 4000);
			Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> B(6400, 4000);
			Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> A(6400, 4000);

			// Draw the scene in the buffers
			viewer_.core.draw_buffer(viewer.data,viewer.opengl,false,R,G,B,A);
			A.setConstant(255);

			// Save it to a PNG
			igl::png::writePNG(R,G,B,A,"foo.png");
		});
		viewer_.ngui->addButton("Load", [&] {
			igl::deserialize(viewer.core, "core", "viewer.core");
		});
		viewer_.ngui->addButton("Save", [&] {
			igl::serialize(viewer.core, "core", "viewer.core");
		});
		viewer_.screen->performLayout();
		return false;
	};
	#endif

	viewer.launch();
}

namespace poly_fem
{
	void UIState::get_plot_edges(const Mesh &mesh, const std::vector< ElementBases > &bases, const int n_samples, const std::vector<bool> &valid_elements, Eigen::MatrixXd &pp0, Eigen::MatrixXd &pp1)
	{
		Eigen::MatrixXd samples, mapped, p0, p1, tmp;
		std::vector<Eigen::MatrixXd> p0v, p1v;

		std::vector<bool> valid_polytopes(valid_elements.size(), false);
		const int actual_dim = mesh.dimension();
		const int n_edges = mesh.is_volume() ? 12 : 4;

		if(mesh.is_volume())
			EdgeSampler::sample_3d(n_samples, samples);
		else
			EdgeSampler::sample_2d(n_samples, samples);

		for(std::size_t i = 0; i < bases.size(); ++i)
		{
			if(!valid_elements[i]) continue;

			if(mesh.is_polytope(i)) {
				valid_polytopes[i] = true;
				continue;
			}

			Eigen::MatrixXd result(samples.rows(), samples.cols());
			result.setZero();

			if(!state.problem->is_scalar() && current_visualization == Visualizing::Solution){
				const ElementBases &bs = bases[i];
				bs.evaluate_bases(samples, tmp);

				for(std::size_t j = 0; j < bs.bases.size(); ++j)
				{
					const Basis &b = bs.bases[j];

					for(std::size_t ii = 0; ii < b.global().size(); ++ii)
					{
						for(int d = 0; d < actual_dim; ++d)
						{
							result.col(d) += b.global()[ii].val * tmp.col(j) * state.sol(b.global()[ii].index*actual_dim + d);
						}
					}
				}
			}

			bases[i].eval_geom_mapping(samples, mapped);

			for(int j = 0; j < n_edges; ++j)
			{
				for(int k = 0; k < n_samples-1; ++k)
				{
					p0v.push_back(mapped.row(j*n_samples + k) + result.row(j*n_samples + k));
					p1v.push_back(mapped.row(j*n_samples + k+1) + result.row(j*n_samples + k+1));
				}
			}
		}

		mesh.get_edges(p0, p1, valid_polytopes);


		pp0.resize(p0.rows() + p0v.size(), mesh.dimension());
		pp1.resize(p1.rows() + p1v.size(), mesh.dimension());

		for(size_t i = 0; i < p1v.size(); ++i)
		{
			pp0.row(i) = p0v[i];
			pp1.row(i) = p1v[i];
		}

		pp0.bottomRows(p0.rows()) = p0;
		pp1.bottomRows(p1.rows()) = p1;
	}


	namespace
	{

		Navigation3D::Index current_3d_index;

		const std::vector<std::string> explode(const std::string &s, const char &c)
		{
			std::string buff{""};
			std::vector<std::string> v;

			for(auto n: s)
			{
				if(n != c) buff+=n; else
				if(n == c && buff != "") { v.push_back(buff); buff = ""; }
			}
			if(buff != "") v.push_back(buff);

			return v;
		}
	}

	void UIState::plot_selection_and_index(const bool recenter)
	{
		std::vector<bool> valid_elements(normalized_barycenter.rows(), false);
		for (auto idx : selected_elements) {
			valid_elements[idx] = true;
		}

		viewer.data.clear();

		if(current_visualization == Visualizing::InputMesh)
		{
			const long n_tris = show_clipped_elements(tri_pts, tri_faces, element_ranges, valid_elements, false, recenter);
			color_mesh(n_tris, valid_elements);
		}
		else
		{
			show_clipped_elements(vis_pts, vis_faces, vis_element_ranges, valid_elements, true, recenter);
		}

		// if(state.mesh->is_volume())
		// {
		// 	const auto p = state.mesh->point(current_3d_index.vertex);
		// 	const auto p1 = state.mesh->point(dynamic_cast<Mesh3D *>(state.mesh.get())->switch_vertex(current_3d_index).vertex);

		// 	viewer.data.add_points(p, MatrixXd::Zero(1, 3));
		// 	viewer.data.add_edges(p, p1, RowVector3d(1, 1, 0));

		// 	viewer.data.add_points(dynamic_cast<Mesh3D *>(state.mesh.get())->face_barycenter(current_3d_index.face), RowVector3d(1, 0, 0));
		// }
	}

	void UIState::color_mesh(const int n_tris, const std::vector<bool> &valid_elements)
	{
		const std::vector<ElementType> &ele_tag = state.mesh->elements_tag();

		Eigen::MatrixXd cols(n_tris, 3);
		cols.setZero();

		int from = 0;
		for(std::size_t i = 1; i < element_ranges.size(); ++i)
		{
			if(!valid_elements[i-1]) continue;

			const ElementType type = ele_tag[i-1];
			const int range = element_ranges[i]-element_ranges[i-1];

			switch(type)
			{
					//green
				case ElementType::RegularInteriorCube:
				// cols.block(from, 1, range, 1).setOnes(); break;
					//dark green
				case ElementType::RegularBoundaryCube:
				// cols.block(from, 1, range, 1).setConstant(0.5); break;
				cols.block(from, 0, range, 1).setConstant(30./255.);
				cols.block(from, 1, range, 1).setConstant(174./255.);
				cols.block(from, 2, range, 1).setConstant(96./255.); break;

					//yellow
				case ElementType::SimpleSingularInteriorCube:
				// cols.block(from, 0, range, 1).setOnes();
				// cols.block(from, 1, range, 1).setOnes(); break;

					//orange
				case ElementType::SimpleSingularBoundaryCube:
				// cols.block(from, 0, range, 1).setOnes();
				// cols.block(from, 1, range, 1).setConstant(0.5); break;

 					//red
				case ElementType::MultiSingularInteriorCube:
				// cols.block(from, 0, range, 1).setOnes(); break;

					//blue
				case ElementType::MultiSingularBoundaryCube:
				// cols.block(from, 2, range, 1).setConstant(0.6); break;

					//rhodamine
				case ElementType::InterfaceCube:
				// cols.middleRows(from, range).rowwise() = Eigen::RowVector3d(0.9, 0, 0.58); break;
				cols.block(from, 0, range, 1).setConstant(231./255.);
				cols.block(from, 1, range, 1).setConstant(76./255.);
				cols.block(from, 2, range, 1).setConstant(60./255.); break;

				  	//light blue
				case ElementType::BoundaryPolytope:
				case ElementType::InteriorPolytope:
				// cols.block(from, 2, range, 1).setOnes();
				// cols.block(from, 1, range, 1).setConstant(0.5); break;
				cols.block(from, 0, range, 1).setConstant(52./255.);
				cols.block(from, 1, range, 1).setConstant(152./255.);
				cols.block(from, 2, range, 1).setConstant(219./255.); break;

					//grey
				case ElementType::Undefined:
				cols.block(from, 0, range, 3).setConstant(0.5); break;
			}

			from += range;
		}

		viewer.data.set_colors(cols);

		if(!light_enabled){
			viewer.data.F_material_specular.setZero();
			viewer.data.V_material_specular.setZero();
			viewer.data.dirty |= igl::viewer::ViewerData::DIRTY_DIFFUSE;

			viewer.data.V_material_ambient *= 2;
			viewer.data.F_material_ambient *= 2;
		}
	}

	long UIState::clip_elements(const Eigen::MatrixXd &pts, const Eigen::MatrixXi &tris, const std::vector<int> &ranges, std::vector<bool> &valid_elements, const bool map_edges)
	{
		viewer.data.clear();

		valid_elements.resize(normalized_barycenter.rows());

		if(!is_slicing)
		{
			std::fill(valid_elements.begin(), valid_elements.end(), true);
			viewer.data.set_mesh(pts, tris);

			if(!light_enabled){
				viewer.data.F_material_specular.setZero();
				viewer.data.V_material_specular.setZero();
				viewer.data.V_material_ambient *= 4;
				viewer.data.F_material_ambient *= 4;

				viewer.data.dirty |= igl::viewer::ViewerData::DIRTY_DIFFUSE;
			}

			if(state.mesh->is_volume())
			{
				MatrixXd normals;
				igl::per_face_normals(pts, tris, normals);
				viewer.data.set_normals(normals);

				igl::per_corner_normals(pts, tris, 20, normals);
				viewer.data.set_normals(normals);
				viewer.data.set_face_based(false);
			}

			MatrixXd p0, p1;

			if(map_edges)
			{
				const auto &current_bases = state.iso_parametric ? state.bases : state.geom_bases;
				get_plot_edges(*state.mesh, current_bases, 20, valid_elements, p0, p1);
			}
			else
			{
				state.mesh->get_edges(p0, p1);
			}

#ifndef __APPLE__
			viewer.core.line_width = 10;
#endif
			viewer.data.add_edges(p0, p1, MatrixXd::Zero(1, 3));
			viewer.core.show_lines = false;

			return tris.rows();
		}

		for (long i = 0; i<normalized_barycenter.rows();++i)
			valid_elements[i] = normalized_barycenter(i, slice_coord) < slice_position;

		return show_clipped_elements(pts, tris, ranges, valid_elements, map_edges);
	}

	long UIState::show_clipped_elements(const Eigen::MatrixXd &pts, const Eigen::MatrixXi &tris, const std::vector<int> &ranges, const std::vector<bool> &valid_elements, const bool map_edges, const bool recenter)
	{
		viewer.data.set_face_based(false);

		int n_vis_valid_tri = 0;

		for (long i = 0; i<normalized_barycenter.rows();++i)
		{
			if(valid_elements[i])
				n_vis_valid_tri += ranges[i+1] - ranges[i];
		}

		MatrixXi valid_tri(n_vis_valid_tri, tris.cols());

		int from = 0;
		for(std::size_t i = 1; i < ranges.size(); ++i)
		{
			if(!valid_elements[i-1]) continue;

			const int range = ranges[i]-ranges[i-1];

			valid_tri.block(from, 0, range, tri_faces.cols()) = tris.block(ranges[i-1], 0, range, tris.cols());

			from += range;
		}

		viewer.data.set_mesh(pts, valid_tri);

		if(!light_enabled){
			viewer.data.F_material_specular.setZero();
			viewer.data.V_material_specular.setZero();
			viewer.data.dirty |= igl::viewer::ViewerData::DIRTY_DIFFUSE;
			viewer.data.V_material_ambient *= 2;
			viewer.data.F_material_ambient *= 2;
		}

		if(state.mesh->is_volume())
		{
			MatrixXd normals;
			igl::per_face_normals(pts, valid_tri, normals);
			viewer.data.set_normals(normals);

			igl::per_corner_normals(pts, valid_tri, 20, normals);
			viewer.data.set_normals(normals);
			viewer.data.set_face_based(false);
		}

		if(recenter)
			viewer.core.align_camera_center(pts, valid_tri);

		MatrixXd p0, p1;
		if(map_edges)
		{
			const auto &current_bases = state.iso_parametric ? state.bases : state.geom_bases;
			get_plot_edges(*state.mesh, current_bases, 20, valid_elements, p0, p1);
		}
		else
		{
			state.mesh->get_edges(p0, p1, valid_elements);
		}

		// std::cout<<p0<<std::endl;

#ifndef __APPLE__
		viewer.core.line_width = 10;
#endif
		viewer.data.add_edges(p0, p1, MatrixXd::Zero(1, 3));
		viewer.core.show_lines = false;

		return valid_tri.rows();
	}

	void UIState::interpolate_function(const MatrixXd &fun, MatrixXd &result)
	{
		MatrixXd tmp;

		int actual_dim = 1;
		if(!state.problem->is_scalar())
			actual_dim = state.mesh->dimension();

		result.resize(vis_pts.rows(), actual_dim);

		int index = 0;

		for(int i = 0; i < int(state.bases.size()); ++i)
		{
			const ElementBases &bs = state.bases[i];
			MatrixXd local_pts;

			if(state.mesh->is_simplicial())
				local_pts = local_vis_pts_tri;
			else if(state.mesh->is_cube(i))
				local_pts = local_vis_pts_quad;
			else
				local_pts = vis_pts_poly[i];

			MatrixXd local_res = MatrixXd::Zero(local_pts.rows(), actual_dim);
			bs.evaluate_bases(local_pts, tmp);
			for(std::size_t j = 0; j < bs.bases.size(); ++j)
			{
				const Basis &b = bs.bases[j];

				for(int d = 0; d < actual_dim; ++d)
				{
					for(std::size_t ii = 0; ii < b.global().size(); ++ii)
						local_res.col(d) += b.global()[ii].val * tmp.col(j) * fun(b.global()[ii].index*actual_dim + d);
				}
			}

			result.block(index, 0, local_res.rows(), actual_dim) = local_res;
			index += local_res.rows();
		}
	}


	UIState::UIState()
	: state(State::state())
	{ }

	void UIState::plot_function(const MatrixXd &fun, double min, double max)
	{
		MatrixXd col;
		std::vector<bool> valid_elements;

		if(fun.cols() != 1)
		{
			// const MatrixXd ffun = (fun.array()*fun.array()).rowwise().sum().sqrt(); //norm of displacement, maybe replace with stress
			// const MatrixXd ffun = fun.col(1); //y component
			MatrixXd ffun(vis_pts.rows(), 1);

			int size = state.mesh->dimension();

			const auto &assembler = AssemblerUtils::instance();

			MatrixXd stresses;
			int counter = 0;
			for(int i = 0; i < int(state.bases.size()); ++i)
			{
				const ElementBases &bs = state.bases[i];
				MatrixXd local_pts;

				if(state.mesh->is_simplicial())
					local_pts = local_vis_pts_tri;
				else if(state.mesh->is_cube(i))
					local_pts = local_vis_pts_quad;
				else
					local_pts = vis_pts_poly[i];

				assembler.compute_scalar_value(state.tensor_formulation, bs, local_pts, state.sol, stresses);

				ffun.block(counter, 0, stresses.rows(), 1) = stresses;
				counter += stresses.rows();
			}

			if(min < max)
				igl::colormap(color_map, ffun, min, max, col);
			else
				igl::colormap(color_map, ffun, true, col);

			std::cout<<"min/max "<< ffun.minCoeff()<<"/"<<ffun.maxCoeff()<<std::endl;

			MatrixXd tmp = vis_pts;

			for(long i = 0; i < fun.cols(); ++i) //apply displacement
				tmp.col(i) += fun.col(i);

			clip_elements(tmp, vis_faces, vis_element_ranges, valid_elements, true);
		}
		else
		{

			if(min < max)
				igl::colormap(color_map, fun, min, max, col);
			else
				igl::colormap(color_map, fun, true, col);

			if(state.mesh->is_volume())
				clip_elements(vis_pts, vis_faces, vis_element_ranges, valid_elements, true);
			else
			{
				MatrixXd tmp;
				tmp.resize(fun.rows(),3);
				tmp.col(0)=vis_pts.col(0);
				tmp.col(1)=vis_pts.col(1);
				// tmp.col(2)=fun;
				tmp.col(2).setZero();
				clip_elements(tmp, vis_faces, vis_element_ranges, valid_elements, true);
			}
		}

		viewer.data.set_colors(col);

		if(!light_enabled){
			viewer.data.F_material_specular.setZero();
			viewer.data.V_material_specular.setZero();

			viewer.data.V_material_ambient *= 2;
			viewer.data.F_material_ambient *= 2;
			viewer.data.dirty |= igl::viewer::ViewerData::DIRTY_DIFFUSE;
		}
	}


	UIState &UIState::ui_state(){
		static UIState instance;

		return instance;
	}

	void UIState::clear()
	{
		viewer.data.clear();
	}

	void UIState::show_mesh()
	{
		if (!state.mesh) { return; }
		clear();
		current_visualization = Visualizing::InputMesh;

		std::vector<bool> valid_elements;
		const long n_tris = clip_elements(tri_pts, tri_faces, element_ranges, valid_elements, false);

		color_mesh(n_tris, valid_elements);

		// for(int i = 0; i < state.mesh->n_faces(); ++i)
		// {
		// 	MatrixXd p = state.mesh->face_barycenter(i);
		// 	viewer.data.add_label(p.transpose(), std::to_string(i));
		// }

		// for(int i = 0; i < state.mesh->n_cells(); ++i)
		// {
		// 	MatrixXd p = state.mesh->cell_barycenter(i);
		// 	viewer.data.add_label(p.transpose(), std::to_string(i));
		// }

		// for(int i = 0; i < state.mesh->n_vertices(); ++i)
		// {
		// 	const auto p = state.mesh->point(i);
		// 	viewer.data.add_label(p.transpose(), std::to_string(i));
		// }
	}

	void UIState::show_vis_mesh()
	{
		if (!state.mesh) { return; }
		clear();
		current_visualization = Visualizing::VisMesh;

		std::cout<<vis_faces.rows()<<" "<<vis_faces.cols()<<std::endl;
		std::vector<bool> valid_elements;
		clip_elements(vis_pts, vis_faces, vis_element_ranges, valid_elements, true);
		viewer.core.show_lines = true;
	}

	void UIState::show_nodes()
	{
		if (!state.mesh) { return; }
		if(state.n_bases > 4500) return;

		for(std::size_t i = 0; i < state.bases.size(); ++i)
		// size_t i = 6;
		{
			const ElementBases &basis = state.bases[i];
			Eigen::MatrixXd P(basis.bases.size(), 3);

			for(std::size_t j = 0; j < basis.bases.size(); ++j)
			{
				for(std::size_t kk = 0; kk < basis.bases[j].global().size(); ++kk)
				{
					const Local2Global &l2g = basis.bases[j].global()[kk];
					int g_index = l2g.index;

					if(!state.problem->is_scalar())
						g_index *= state.mesh->dimension();

					MatrixXd node = l2g.node;
					MatrixXd col = MatrixXd::Zero(1, 3);

					if(std::find(state.boundary_nodes.begin(), state.boundary_nodes.end(), g_index) != state.boundary_nodes.end()){
						col.col(0).setOnes();
					}
					else{
						col(0) = 142./255.;
						col(1) = 68./255.;
						col(2) = 173./255.;
					}


					// P.row(j) = node;
					viewer.data.add_points(node, col);
					// viewer.data.add_label(node.transpose(), std::to_string(l2g.index));
				}
			}
			// add_spheres(viewer, P, 0.05);
		}
	}

	void UIState::show_rhs()
	{
		if (!state.mesh) { return; }
		current_visualization = Visualizing::Rhs;
		MatrixXd global_rhs;
		state.interpolate_function(state.rhs, local_vis_pts_quad, global_rhs);

		plot_function(global_rhs, 0, 1);
	}

	void UIState::show_error()
	{
		if (!state.mesh) { return; }
		if(state.sol.size() <= 0) {
			std::cerr<<"Solve the problem first!"<<std::endl;
			return;
		}
		if (!state.problem->has_exact_sol()) { return; }
		current_visualization = Visualizing::Error;
		MatrixXd global_sol;
		interpolate_function(state.sol, global_sol);

		MatrixXd exact_sol;
		state.problem->exact(state.formulation(), vis_pts, exact_sol);

		const MatrixXd err = (global_sol - exact_sol).eval().rowwise().norm();
		plot_function(err);
	}

	void UIState::show_basis()
	{
		if (!state.mesh) { return; }
		if(vis_basis < 0 || vis_basis >= state.n_bases) return;

		current_visualization = Visualizing::VisBasis;

		MatrixXd fun = MatrixXd::Zero(state.n_bases, 1);
		fun(vis_basis) = 1;

		MatrixXd global_fun;
		interpolate_function(fun, global_fun);
		// global_fun /= 10;


		std::cout<<global_fun.minCoeff()<<" "<<global_fun.maxCoeff()<<std::endl;
		plot_function(global_fun);
	}

	void UIState::show_sol()
	{
		if (!state.mesh) { return; }
		current_visualization = Visualizing::Solution;
		MatrixXd global_sol;
		interpolate_function(state.sol, global_sol);
		plot_function(global_sol);
	}

	void UIState::show_linear_reproduction()
	{
		auto ff = [](double x, double y) {return -0.1 + .3*x - .5*y;};

		MatrixXd fun = MatrixXd::Zero(state.n_bases, 1);

		for(std::size_t i = 0; i < state.bases.size(); ++i)
		{
			const ElementBases &basis = state.bases[i];
			if(!basis.has_parameterization) continue;
			for(std::size_t j = 0; j < basis.bases.size(); ++j)
			{
				for(std::size_t kk = 0; kk < basis.bases[j].global().size(); ++kk)
				{
					const Local2Global &l2g = basis.bases[j].global()[kk];
					const int g_index = l2g.index;

					const MatrixXd node = l2g.node;
					// std::cout<<node<<std::endl;
					fun(g_index) = ff(node(0),node(1));
				}
			}
		}

		MatrixXd tmp;
		interpolate_function(fun, tmp);

		MatrixXd exact_sol(vis_pts.rows(), 1);
		for(long i = 0; i < vis_pts.rows(); ++i)
			exact_sol(i) =  ff(vis_pts(i, 0),vis_pts(i, 1));

		const MatrixXd global_fun = (exact_sol - tmp).array().abs();

		std::cout<<global_fun.minCoeff()<<" "<<global_fun.maxCoeff()<<std::endl;
		plot_function(global_fun);
	}

	void UIState::show_quadratic_reproduction()
	{
		auto ff = [](double x, double y) {return -0.1 + .3*x - .5*y + .4*x*y - 0.9*y*y + 0.1*x*x;};

		MatrixXd fun = MatrixXd::Zero(state.n_bases, 1);

		for(std::size_t i = 0; i < state.bases.size(); ++i)
		{
			const ElementBases &basis = state.bases[i];
			if(!basis.has_parameterization) continue;
			for(std::size_t j = 0; j < basis.bases.size(); ++j)
			{
				for(std::size_t kk = 0; kk < basis.bases[j].global().size(); ++kk)
				{
					const Local2Global &l2g = basis.bases[j].global()[kk];
					const int g_index = l2g.index;

					const MatrixXd node = l2g.node;
					// std::cout<<node<<std::endl;
					fun(g_index) = ff(node(0),node(1));
				}
			}
		}

		MatrixXd tmp;
		interpolate_function(fun, tmp);

		MatrixXd exact_sol(vis_pts.rows(), 1);
		for(long i = 0; i < vis_pts.rows(); ++i)
			exact_sol(i) =  ff(vis_pts(i, 0),vis_pts(i, 1));

		const MatrixXd global_fun = (exact_sol - tmp).array().abs();

		std::cout<<global_fun.minCoeff()<<" "<<global_fun.maxCoeff()<<std::endl;
		plot_function(global_fun);
	}

	void UIState::build_vis_mesh()
	{
		if (!state.mesh) { return; }
		vis_element_ranges.clear();


		vis_faces_poly.clear();
		vis_pts_poly.clear();

		igl::Timer timer; timer.start();
		std::cout<<"Building vis mesh..."<<std::flush;

		const double area_param = 0.00001*state.mesh->n_elements();

		std::stringstream buf;
		buf.precision(100);
		buf.setf(std::ios::fixed, std::ios::floatfield);

		if(state.mesh->is_volume())
		{
			buf<<"Qpq1.414a"<<area_param;
			{
				MatrixXd pts(8,3); pts <<
				0, 0, 0,
				0, 1, 0,
				1, 1, 0,
				1, 0, 0,

				0, 0, 1, //4
				0, 1, 1,
				1, 1, 1,
				1, 0, 1;

				Eigen::MatrixXi faces(12,3); faces <<
				1, 2, 0,
				0, 2, 3,

				5, 4, 6,
				4, 7, 6,

				1, 0, 4,
				1, 4, 5,

				2, 1, 5,
				2, 5, 6,

				3, 2, 6,
				3, 6, 7,

				0, 3, 7,
				0, 7, 4;

				MatrixXi tets;
				igl::copyleft::tetgen::tetrahedralize(pts, faces, buf.str(), local_vis_pts_quad, tets, local_vis_faces_quad);
			}
			{
				MatrixXd pts(4,3); pts <<
				0, 0, 0,
				1, 0, 0,
				0, 1, 0,
				0, 0, 1;

				Eigen::MatrixXi faces(4,3); faces <<
				0, 1, 2,

				3, 1, 0,
				2, 1, 3,
				0, 2, 3;

				MatrixXi tets;
				igl::copyleft::tetgen::tetrahedralize(pts, faces, buf.str(), local_vis_pts_tri, tets, local_vis_faces_tri);
			}
		}
		else
		{
			buf<<"Qqa"<<area_param;
			{
				MatrixXd pts(4,2); pts <<
				0,0,
				0,1,
				1,1,
				1,0;

				MatrixXi E(4,2); E <<
				0,1,
				1,2,
				2,3,
				3,0;

				MatrixXd H(0,2);
				igl::triangle::triangulate(pts, E, H, buf.str(), local_vis_pts_quad, local_vis_faces_quad);
			}
			{
				MatrixXd pts(3,2); pts <<
				0,0,
				1,0,
				0,1;

				MatrixXi E(3,2); E <<
				0,1,
				1,2,
				2,0;

				igl::triangle::triangulate(pts, E, MatrixXd(0,2), buf.str(), local_vis_pts_tri, local_vis_faces_tri);
			}
		}

		const auto &current_bases = state.iso_parametric ? state.bases : state.geom_bases;
		int faces_total_size = 0, points_total_size = 0;
		vis_element_ranges.push_back(0);

		for(int i = 0; i < int(current_bases.size()); ++i)
		{
			const ElementBases &bs = current_bases[i];

			if(state.mesh->is_simplicial())
			{
				faces_total_size   += local_vis_faces_tri.rows();
				points_total_size += local_vis_pts_tri.rows();
			}
			else if(state.mesh->is_cube(i)){
				faces_total_size   += local_vis_faces_quad.rows();
				points_total_size += local_vis_pts_quad.rows();
			}
			else
			{
				if(state.mesh->is_volume())
				{
					vis_pts_poly[i] = state.polys_3d[i].first;
					vis_faces_poly[i] = state.polys_3d[i].second;

					faces_total_size   += vis_faces_poly[i].rows();
					points_total_size += vis_pts_poly[i].rows();
				}
				else
				{
					MatrixXd poly = state.polys[i];
					MatrixXi E(poly.rows(),2);
					for(int e = 0; e < int(poly.rows()); ++e)
					{
						E(e, 0) = e;
						E(e, 1) = (e+1) % poly.rows();
					}

					igl::triangle::triangulate(poly, E, MatrixXd(0,2), "Qpqa0.0001", vis_pts_poly[i], vis_faces_poly[i]);

					faces_total_size   += vis_faces_poly[i].rows();
					points_total_size += vis_pts_poly[i].rows();
				}
			}

			vis_element_ranges.push_back(faces_total_size);
		}

		vis_pts.resize(points_total_size, local_vis_pts_quad.cols());
		vis_faces.resize(faces_total_size, 3);

		MatrixXd mapped, tmp;
		int face_index = 0, point_index = 0;
		for(int i = 0; i < int(current_bases.size()); ++i)
		{
			const ElementBases &bs = current_bases[i];
			if(state.mesh->is_simplicial())
			{
				bs.eval_geom_mapping(local_vis_pts_tri, mapped);
				vis_faces.block(face_index, 0, local_vis_faces_tri.rows(), 3) = local_vis_faces_tri.array() + point_index;

				face_index += local_vis_faces_tri.rows();

				vis_pts.block(point_index, 0, mapped.rows(), mapped.cols()) = mapped;
				point_index += mapped.rows();
			}
			else if(state.mesh->is_cube(i))
			{
				bs.eval_geom_mapping(local_vis_pts_quad, mapped);
				vis_faces.block(face_index, 0, local_vis_faces_quad.rows(), 3) = local_vis_faces_quad.array() + point_index;
				face_index += local_vis_faces_quad.rows();

				vis_pts.block(point_index, 0, mapped.rows(), mapped.cols()) = mapped;
				point_index += mapped.rows();
			}
			else{
				bs.eval_geom_mapping(vis_pts_poly[i], mapped);
				vis_faces.block(face_index, 0, vis_faces_poly[i].rows(), 3) = vis_faces_poly[i].array() + point_index;

				face_index += vis_faces_poly[i].rows();

				vis_pts.block(point_index, 0, vis_pts_poly[i].rows(), vis_pts_poly[i].cols()) = mapped;
				point_index += mapped.rows();
			}
		}

		assert(point_index == vis_pts.rows());
		assert(face_index == vis_faces.rows());

		if(state.mesh->is_volume())
		{
			//reverse all faces
			for(long i = 0; i < vis_faces.rows(); ++i)
			{
				const int v0 = vis_faces(i, 0);
				const int v1 = vis_faces(i, 1);
				const int v2 = vis_faces(i, 2);

				int tmpc = vis_faces(i, 2);
				vis_faces(i, 2) = vis_faces(i, 1);
				vis_faces(i, 1) = tmpc;
			}
		}
		else
		{
			Matrix2d mmat;
			for(long i = 0; i < vis_faces.rows(); ++i)
			{
				const int v0 = vis_faces(i, 0);
				const int v1 = vis_faces(i, 1);
				const int v2 = vis_faces(i, 2);

				mmat.row(0) = vis_pts.row(v2) - vis_pts.row(v0);
				mmat.row(1) = vis_pts.row(v1) - vis_pts.row(v0);

				if(mmat.determinant() > 0)
				{
					int tmpc = vis_faces(i, 2);
					vis_faces(i, 2) = vis_faces(i, 1);
					vis_faces(i, 1) = tmpc;
				}
			}
		}

		timer.stop();
		std::cout<<" took "<<timer.getElapsedTime()<<"s"<<std::endl;

		if(skip_visualization) return;

		clear();
		show_vis_mesh();
	}

	void UIState::load_mesh()
	{
		if (state.mesh_path.empty()) { return; }
		element_ranges.clear();
		vis_element_ranges.clear();

		vis_faces_poly.clear();
		vis_pts_poly.clear();

		state.load_mesh();
		state.compute_mesh_stats();
		state.mesh->triangulate_faces(tri_faces, tri_pts, element_ranges);
		state.mesh->compute_element_barycenters(normalized_barycenter);

		// std::cout<<"normalized_barycenter\n"<<normalized_barycenter<<"\n\n"<<std::endl;
		for(long i = 0; i < normalized_barycenter.cols(); ++i){
			normalized_barycenter.col(i) = MatrixXd(normalized_barycenter.col(i).array() - normalized_barycenter.col(i).minCoeff());
			normalized_barycenter.col(i) /= normalized_barycenter.col(i).maxCoeff();
		}

		// std::cout<<"normalized_barycenter\n"<<normalized_barycenter<<"\n\n"<<std::endl;

		if(skip_visualization) return;

		if (!state.mesh->is_volume()) {
			light_enabled = false;
		}

		clear();
		show_mesh();
		viewer.core.align_camera_center(tri_pts);
	}

	void UIState::build_basis()
	{
		if (!state.mesh) { return; }
		state.build_basis();

		if(skip_visualization) return;
		clear();
		show_mesh();
		show_nodes();
	}

	void UIState::build_polygonal_basis()
	{
		if (!state.mesh) { return; }
		state.build_polygonal_basis();

		if(skip_visualization) return;
		// clear();
		// show_mesh();
		// show_quadrature();
	}

	void UIState::assemble_stiffness_mat() {
		if (!state.mesh) { return; }
		state.assemble_stiffness_mat();
	}

	void UIState::assemble_rhs()
	{
		if (!state.mesh) { return; }
		state.assemble_rhs();

		// std::cout<<state.rhs<<std::endl;

		// if(skip_visualization) return;
		// clear();
		// show_rhs();
	}

	void UIState::solve_problem()
	{
		if (!state.mesh) { return; }
		state.solve_problem();
		// state.solve_problem_old();

		if(skip_visualization) return;
		clear();
		show_sol();
	}

	void UIState::compute_errors()
	{
		if (!state.mesh) { return; }
		state.compute_errors();

		if(skip_visualization) return;
		clear();
		show_error();
	}

	void UIState::update_slices()
	{
		clear();
		switch(current_visualization)
		{
			case Visualizing::InputMesh: show_mesh(); break;
			case Visualizing::VisMesh: show_vis_mesh(); break;
			case Visualizing::Solution: show_sol(); break;
			case Visualizing::Rhs: break;
			case Visualizing::Error: show_error(); break;
			case Visualizing::VisBasis: show_basis(); break;
		}
	}

	void UIState::launch(const std::string &mesh_path, const int n_refs, const std::string &problem_name)
	{
		state.init(mesh_path, n_refs, problem_name);

		if(state.problem->boundary_ids().empty())
			std::fill(dirichlet_bc.begin(), dirichlet_bc.end(), true);
		else
			std::fill(dirichlet_bc.begin(), dirichlet_bc.end(), false);

		for(int i = 0; i < (int) state.problem->boundary_ids().size(); ++i)
			dirichlet_bc[state.problem->boundary_ids()[i]-1] = true;

		#if 0
		enum Foo : int { A=0 };

		viewer.callback_init = [&](igl::viewer::Viewer& viewer_)
		{
			#ifdef IGL_VIEWER_WITH_NANOGUI
			viewer_.ngui->addWindow(Eigen::Vector2i(220,10),"PolyFEM");

			viewer_.ngui->addGroup("Settings");

			viewer_.ngui->addVariable("quad order", state.quadrature_order);
			viewer_.ngui->addVariable("discr order", state.discr_order);
			viewer_.ngui->addVariable("b samples", state.n_boundary_samples);

			viewer_.ngui->addVariable("lambda", state.lambda);
			viewer_.ngui->addVariable("mu", state.mu);

			viewer_.ngui->addVariable("mesh path", state.mesh_path);
			viewer_.ngui->addButton("browse...", [&]() {
				std::string path = nanogui::file_dialog({
					{ "HYBRID", "General polyhedral mesh" }, { "OBJ", "Obj 2D mesh" }
				}, false);

				if (!path.empty())
					state.mesh_path = path;

			});
			viewer_.ngui->addVariable("n refs", state.n_refs);
			viewer_.ngui->addVariable("refinenemt t", state.refinenemt_location);

			viewer_.ngui->addVariable("spline basis", state.use_splines);
			viewer_.ngui->addVariable("fit nodes", state.fit_nodes);


			viewer_.ngui->addVariable<igl::ColorMapType>("Colormap", color_map)->setItems({"inferno", "jet", "magma", "parula", "plasma", "viridis"});

			// viewer_.ngui->addVariable<ProblemType>("Problem",
			// 	[&](ProblemType val) {
			// 		state.problem = Problem::get_problem(ProblemType(val));
			// 		if(state.problem->boundary_ids().empty())
			// 			std::fill(dirichlet_bc.begin(), dirichlet_bc.end(), true);
			// 		else
			// 			std::fill(dirichlet_bc.begin(), dirichlet_bc.end(), false);

			// 		for(int i = 0; i < state.problem->boundary_ids().size(); ++i)
			// 			dirichlet_bc[state.problem->boundary_ids()[i]-1] = true;
			// 	},
			// 	[&]() { return state.problem->problem_num(); }
			// )->setItems({"Linear","Quadratic","Franke", "Elastic", "Zero BC", "Franke3D", "ElasticExact"});


			auto solvers = LinearSolver::availableSolvers();
			if (state.solver_type.empty()) {
				state.solver_type = LinearSolver::defaultSolver();
			}
			viewer_.ngui->addVariable<Foo>("Solver",
				[&,solvers](Foo i) { state.solver_type = solvers[i]; },
				[&,solvers]() { return (Foo) std::distance(solvers.begin(),
					std::find(solvers.begin(), solvers.end(), state.solver_type)); }
				)->setItems(solvers);

			auto precond = LinearSolver::availablePrecond();
			if (state.precond_type.empty()) {
				state.precond_type = LinearSolver::defaultPrecond();
			}
			viewer_.ngui->addVariable<Foo>("Precond",
				[&,precond](Foo i) { state.precond_type = precond[i]; },
				[&,precond]() { return (Foo) std::distance(precond.begin(),
					std::find(precond.begin(), precond.end(), state.precond_type)); }
				)->setItems(precond);

			viewer_.ngui->addVariable("skip visualization", skip_visualization);

			viewer_.ngui->addGroup("Runners");
			viewer_.ngui->addButton("Load mesh", std::bind(&UIState::load_mesh, this));
			viewer_.ngui->addButton("Build  basis", std::bind(&UIState::build_basis, this));
			viewer_.ngui->addButton("Compute poly bases", std::bind(&UIState::build_polygonal_basis, this));
			viewer_.ngui->addButton("Build vis mesh", std::bind(&UIState::build_vis_mesh, this));

			viewer_.ngui->addButton("Assemble stiffness", std::bind(&UIState::assemble_stiffness_mat, this));
			viewer_.ngui->addButton("Assemble rhs", std::bind(&UIState::assemble_rhs, this));
			viewer_.ngui->addButton("Solve", std::bind(&UIState::solve_problem, this));
			viewer_.ngui->addButton("Compute errors", std::bind(&UIState::compute_errors, this));

			viewer_.ngui->addButton("Run all", [&](){
				load_mesh();
				build_basis();
				build_polygonal_basis();

				if(!skip_visualization)
					build_vis_mesh();

				assemble_stiffness_mat();
				assemble_rhs();
				solve_problem();
				compute_errors();
				state.save_json(std::cout);
			});

			viewer_.ngui->addWindow(Eigen::Vector2i(400,10),"Debug");
			viewer_.ngui->addButton("Clear", std::bind(&UIState::clear, this));
			viewer_.ngui->addButton("Show mesh", std::bind(&UIState::show_mesh, this));
			viewer_.ngui->addButton("Show vis mesh", std::bind(&UIState::show_vis_mesh, this));
			viewer_.ngui->addButton("Show nodes", std::bind(&UIState::show_nodes, this));
			// viewer_.ngui->addButton("Show quadrature", std::bind(&UIState::show_quadrature, this));
			viewer_.ngui->addButton("Show rhs", std::bind(&UIState::show_rhs, this));
			viewer_.ngui->addButton("Show sol", std::bind(&UIState::show_sol, this));
			viewer_.ngui->addButton("Show error", std::bind(&UIState::show_error, this));

			viewer_.ngui->addButton("Show linear r", std::bind(&UIState::show_linear_reproduction, this));
			viewer_.ngui->addButton("Show quadra r", std::bind(&UIState::show_quadratic_reproduction, this));

			viewer_.ngui->addVariable("basis num",vis_basis);
			viewer_.ngui->addButton("Show basis", std::bind(&UIState::show_basis, this));

			viewer_.ngui->addGroup("Slicing");
			viewer_.ngui->addVariable<int>("coord",[&](int val) {
				slice_coord = val;
				if(is_slicing)
					update_slices();
			},[&]() {
				return slice_coord;
			});
			viewer_.ngui->addVariable<float>("pos",[&](float val) {
				slice_position = val;
				if(is_slicing)
					update_slices();
			},[&]() {
				return slice_position;
			});

			viewer_.ngui->addButton("+0.1", [&](){ slice_position += 0.1; if(is_slicing) update_slices();});
			viewer_.ngui->addButton("-0.1", [&](){ slice_position -= 0.1; if(is_slicing) update_slices();});

			viewer_.ngui->addVariable<bool>("enable",[&](bool val) {
				is_slicing = val;
				update_slices();
			},[&]() {
				return is_slicing;
			});

			// viewer_.ngui->addGroup("Stats");
			// viewer_.ngui->addVariable("NNZ", Type &value)

			viewer_.ngui->addGroup("Selection");
			viewer_.ngui->addVariable("element ids", selected_elements);
			viewer_.ngui->addButton("Show", [&]{
				if(state.mesh->is_volume())
				{
					auto v{explode(selected_elements, ',')};
					current_3d_index = dynamic_cast<Mesh3D *>(state.mesh.get())->get_index_from_element(atoi(v.front().c_str()), 9, 0);
					std::cout<<"e:"<<current_3d_index.element<<" f:"<<current_3d_index.face<<" e:"<<current_3d_index.edge<<" v:"<<current_3d_index.vertex<<std::endl;
				}

				plot_selection_and_index(true);
			});

			viewer_.ngui->addButton("Switch vertex", [&]{
				if(state.mesh->is_volume())
				{
					current_3d_index = dynamic_cast<Mesh3D *>(state.mesh.get())->switch_vertex(current_3d_index);
					std::cout<<"e:"<<current_3d_index.element<<" f:"<<current_3d_index.face<<" e:"<<current_3d_index.edge<<" v:"<<current_3d_index.vertex<<std::endl;
				}

				plot_selection_and_index();
			});

			viewer_.ngui->addButton("Switch edge", [&]{
				if(state.mesh->is_volume())
				{
					current_3d_index = dynamic_cast<Mesh3D *>(state.mesh.get())->switch_edge(current_3d_index);
					std::cout<<"e:"<<current_3d_index.element<<" f:"<<current_3d_index.face<<" e:"<<current_3d_index.edge<<" v:"<<current_3d_index.vertex<<std::endl;
				}

				plot_selection_and_index();
			});

			viewer_.ngui->addButton("Switch face", [&]{
				if(state.mesh->is_volume())
				{
					current_3d_index = dynamic_cast<Mesh3D *>(state.mesh.get())->switch_face(current_3d_index);
					std::cout<<"e:"<<current_3d_index.element<<" f:"<<current_3d_index.face<<" e:"<<current_3d_index.edge<<" v:"<<current_3d_index.vertex<<std::endl;
				}

				plot_selection_and_index();
			});

			viewer_.ngui->addButton("Switch element", [&]{
				if(state.mesh->is_volume())
				{
					current_3d_index = dynamic_cast<Mesh3D *>(state.mesh.get())->switch_element(current_3d_index);
					selected_elements += ","+std::to_string(current_3d_index.element);
					std::cout<<"e:"<<current_3d_index.element<<" f:"<<current_3d_index.face<<" e:"<<current_3d_index.edge<<" v:"<<current_3d_index.vertex<<std::endl;
				}

				plot_selection_and_index();
			});

			viewer_.ngui->addButton("Save selection", [&]{
				if(state.mesh->is_volume())
				{
					auto v{explode(selected_elements, ',')};
					std::set<int> idx;
					for(auto s : v)
						idx.insert(atoi(s.c_str()));

					std::vector<int> idx_v(idx.begin(), idx.end());

					dynamic_cast<Mesh3D *>(state.mesh.get())->save(idx_v, 2, "mesh.HYBRID");
				}
			});

			viewer_.ngui->addWindow(Eigen::Vector2i(0,680),"Screenshot");

			// viewer_.ngui->addVariable<bool>("lighting enabled",[&](bool val) {
			// 	light_enabled = val;


			// },[&]() {
			// 	return light_enabled;
			// });

			viewer_.ngui->addButton("Save", [&]{
				// Allocate temporary buffers
				Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> R(6400, 4000);
				Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> G(6400, 4000);
				Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> B(6400, 4000);
				Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> A(6400, 4000);

    			// Draw the scene in the buffers
				viewer.core.draw_buffer(viewer.data,viewer.opengl,false,R,G,B,A);
				A.setConstant(255);

    			// Save it to a PNG
				std::string path = (screenshot.empty() ? "out.png" : screenshot);
				igl::png::writePNG(R,G,B,A,path);
			});



			viewer_.ngui->addWindow(Eigen::Vector2i(1000,0),"Elasticity BC");
			for(int local_id = 1; local_id <= 6; ++local_id)
			{
				viewer_.ngui->addVariable<bool>("Dirichlet " + std::to_string(local_id),
					[this,local_id](bool val) {
					dirichlet_bc[local_id-1] = val;

					auto &ids = state.problem->boundary_ids();
					ids.clear();
					for(int i=0; i < dirichlet_bc.size(); ++i)
					{
						if(dirichlet_bc[i])
							ids.push_back(i+1);
					}

				},[this,local_id]() {
					return dirichlet_bc[local_id-1];
				});
			}

			viewer_.screen->performLayout();

			#endif

			return false;
		};
		#endif

		viewer.core.background_color.setOnes();
		viewer.core.set_rotation_type(igl::viewer::ViewerCore::RotationType::ROTATION_TYPE_TRACKBALL);

		if (screenshot.empty()) {
			viewer.core.is_animating = true;
			viewer.plugins.push_back(this);
			viewer.launch();
		} else {
			load_mesh();
			offscreen_screenshot(viewer, screenshot);
		}
	}

	void UIState::sertialize(const std::string &name)
	{

	}

}

#include <GLFW/glfw3.h>

#ifndef __APPLE__
#  define GLEW_STATIC
#  include <GL/glew.h>
#endif

#ifdef __APPLE__
#   include <OpenGL/gl3.h>
#   define __gl_h_ /* Prevent inclusion of the old gl.h */
#else
#   include <GL/gl.h>
#endif

namespace {

	static void my_glfw_error_callback(int error, const char* description)
	{
		fputs(description, stderr);
		fputs("\n", stderr);
	}

}

int offscreen_screenshot(igl::viewer::Viewer &viewer, const std::string &path) {
	glfwSetErrorCallback(my_glfw_error_callback);
	if (!glfwInit()) {
		std::cout << "init failure" << std::endl;
		return EXIT_FAILURE;
	}

	// glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_OSMESA_CONTEXT_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

	glfwWindowHint(GLFW_SAMPLES, 8);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    #ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif

	printf("create window\n");
	GLFWwindow* offscreen_context = glfwCreateWindow(640, 480, "", NULL, NULL);
	printf("create context\n");
	glfwMakeContextCurrent(offscreen_context);
    #ifndef __APPLE__
	glewExperimental = true;
	GLenum err = glewInit();
	if(GLEW_OK != err)
	{
        /* Problem: glewInit failed, something is seriously wrong. */
		fprintf(stderr, "Error: %s\n", glewGetErrorString(err));
	}
      glGetError(); // pull and savely ignonre unhandled errors like GL_INVALID_ENUM
      fprintf(stdout, "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION));
    #endif
      viewer.opengl.init();
      viewer.core.align_camera_center(viewer.data.V, viewer.data.F);
      viewer.init();

      Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> R(6400, 4000);
      Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> G(6400, 4000);
      Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> B(6400, 4000);
      Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> A(6400, 4000);

    // Draw the scene in the buffers
      viewer.core.draw_buffer(viewer.data,viewer.opengl,true,R,G,B,A);
      A.setConstant(255);

    // Save it to a PNG
      igl::png::writePNG(R,G,B,A, path);

      return 0;
  }

