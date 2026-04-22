#include <time.h>

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <algorithm>
#include <array>
#include <cmath>
#include <igl/arap.h>
#include <igl/lscm.h>
#include <limits>
#include <stdexcept>
#include <vector>

#include "GCore/Components.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "GCore/util_openmesh_bind.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"

/*
** @brief HW6_ARAP_Parameterization
**
** This file presents the basic framework of a "node", which processes inputs
** received from the left and outputs specific variables for downstream nodes to
** use.
**
** - In the first function, node_declare, you can set up the node's input and
** output variables.
**
** - The second function, node_exec is the execution part of the node, where we
** need to implement the node's functionality.
**
** - The third function generates the node's registration information, which
** eventually allows placing this node in the GUI interface.
**
** Your task is to fill in the required logic at the specified locations
** within this template, especially in node_exec.
*/

namespace Ruzino {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-8;

Eigen::Vector3d to_eigen(const OpenMesh::Vec3f& point)
{
    return Eigen::Vector3d(point[0], point[1], point[2]);
}

struct FaceData {
    std::array<int, 3> vertices;
    double area = 0.0;
    Eigen::Matrix2d d_inverse = Eigen::Matrix2d::Zero();
    std::array<Eigen::Vector2d, 3> gradients;
};

struct ReducedSystem {
    int vertex_count = 0;
    std::vector<int> free_vertices;
    std::vector<int> fixed_vertices;
    std::vector<int> vertex_to_free;
    std::vector<int> vertex_to_fixed;
    Eigen::SparseMatrix<double> free_free;
    Eigen::SparseMatrix<double> free_fixed;
};

using SparseSolver = Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>;

enum class ParameterizationMode {
    kARAP = 0,
    kASAP = 1,
    kHybrid = 2,
};

ParameterizationMode parse_parameterization_mode(int mode)
{
    switch (mode) {
    case 0:
        return ParameterizationMode::kARAP;
    case 1:
        return ParameterizationMode::kASAP;
    case 2:
        return ParameterizationMode::kHybrid;
    default:
        throw std::runtime_error(
            "Mode must be 0 (ARAP), 1 (ASAP), or 2 (Hybrid).");
    }
}

std::vector<int> extract_boundary_loop(const std::shared_ptr<PolyMesh>& mesh)
{
    const int vertex_count = static_cast<int>(mesh->n_vertices());
    std::vector<int> boundary_next(vertex_count, -1);

    for (const auto& halfedge_handle : mesh->halfedges()) {
        if (!halfedge_handle.is_boundary()) {
            continue;
        }

        const int from = halfedge_handle.from().idx();
        const int to = halfedge_handle.to().idx();
        if (boundary_next[from] != -1) {
            throw std::runtime_error(
                "HW6 parameterization expects a manifold boundary.");
        }
        boundary_next[from] = to;
    }

    std::vector<bool> visited(vertex_count, false);
    std::vector<std::vector<int>> loops;

    for (int start = 0; start < vertex_count; ++start) {
        if (boundary_next[start] < 0 || visited[start]) {
            continue;
        }

        std::vector<int> loop;
        int current = start;
        while (true) {
            if (current < 0 || current >= vertex_count) {
                throw std::runtime_error("Encountered an invalid boundary loop.");
            }
            if (visited[current]) {
                if (current != start) {
                    throw std::runtime_error(
                        "Encountered a non-simple boundary loop.");
                }
                break;
            }

            visited[current] = true;
            loop.push_back(current);
            current = boundary_next[current];

            if (current == start) {
                break;
            }
        }

        if (!loop.empty()) {
            loops.push_back(std::move(loop));
        }
    }

    if (loops.empty()) {
        throw std::runtime_error("HW6 parameterization needs a mesh boundary.");
    }

    if (loops.size() != 1) {
        throw std::runtime_error(
            "HW6 parameterization currently supports meshes with exactly one "
            "boundary loop.");
    }

    if (loops.front().size() < 3) {
        throw std::runtime_error("Boundary loop is degenerate.");
    }

    return loops.front();
}

std::vector<FaceData> precompute_face_data(const std::shared_ptr<PolyMesh>& mesh)
{
    std::vector<FaceData> faces;
    faces.reserve(mesh->n_faces());

    for (const auto& face_handle : mesh->faces()) {
        std::array<int, 3> vertices = { -1, -1, -1 };
        int local_index = 0;
        for (const auto& vertex_handle : face_handle.vertices()) {
            if (local_index >= 3) {
                throw std::runtime_error(
                    "HW6 parameterization expects triangular meshes.");
            }
            vertices[local_index++] = vertex_handle.idx();
        }

        if (local_index != 3) {
            throw std::runtime_error(
                "HW6 parameterization expects triangular meshes.");
        }

        const Eigen::Vector3d p0 =
            to_eigen(mesh->point(mesh->vertex_handle(vertices[0])));
        const Eigen::Vector3d p1 =
            to_eigen(mesh->point(mesh->vertex_handle(vertices[1])));
        const Eigen::Vector3d p2 =
            to_eigen(mesh->point(mesh->vertex_handle(vertices[2])));

        const Eigen::Vector3d edge01 = p1 - p0;
        const Eigen::Vector3d edge02 = p2 - p0;
        const double edge01_length = edge01.norm();
        const double area = 0.5 * edge01.cross(edge02).norm();

        if (edge01_length <= kEpsilon || area <= kEpsilon) {
            throw std::runtime_error(
                "Encountered a degenerate triangle in HW6 parameterization.");
        }

        const double x2 = edge02.dot(edge01) / edge01_length;
        const double y2_sq =
            std::max(0.0, edge02.squaredNorm() - x2 * x2);
        double y2 = std::sqrt(y2_sq);
        if (y2 <= kEpsilon) {
            y2 = 2.0 * area / edge01_length;
        }

        Eigen::Matrix2d d_matrix;
        d_matrix.col(0) = Eigen::Vector2d(edge01_length, 0.0);
        d_matrix.col(1) = Eigen::Vector2d(x2, y2);

        if (std::abs(d_matrix.determinant()) <= kEpsilon) {
            throw std::runtime_error(
                "Encountered an invalid local triangle basis.");
        }

        FaceData data;
        data.vertices = vertices;
        data.area = area;
        data.d_inverse = d_matrix.inverse();

        const Eigen::Matrix2d d_inverse_transpose =
            data.d_inverse.transpose();
        data.gradients[1] = d_inverse_transpose.col(0);
        data.gradients[2] = d_inverse_transpose.col(1);
        data.gradients[0] = -data.gradients[1] - data.gradients[2];

        faces.push_back(data);
    }

    return faces;
}

std::vector<std::vector<int>> build_vertex_adjacency(
    const std::shared_ptr<PolyMesh>& mesh)
{
    std::vector<std::vector<int>> adjacency(mesh->n_vertices());

    for (const auto& vertex_handle : mesh->vertices()) {
        auto& neighbors = adjacency[vertex_handle.idx()];
        for (const auto& halfedge_handle : vertex_handle.outgoing_halfedges()) {
            neighbors.push_back(halfedge_handle.to().idx());
        }
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(
            std::unique(neighbors.begin(), neighbors.end()),
            neighbors.end());
    }

    return adjacency;
}

Eigen::SparseMatrix<double> build_uniform_laplacian(
    const std::vector<std::vector<int>>& adjacency)
{
    const int vertex_count = static_cast<int>(adjacency.size());
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(vertex_count * 7);

    for (int vertex = 0; vertex < vertex_count; ++vertex) {
        const auto& neighbors = adjacency[vertex];
        triplets.emplace_back(vertex, vertex, double(neighbors.size()));
        for (int neighbor : neighbors) {
            triplets.emplace_back(vertex, neighbor, -1.0);
        }
    }

    Eigen::SparseMatrix<double> matrix(vertex_count, vertex_count);
    matrix.setFromTriplets(triplets.begin(), triplets.end());
    return matrix;
}

void export_triangle_mesh_to_eigen(
    const std::shared_ptr<PolyMesh>& mesh,
    Eigen::MatrixXd& vertices,
    Eigen::MatrixXi& faces)
{
    vertices.resize(mesh->n_vertices(), 3);
    for (const auto& vertex_handle : mesh->vertices()) {
        const auto& point = mesh->point(vertex_handle);
        vertices(vertex_handle.idx(), 0) = point[0];
        vertices(vertex_handle.idx(), 1) = point[1];
        vertices(vertex_handle.idx(), 2) = point[2];
    }

    faces.resize(mesh->n_faces(), 3);
    int face_index = 0;
    for (const auto& face_handle : mesh->faces()) {
        int local_index = 0;
        for (const auto& vertex_handle : face_handle.vertices()) {
            if (local_index >= 3) {
                throw std::runtime_error(
                    "HW6 parameterization expects triangular meshes.");
            }
            faces(face_index, local_index++) = vertex_handle.idx();
        }

        if (local_index != 3) {
            throw std::runtime_error(
                "HW6 parameterization expects triangular meshes.");
        }
        ++face_index;
    }
}

Eigen::SparseMatrix<double> build_stiffness_matrix(
    int vertex_count,
    const std::vector<FaceData>& faces)
{
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(faces.size() * 9);

    for (const auto& face : faces) {
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const double value =
                    face.area * face.gradients[row].dot(face.gradients[col]);
                triplets.emplace_back(
                    face.vertices[row], face.vertices[col], value);
            }
        }
    }

    Eigen::SparseMatrix<double> matrix(vertex_count, vertex_count);
    matrix.setFromTriplets(triplets.begin(), triplets.end());
    return matrix;
}

ReducedSystem build_reduced_system(
    const Eigen::SparseMatrix<double>& matrix,
    const std::vector<int>& fixed_vertices)
{
    if (matrix.rows() != matrix.cols()) {
        throw std::runtime_error("Reduced system requires a square matrix.");
    }

    ReducedSystem system;
    system.vertex_count = matrix.rows();
    system.fixed_vertices = fixed_vertices;
    system.vertex_to_free.assign(system.vertex_count, -1);
    system.vertex_to_fixed.assign(system.vertex_count, -1);

    for (int index = 0; index < int(fixed_vertices.size()); ++index) {
        const int vertex = fixed_vertices[index];
        if (vertex < 0 || vertex >= system.vertex_count) {
            throw std::runtime_error("Encountered an invalid fixed vertex.");
        }
        if (system.vertex_to_fixed[vertex] != -1) {
            throw std::runtime_error("Encountered duplicate fixed vertices.");
        }
        system.vertex_to_fixed[vertex] = index;
    }

    for (int vertex = 0; vertex < system.vertex_count; ++vertex) {
        if (system.vertex_to_fixed[vertex] != -1) {
            continue;
        }
        system.vertex_to_free[vertex] = int(system.free_vertices.size());
        system.free_vertices.push_back(vertex);
    }

    const int free_count = int(system.free_vertices.size());
    const int fixed_count = int(system.fixed_vertices.size());
    system.free_free.resize(free_count, free_count);
    system.free_fixed.resize(free_count, fixed_count);

    if (free_count == 0) {
        return system;
    }

    std::vector<Eigen::Triplet<double>> ff_triplets;
    std::vector<Eigen::Triplet<double>> fc_triplets;
    ff_triplets.reserve(matrix.nonZeros());
    fc_triplets.reserve(matrix.nonZeros());

    for (int outer = 0; outer < matrix.outerSize(); ++outer) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(matrix, outer); it;
             ++it) {
            const int free_row = system.vertex_to_free[it.row()];
            if (free_row < 0) {
                continue;
            }

            const int free_col = system.vertex_to_free[it.col()];
            if (free_col >= 0) {
                ff_triplets.emplace_back(free_row, free_col, it.value());
            }
            else {
                const int fixed_col = system.vertex_to_fixed[it.col()];
                fc_triplets.emplace_back(free_row, fixed_col, it.value());
            }
        }
    }

    system.free_free.setFromTriplets(ff_triplets.begin(), ff_triplets.end());
    system.free_fixed.setFromTriplets(fc_triplets.begin(), fc_triplets.end());

    return system;
}

Eigen::VectorXd solve_reduced_system(
    const ReducedSystem& system,
    const SparseSolver& solver,
    const Eigen::VectorXd& rhs,
    const Eigen::VectorXd& fixed_values)
{
    if (rhs.rows() != system.vertex_count) {
        throw std::runtime_error("RHS size does not match system size.");
    }
    if (fixed_values.rows() != int(system.fixed_vertices.size())) {
        throw std::runtime_error(
            "Fixed value count does not match fixed vertex count.");
    }

    Eigen::VectorXd solution =
        Eigen::VectorXd::Zero(system.vertex_count);

    for (int index = 0; index < int(system.fixed_vertices.size()); ++index) {
        solution(system.fixed_vertices[index]) = fixed_values(index);
    }

    if (system.free_vertices.empty()) {
        return solution;
    }

    Eigen::VectorXd reduced_rhs(system.free_vertices.size());
    for (int index = 0; index < int(system.free_vertices.size()); ++index) {
        reduced_rhs(index) = rhs(system.free_vertices[index]);
    }

    if (!system.fixed_vertices.empty()) {
        reduced_rhs -= system.free_fixed * fixed_values;
    }

    const Eigen::VectorXd free_solution = solver.solve(reduced_rhs);
    if (solver.info() != Eigen::Success) {
        throw std::runtime_error("Failed to solve the parameterization linear system.");
    }

    for (int index = 0; index < int(system.free_vertices.size()); ++index) {
        solution(system.free_vertices[index]) = free_solution(index);
    }

    return solution;
}

std::vector<Eigen::Vector2d> build_circle_boundary_map(
    const std::shared_ptr<PolyMesh>& mesh,
    const std::vector<int>& boundary_loop)
{
    std::vector<Eigen::Vector2d> uv(mesh->n_vertices(), Eigen::Vector2d::Zero());
    std::vector<double> edge_lengths(boundary_loop.size(), 0.0);

    double total_length = 0.0;
    for (int index = 0; index < int(boundary_loop.size()); ++index) {
        const int next_index = (index + 1) % int(boundary_loop.size());
        const Eigen::Vector3d current =
            to_eigen(mesh->point(mesh->vertex_handle(boundary_loop[index])));
        const Eigen::Vector3d next =
            to_eigen(mesh->point(mesh->vertex_handle(boundary_loop[next_index])));
        edge_lengths[index] = (next - current).norm();
        total_length += edge_lengths[index];
    }

    if (total_length <= kEpsilon) {
        throw std::runtime_error("Boundary loop length is too small.");
    }

    double accumulated_length = 0.0;
    for (int index = 0; index < int(boundary_loop.size()); ++index) {
        const double angle = 2.0 * kPi * accumulated_length / total_length;
        uv[boundary_loop[index]] = Eigen::Vector2d(
            0.5 + 0.5 * std::cos(angle),
            0.5 + 0.5 * std::sin(angle));
        accumulated_length += edge_lengths[index];
    }

    return uv;
}

std::vector<Eigen::Vector2d> compute_tutte_initialization(
    const std::shared_ptr<PolyMesh>& mesh,
    const std::vector<int>& boundary_loop)
{
    auto uv = build_circle_boundary_map(mesh, boundary_loop);
    if (boundary_loop.size() == mesh->n_vertices()) {
        return uv;
    }

    const auto adjacency = build_vertex_adjacency(mesh);
    const auto laplacian = build_uniform_laplacian(adjacency);
    const auto system = build_reduced_system(laplacian, boundary_loop);
    SparseSolver solver;
    if (!system.free_vertices.empty()) {
        solver.compute(system.free_free);
        if (solver.info() != Eigen::Success) {
            throw std::runtime_error(
                "Failed to factorize the initialization linear system.");
        }
    }

    Eigen::VectorXd fixed_u(boundary_loop.size());
    Eigen::VectorXd fixed_v(boundary_loop.size());
    for (int index = 0; index < int(boundary_loop.size()); ++index) {
        fixed_u(index) = uv[boundary_loop[index]].x();
        fixed_v(index) = uv[boundary_loop[index]].y();
    }

    const Eigen::VectorXd zero_rhs =
        Eigen::VectorXd::Zero(mesh->n_vertices());
    const Eigen::VectorXd solution_u =
        solve_reduced_system(system, solver, zero_rhs, fixed_u);
    const Eigen::VectorXd solution_v =
        solve_reduced_system(system, solver, zero_rhs, fixed_v);

    for (int vertex = 0; vertex < int(mesh->n_vertices()); ++vertex) {
        uv[vertex] = Eigen::Vector2d(solution_u(vertex), solution_v(vertex));
    }

    return uv;
}

Eigen::MatrixXd to_eigen_uv_matrix(const std::vector<Eigen::Vector2d>& uv)
{
    Eigen::MatrixXd uv_matrix(uv.size(), 2);
    for (int vertex = 0; vertex < int(uv.size()); ++vertex) {
        uv_matrix(vertex, 0) = uv[vertex].x();
        uv_matrix(vertex, 1) = uv[vertex].y();
    }
    return uv_matrix;
}

std::vector<Eigen::Vector2d> from_eigen_uv_matrix(const Eigen::MatrixXd& uv_matrix)
{
    std::vector<Eigen::Vector2d> uv(
        uv_matrix.rows(), Eigen::Vector2d::Zero());
    for (int vertex = 0; vertex < uv_matrix.rows(); ++vertex) {
        uv[vertex] = uv_matrix.row(vertex).transpose();
    }
    return uv;
}

std::vector<Eigen::Vector2d> compute_lscm_parameterization(
    const std::shared_ptr<PolyMesh>& mesh,
    const std::vector<int>& anchors)
{
    if (anchors.size() < 2) {
        throw std::runtime_error("LSCM parameterization needs two anchors.");
    }

    Eigen::MatrixXd vertices;
    Eigen::MatrixXi faces;
    export_triangle_mesh_to_eigen(mesh, vertices, faces);

    Eigen::VectorXi b(2);
    b << anchors[0], anchors[1];

    Eigen::MatrixXd bc(2, 2);
    bc << 0.0, 0.0, 1.0, 0.0;

    Eigen::MatrixXd uv_matrix;
    if (!igl::lscm(vertices, faces, b, bc, uv_matrix)) {
        throw std::runtime_error("Failed to solve the LSCM linear system.");
    }

    return from_eigen_uv_matrix(uv_matrix);
}

std::vector<Eigen::Vector2d> compute_libigl_arap_parameterization(
    const std::shared_ptr<PolyMesh>& mesh,
    const std::vector<int>& anchors,
    const std::vector<Eigen::Vector2d>& initial_uv,
    int iterations)
{
    if (anchors.size() < 2) {
        throw std::runtime_error("ARAP parameterization needs two anchors.");
    }

    Eigen::MatrixXd vertices;
    Eigen::MatrixXi faces;
    export_triangle_mesh_to_eigen(mesh, vertices, faces);

    Eigen::VectorXi b(anchors.size());
    Eigen::MatrixXd bc(anchors.size(), 2);
    for (int index = 0; index < int(anchors.size()); ++index) {
        b(index) = anchors[index];
        bc(index, 0) = initial_uv[anchors[index]].x();
        bc(index, 1) = initial_uv[anchors[index]].y();
    }

    Eigen::MatrixXd uv_matrix = to_eigen_uv_matrix(initial_uv);

    igl::ARAPData data;
    data.energy = igl::ARAP_ENERGY_TYPE_ELEMENTS;
    data.max_iter = std::max(1, iterations);

    if (!igl::arap_precomputation(vertices, faces, 2, b, data)) {
        throw std::runtime_error("Failed to precompute the ARAP system.");
    }
    if (!igl::arap_solve(bc, data, uv_matrix)) {
        throw std::runtime_error("Failed to solve the ARAP system.");
    }

    return from_eigen_uv_matrix(uv_matrix);
}

std::vector<int> choose_anchor_vertices(
    const std::vector<int>& boundary_loop,
    const std::vector<Eigen::Vector2d>& uv)
{
    if (boundary_loop.size() < 2) {
        throw std::runtime_error("Need at least two boundary vertices.");
    }

    std::vector<int> anchors = { boundary_loop.front(),
                                 boundary_loop[boundary_loop.size() / 2] };
    double best_distance = -1.0;

    for (int i = 0; i < int(boundary_loop.size()); ++i) {
        for (int j = i + 1; j < int(boundary_loop.size()); ++j) {
            const double distance =
                (uv[boundary_loop[i]] - uv[boundary_loop[j]]).squaredNorm();
            if (distance > best_distance) {
                best_distance = distance;
                anchors[0] = boundary_loop[i];
                anchors[1] = boundary_loop[j];
            }
        }
    }

    if (best_distance <= kEpsilon) {
        throw std::runtime_error("Failed to pick stable ARAP anchors.");
    }

    return anchors;
}

Eigen::Matrix2d closest_rotation(const Eigen::Matrix2d& jacobian)
{
    Eigen::JacobiSVD<Eigen::Matrix2d> svd(
        jacobian, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix2d rotation = svd.matrixU() * svd.matrixV().transpose();

    if (rotation.determinant() < 0.0) {
        Eigen::Matrix2d correction = Eigen::Matrix2d::Identity();
        correction(1, 1) = -1.0;
        rotation = svd.matrixU() * correction * svd.matrixV().transpose();
    }

    return rotation;
}

std::vector<double> solve_depressed_cubic_real_roots(double p, double q)
{
    std::vector<double> roots;
    const double discriminant =
        0.25 * q * q + (p * p * p) / 27.0;

    if (discriminant >= -kEpsilon) {
        const double clamped_discriminant = std::max(0.0, discriminant);
        const double sqrt_discriminant = std::sqrt(clamped_discriminant);
        roots.push_back(
            std::cbrt(-0.5 * q + sqrt_discriminant) +
            std::cbrt(-0.5 * q - sqrt_discriminant));
    }
    else {
        const double radius = 2.0 * std::sqrt(-p / 3.0);
        const double cosine_argument =
            std::clamp(
                (-0.5 * q) / std::sqrt(-(p * p * p) / 27.0), -1.0, 1.0);
        const double angle = std::acos(cosine_argument);

        roots.push_back(radius * std::cos(angle / 3.0));
        roots.push_back(radius * std::cos((angle + 2.0 * kPi) / 3.0));
        roots.push_back(radius * std::cos((angle + 4.0 * kPi) / 3.0));
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(
        std::unique(
            roots.begin(),
            roots.end(),
            [](double lhs, double rhs) { return std::abs(lhs - rhs) <= 1e-10; }),
        roots.end());

    return roots;
}

double evaluate_hybrid_face_energy(
    const Eigen::Matrix2d& jacobian,
    const Eigen::Matrix2d& rotation,
    double area,
    double lambda,
    double scale)
{
    const double trace_term = (rotation.transpose() * jacobian).trace();
    const double fitting_energy =
        area *
        (jacobian.squaredNorm() - 2.0 * scale * trace_term +
         2.0 * scale * scale);
    const double rigidity_energy =
        0.5 * lambda * std::pow(scale * scale - 1.0, 2.0);
    return fitting_energy + rigidity_energy;
}

double solve_hybrid_scale(
    const Eigen::Matrix2d& jacobian,
    const Eigen::Matrix2d& rotation,
    double area,
    double lambda)
{
    const double trace_term = (rotation.transpose() * jacobian).trace();
    if (lambda <= kEpsilon) {
        return 0.5 * trace_term;
    }

    const double p = (2.0 * area - lambda) / lambda;
    const double q = -area * trace_term / lambda;
    const auto roots = solve_depressed_cubic_real_roots(p, q);

    double best_scale = 0.5 * trace_term;
    double best_energy = std::numeric_limits<double>::infinity();

    for (double root : roots) {
        const double energy =
            evaluate_hybrid_face_energy(jacobian, rotation, area, lambda, root);
        if (energy < best_energy) {
            best_energy = energy;
            best_scale = root;
        }
    }

    return best_scale;
}

Eigen::Matrix2d compute_local_transform(
    const Eigen::Matrix2d& jacobian,
    double area,
    ParameterizationMode mode,
    double lambda)
{
    const Eigen::Matrix2d rotation = closest_rotation(jacobian);

    if (mode == ParameterizationMode::kARAP) {
        return rotation;
    }

    if (mode == ParameterizationMode::kASAP) {
        const double scale = 0.5 * (rotation.transpose() * jacobian).trace();
        return scale * rotation;
    }

    return solve_hybrid_scale(jacobian, rotation, area, lambda) * rotation;
}

void run_local_global_iterations(
    const std::vector<FaceData>& faces,
    const ReducedSystem& system,
    const SparseSolver& solver,
    const Eigen::VectorXd& fixed_u,
    const Eigen::VectorXd& fixed_v,
    int iterations,
    ParameterizationMode mode,
    double lambda,
    std::vector<Eigen::Vector2d>& uv)
{
    const int vertex_count = int(uv.size());
    Eigen::VectorXd rhs_u(vertex_count);
    Eigen::VectorXd rhs_v(vertex_count);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        rhs_u.setZero();
        rhs_v.setZero();

        for (const auto& face : faces) {
            const Eigen::Vector2d& u0 = uv[face.vertices[0]];
            const Eigen::Vector2d& u1 = uv[face.vertices[1]];
            const Eigen::Vector2d& u2 = uv[face.vertices[2]];

            Eigen::Matrix2d uv_edges;
            uv_edges.col(0) = u1 - u0;
            uv_edges.col(1) = u2 - u0;

            const Eigen::Matrix2d jacobian = uv_edges * face.d_inverse;
            const Eigen::Matrix2d local_transform =
                compute_local_transform(jacobian, face.area, mode, lambda);
            const Eigen::Vector2d transform_row_u =
                local_transform.row(0).transpose();
            const Eigen::Vector2d transform_row_v =
                local_transform.row(1).transpose();

            for (int local = 0; local < 3; ++local) {
                const int vertex = face.vertices[local];
                rhs_u(vertex) +=
                    face.area * face.gradients[local].dot(transform_row_u);
                rhs_v(vertex) +=
                    face.area * face.gradients[local].dot(transform_row_v);
            }
        }

        const Eigen::VectorXd solution_u =
            solve_reduced_system(system, solver, rhs_u, fixed_u);
        const Eigen::VectorXd solution_v =
            solve_reduced_system(system, solver, rhs_v, fixed_v);

        for (int vertex = 0; vertex < vertex_count; ++vertex) {
            uv[vertex] =
                Eigen::Vector2d(solution_u(vertex), solution_v(vertex));
        }
    }
}

void normalize_uvs(std::vector<Eigen::Vector2d>& uv)
{
    if (uv.empty()) {
        return;
    }

    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();

    for (const auto& coord : uv) {
        min_x = std::min(min_x, coord.x());
        max_x = std::max(max_x, coord.x());
        min_y = std::min(min_y, coord.y());
        max_y = std::max(max_y, coord.y());
    }

    const double width = max_x - min_x;
    const double height = max_y - min_y;
    const double span = std::max(width, height);
    if (!std::isfinite(span) || span <= kEpsilon) {
        return;
    }

    const double scale = 1.0 / span;
    const double x_padding = 0.5 * (1.0 - width * scale);
    const double y_padding = 0.5 * (1.0 - height * scale);

    for (auto& coord : uv) {
        coord.x() = (coord.x() - min_x) * scale + x_padding;
        coord.y() = (coord.y() - min_y) * scale + y_padding;
    }
}

std::vector<glm::vec2> to_glm_uvs(const std::vector<Eigen::Vector2d>& uv)
{
    std::vector<glm::vec2> result;
    result.reserve(uv.size());
    for (const auto& coord : uv) {
        result.emplace_back(float(coord.x()), float(coord.y()));
    }
    return result;
}

class HW6ParameterizationSolver {
public:
    explicit HW6ParameterizationSolver(std::shared_ptr<PolyMesh> mesh)
        : mesh_(std::move(mesh)),
          boundary_loop_(extract_boundary_loop(mesh_)),
          face_data_(precompute_face_data(mesh_))
    {
    }

    std::vector<glm::vec2> solve(
        ParameterizationMode mode,
        int iterations,
        double lambda) const
    {
        auto uv = compute_tutte_initialization(mesh_, boundary_loop_);
        const auto anchors = choose_anchor_vertices(boundary_loop_, uv);

        if (mode == ParameterizationMode::kASAP ||
            (mode == ParameterizationMode::kHybrid && lambda <= kEpsilon)) {
            uv = compute_lscm_parameterization(mesh_, anchors);
        }
        else if (mode == ParameterizationMode::kARAP) {
            uv = compute_libigl_arap_parameterization(
                mesh_, anchors, uv, iterations);
        }
        else {
            uv = solve_hybrid_parameterization(anchors, uv, iterations, lambda);
        }

        normalize_uvs(uv);
        return to_glm_uvs(uv);
    }

private:
    std::vector<Eigen::Vector2d> solve_hybrid_parameterization(
        const std::vector<int>& anchors,
        const std::vector<Eigen::Vector2d>& initial_uv,
        int iterations,
        double lambda) const
    {
        auto uv = initial_uv;
        const auto stiffness_matrix =
            build_stiffness_matrix(mesh_->n_vertices(), face_data_);
        const auto reduced_system =
            build_reduced_system(stiffness_matrix, anchors);

        SparseSolver solver;
        if (!reduced_system.free_vertices.empty()) {
            solver.compute(reduced_system.free_free);
            if (solver.info() != Eigen::Success) {
                throw std::runtime_error(
                    "Failed to factorize the Hybrid linear system.");
            }
        }

        Eigen::VectorXd fixed_u(anchors.size());
        Eigen::VectorXd fixed_v(anchors.size());
        for (int index = 0; index < int(anchors.size()); ++index) {
            fixed_u(index) = uv[anchors[index]].x();
            fixed_v(index) = uv[anchors[index]].y();
        }

        run_local_global_iterations(
            face_data_,
            reduced_system,
            solver,
            fixed_u,
            fixed_v,
            iterations,
            ParameterizationMode::kHybrid,
            lambda,
            uv);
        return uv;
    }

    std::shared_ptr<PolyMesh> mesh_;
    std::vector<int> boundary_loop_;
    std::vector<FaceData> face_data_;
};

}  // namespace
}  // namespace Ruzino

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(hw6_arap)
{
    // Input-1: Original 3D mesh with boundary
    b.add_input<Geometry>("Input");
    b.add_input<int>("Iterations").default_val(10).min(1).max(50);
    b.add_input<int>("Mode").default_val(0).min(0).max(2);
    b.add_input<float>("Lambda").default_val(0.001f).min(0.0f).max(10.0f);

    // Output-1: The UV coordinate of the mesh, provided by ARAP algorithm
    b.add_output<std::vector<glm::vec2>>("OutputUV");
}

NODE_EXECUTION_FUNCTION(hw6_arap)
{
    // Get the input from params
    auto input = params.get_input<Geometry>("Input");
    const int iterations = params.get_input<int>("Iterations");
    const auto mode = parse_parameterization_mode(params.get_input<int>("Mode"));
    const double lambda =
        std::max(0.0, double(params.get_input<float>("Lambda")));

    // Avoid processing the node when there is no input
    if (!input.get_component<MeshComponent>()) {
        throw std::runtime_error("Need Geometry Input.");
    }

    if (iterations < 1) {
        throw std::runtime_error("Iterations must be positive.");
    }

    /* ----------------------------- Preprocess -------------------------------
    ** Create a halfedge structure (using OpenMesh) for the input mesh. The
    ** half-edge data structure is a widely used data structure in geometric
    ** processing, offering convenient operations for traversing and modifying
    ** mesh elements.
    */
    auto halfedge_mesh = operand_to_openmesh(&input);
    if (halfedge_mesh->n_vertices() == 0 || halfedge_mesh->n_faces() == 0) {
        throw std::runtime_error("Input mesh is empty.");
    }

    /* -------------------- [HW6_TODO] Parameterization ------------------------
    ** Implement the homework 6 parameterization family:
    **
    ** - ARAP   (Mode = 0): local/global with rotational local fits
    ** - ASAP   (Mode = 1): LSCM-equivalent free-boundary conformal solve
    ** - Hybrid (Mode = 2): local/global with similarity local fits and lambda
    **                      controlling the ARAP/ASAP trade-off
    */
    const HW6ParameterizationSolver solver(halfedge_mesh);
    const std::vector<glm::vec2> uv_result =
        solver.solve(mode, iterations, lambda);

    // Set the output of the node
    params.set_output("OutputUV", uv_result);
    return true;
}

NODE_DECLARATION_UI(hw6_arap);
NODE_DEF_CLOSE_SCOPE
