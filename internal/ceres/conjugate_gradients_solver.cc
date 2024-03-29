// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2010, 2011, 2012 Google Inc. All rights reserved.
// http://code.google.com/p/ceres-solver/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: sameeragarwal@google.com (Sameer Agarwal)
//
// A preconditioned conjugate gradients solver
// (ConjugateGradientsSolver) for positive semidefinite linear
// systems.
//
// We have also augmented the termination criterion used by this
// solver to support not just residual based termination but also
// termination based on decrease in the value of the quadratic model
// that CG optimizes.

#include "ceres/conjugate_gradients_solver.h"

#include <cmath>
#include <cstddef>
#include <glog/logging.h>
#include "ceres/fpclassify.h"
#include "ceres/linear_operator.h"
#include "ceres/internal/eigen.h"
#include "ceres/types.h"

namespace ceres {
namespace internal {
namespace {

bool IsZeroOrInfinity(double x) {
  return ((x == 0.0) || (IsInfinite(x)));
}

// Constant used in the MATLAB implementation ~ 2 * eps.
const double kEpsilon = 2.2204e-16;

}  // namespace

ConjugateGradientsSolver::ConjugateGradientsSolver(
    const LinearSolver::Options& options)
    : options_(options) {
}

LinearSolver::Summary ConjugateGradientsSolver::Solve(
    LinearOperator* A,
    const double* b,
    const LinearSolver::PerSolveOptions& per_solve_options,
    double* x) {
  CHECK_NOTNULL(A);
  CHECK_NOTNULL(x);
  CHECK_NOTNULL(b);
  CHECK_EQ(A->num_rows(), A->num_cols());

  LinearSolver::Summary summary;
  summary.termination_type = MAX_ITERATIONS;
  summary.num_iterations = 0;

  int num_cols = A->num_cols();
  VectorRef xref(x, num_cols);
  ConstVectorRef bref(b, num_cols);

  double norm_b = bref.norm();
  if (norm_b == 0.0) {
    xref.setZero();
    summary.termination_type = TOLERANCE;
    return summary;
  }

  Vector r(num_cols);
  Vector p(num_cols);
  Vector z(num_cols);
  Vector tmp(num_cols);

  double tol_r = per_solve_options.r_tolerance * norm_b;

  tmp.setZero();
  A->RightMultiply(x, tmp.data());
  r = bref - tmp;
  double norm_r = r.norm();

  if (norm_r <= tol_r) {
    summary.termination_type = TOLERANCE;
    return summary;
  }

  double rho = 1.0;

  // Initial value of the quadratic model Q = x'Ax - 2 * b'x.
  double Q0 = -1.0 * xref.dot(bref + r);

  for (summary.num_iterations = 1;
       summary.num_iterations < options_.max_num_iterations;
       ++summary.num_iterations) {
    VLOG(3) << "cg iteration " << summary.num_iterations;

    // Apply preconditioner
    if (per_solve_options.preconditioner != NULL) {
      z.setZero();
      per_solve_options.preconditioner->RightMultiply(r.data(), z.data());
    } else {
      z = r;
    }

    double last_rho = rho;
    rho = r.dot(z);

    if (IsZeroOrInfinity(rho)) {
      LOG(ERROR) << "Numerical failure. rho = " << rho;
      summary.termination_type = FAILURE;
      break;
    };

    if (summary.num_iterations == 1) {
      p = z;
    } else {
      double beta = rho / last_rho;
      if (IsZeroOrInfinity(beta)) {
        LOG(ERROR) << "Numerical failure. beta = " << beta;
        summary.termination_type = FAILURE;
        break;
      }
      p = z + beta * p;
    }

    Vector& q = z;
    q.setZero();
    A->RightMultiply(p.data(), q.data());
    double pq = p.dot(q);

    if ((pq <= 0) || IsInfinite(pq))  {
      LOG(ERROR) << "Numerical failure. pq = " << pq;
      summary.termination_type = FAILURE;
      break;
    }

    double alpha = rho / pq;
    if (IsInfinite(alpha)) {
      LOG(ERROR) << "Numerical failure. alpha " << alpha;
      summary.termination_type = FAILURE;
      break;
    }

    xref = xref + alpha * p;

    // Ideally we would just use the update r = r - alpha*q to keep
    // track of the residual vector. However this estimate tends to
    // drift over time due to round off errors. Thus every
    // residual_reset_period iterations, we calculate the residual as
    // r = b - Ax. We do not do this every iteration because this
    // requires an additional matrix vector multiply which would
    // double the complexity of the CG algorithm.
    if (summary.num_iterations % options_.residual_reset_period == 0) {
      tmp.setZero();
      A->RightMultiply(x, tmp.data());
      r = bref - tmp;
    } else {
      r = r - alpha * q;
    }

    // Quadratic model based termination.
    //   Q1 = x'Ax - 2 * b' x.
    double Q1 = -1.0 * xref.dot(bref + r);

    // For PSD matrices A, let
    //
    //   Q(x) = x'Ax - 2b'x
    //
    // be the cost of the quadratic function defined by A and b. Then,
    // the solver terminates at iteration i if
    //
    //   i * (Q(x_i) - Q(x_i-1)) / Q(x_i) < q_tolerance.
    //
    // This termination criterion is more useful when using CG to
    // solve the Newton step. This particular convergence test comes
    // from Stephen Nash's work on truncated Newton
    // methods. References:
    //
    //   1. Stephen G. Nash & Ariela Sofer, Assessing A Search
    //   Direction Within A Truncated Newton Method, Operation
    //   Research Letters 9(1990) 219-221.
    //
    //   2. Stephen G. Nash, A Survey of Truncated Newton Methods,
    //   Journal of Computational and Applied Mathematics,
    //   124(1-2), 45-59, 2000.
    //
    double zeta = summary.num_iterations * (Q1 - Q0) / Q1;
    VLOG(3) << "Q termination: zeta " << zeta
            << " " << per_solve_options.q_tolerance;
    if (zeta < per_solve_options.q_tolerance) {
      summary.termination_type = TOLERANCE;
      break;
    }
    Q0 = Q1;

    // Residual based termination.
    norm_r = r. norm();
    VLOG(3) << "R termination: norm_r " << norm_r
            << " " << tol_r;
    if (norm_r <= tol_r) {
      summary.termination_type = TOLERANCE;
      break;
    }
  }

  return summary;
};

}  // namespace internal
}  // namespace ceres
