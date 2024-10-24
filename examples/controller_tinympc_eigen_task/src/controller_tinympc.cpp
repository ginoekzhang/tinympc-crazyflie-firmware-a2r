/**
 * ,---------,       ____  _ __
 * |  ,-^-,  |      / __ )(_) /_______________ _____  ___
 * | (  O  ) |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * | / ,--´  |    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *    +------`   /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie control firmware
 *
 * Copyright (C) 2019 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * controller_tinympc.c - App layer application of TinyMPC.
 */

/**
 * Single lap
 */

#include "Eigen.h"

#ifdef __cplusplus
extern "C"
{
#endif

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "app.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "sensors.h"
#include "static_mem.h"
#include "system.h"

#include "controller.h"
#include "physicalConstants.h"
#include "log.h"
#include "eventtrigger.h"
#include "param.h"
#include "num.h"
#include "math3d.h"

#include "cpp_compat.h" // needed to compile Cpp to C

// TinyMPC and PID controllers
#include "tinympc/admm.hpp"
#include "controller_pid.h"

// Params
// #include "quadrotor_10hz_params.hpp"
// #include "quadrotor_50hz_params.hpp" // rho = 65
// #include "quadrotor_50hz_params_2.hpp" // rho = 5, passive
// #include "quadrotor_50hz_params_3.hpp" // rho = 5, aggressive
// #include "quadrotor_50hz_params_constraints.hpp"
// #include "quadrotor_250hz_params.hpp"
#include "quadrotor_50hz_params_unconstrained.hpp"
#include "quadrotor_50hz_params_constrained.hpp"

// Trajectory
#include "quadrotor_100hz_ref_hover.hpp"
// #include "quadrotor_50hz_ref_circle.hpp"
// #include "quadrotor_50hz_ref_circle_2_5s.hpp"
// #include "quadrotor_50hz_line_5s.hpp"
// #include "quadrotor_50hz_line_8s.hpp"
// #include "quadrotor_50hz_line_9s_xyz.hpp"

// Edit the debug name to get nice debug prints
#define DEBUG_MODULE "MPCTASK"
#include "debug.h"

// #define MPC_RATE RATE_250_HZ  // control frequency
// #define MPC_RATE RATE_50_HZ
#define MPC_RATE RATE_100_HZ
#define LOWLEVEL_RATE RATE_500_HZ

// Semaphore to signal that we got data from the stabilizer loop to process
static SemaphoreHandle_t runTaskSemaphore;

// Mutex to protect data that is shared between the task and
// functions called by the stabilizer loop
static SemaphoreHandle_t dataMutex;
static StaticSemaphore_t dataMutexBuffer;

static void tinympcControllerTask(void *parameters);

STATIC_MEM_TASK_ALLOC(tinympcControllerTask, TINYMPC_TASK_STACKSIZE);

// // declares eventTrigger_[name] and eventTrigger_[name]_payload
// EVENTTRIGGER(horizon_part1, float, h0, float, h1, float, h2, float, h3, float, h4);
// EVENTTRIGGER(horizon_part2, float, h5, float, h6, float, h7, float, h8, float, h9);
// EVENTTRIGGER(horizon_part3, float, h10, float, h11, float, h12, float, h13, float, h14);
// EVENTTRIGGER(horizon_part3, float, h15, float, h16, float, h17, float, h18, float, h19);
// EVENTTRIGGER(iters_event, int32, iters);
// EVENTTRIGGER(cache_level_event, int32, level);

// declares eventTrigger_[name] and eventTrigger_[name]_payload
EVENTTRIGGER(horizon_x_part1, float, h0, float, h1, float, h2, float, h3, float, h4);
EVENTTRIGGER(horizon_x_part2, float, h5, float, h6, float, h7, float, h8, float, h9);
EVENTTRIGGER(horizon_x_part3, float, h10, float, h11, float, h12, float, h13, float, h14);
EVENTTRIGGER(horizon_x_part4, float, h15, float, h16, float, h17, float, h18, float, h19);
EVENTTRIGGER(horizon_y_part1, float, h0, float, h1, float, h2, float, h3, float, h4);
EVENTTRIGGER(horizon_y_part2, float, h5, float, h6, float, h7, float, h8, float, h9);
EVENTTRIGGER(horizon_y_part3, float, h10, float, h11, float, h12, float, h13, float, h14);
EVENTTRIGGER(horizon_y_part4, float, h15, float, h16, float, h17, float, h18, float, h19);
EVENTTRIGGER(horizon_z_part1, float, h0, float, h1, float, h2, float, h3, float, h4);
EVENTTRIGGER(horizon_z_part2, float, h5, float, h6, float, h7, float, h8, float, h9);
EVENTTRIGGER(horizon_z_part3, float, h10, float, h11, float, h12, float, h13, float, h14);
EVENTTRIGGER(horizon_z_part4, float, h15, float, h16, float, h17, float, h18, float, h19);
EVENTTRIGGER(problem_data_event, int32, solvetime_us, int32, iters, int32, cache_level);
EVENTTRIGGER(problem_residuals_event, float, prim_resid_state, float, prim_resid_input, float, dual_resid_state, float, dual_resid_input);



// Structs to keep track of data sent to and received by stabilizer loop
// Stabilizer loop updates/uses these
control_t control_data;
setpoint_t setpoint_data;
sensorData_t sensors_data;
state_t state_data;
tiny_VectorNx mpc_setpoint;
setpoint_t mpc_setpoint_pid;
// Copies that stay constant for duration of MPC loop
setpoint_t setpoint_task;
sensorData_t sensors_task;
state_t state_task;
control_t control_task;
tiny_VectorNx mpc_setpoint_task;

/* Allocate global variables for MPC */
// static tinytype u_hover[4] = {.65, .65, .65, .65};
static tinytype u_hover[4] = {.583, .583, .583, .583};
static struct tiny_cache cache;
static struct tiny_params params;
static struct tiny_problem problem;
static tiny_MatrixNxNh problem_x;
static float horizon_nh_z;
static float init_vel_z;
// static Eigen::Matrix<tinytype, NSTATES, NTOTAL, Eigen::ColMajor> Xref_total;
static Eigen::Matrix<tinytype, 3, NTOTAL, Eigen::ColMajor> Xref_total;
static Eigen::Matrix<tinytype, NSTATES, 1, Eigen::ColMajor> Xref_origin; // Start position for trajectory
static Eigen::Matrix<tinytype, NSTATES, 1, Eigen::ColMajor> Xref_end; // End position for trajectory
static tiny_VectorNu u_lqr;
static tiny_VectorNx current_state;

// Helper variables
static bool enable_traj = false;
static int traj_index = 0;
static int max_traj_index = 0;
static int mpc_steps_taken = 0;
static uint64_t startTimestamp;
static uint32_t timestamp;
static uint32_t mpc_start_timestamp;
static uint32_t mpc_time_us;
static struct vec phi; // For converting from the current state estimate's quaternion to Rodrigues parameters
static bool isInit = false;
static float obs_velocity_scale = 1;
static float use_obs_offset = 0;

// Obstacle constraint variables
static Eigen::Matrix<tinytype, 3, 1> obs_center;
static Eigen::Matrix<tinytype, 3, 1> obs_predicted_center;
static Eigen::Matrix<tinytype, 3, 1> obs_velocity;
static Eigen::Matrix<tinytype, 3, 1> obs_offset;
static float r_obs = .5;

static Eigen::Matrix<tinytype, 3, 1> xc;
static Eigen::Matrix<tinytype, 3, 1> a_norm;
static Eigen::Matrix<tinytype, 3, 1> q_c;

static inline float quat_dot(quaternion_t a, quaternion_t b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

static inline quaternion_t make_quat(float x, float y, float z, float w)
{
  quaternion_t q;
  q.x = x;
  q.y = y;
  q.z = z;
  q.w = w;
  return q;
}

static inline quaternion_t normalize_quat(quaternion_t q)
{
  float s = 1.0f / sqrtf(quat_dot(q, q));
  return make_quat(s * q.x, s * q.y, s * q.z, s * q.w);
}

static inline struct vec quat_2_rp(quaternion_t q)
{
  struct vec v;
  v.x = q.x / q.w;
  v.y = q.y / q.w;
  v.z = q.z / q.w;
  return v;
}

void appMain()
{
  DEBUG_PRINT("Waiting for activation ...\n");

  while (1)
  {
    vTaskDelay(M2T(2000));
  }
}

static void resetProblem(void) {
  // Copy problem data
  problem.x = tiny_MatrixNxNh::Zero();
  problem.q = tiny_MatrixNxNh::Zero();
  problem.p = tiny_MatrixNxNh::Zero();
  problem.v = tiny_MatrixNxNh::Zero();
  problem.vnew = tiny_MatrixNxNh::Zero();
  problem.g = tiny_MatrixNxNh::Zero();

  problem.u = tiny_MatrixNuNhm1::Zero();
  problem.r = tiny_MatrixNuNhm1::Zero();
  problem.d = tiny_MatrixNuNhm1::Zero();
  problem.z = tiny_MatrixNuNhm1::Zero();
  problem.znew = tiny_MatrixNuNhm1::Zero();
  problem.y = tiny_MatrixNuNhm1::Zero();
}


void controllerOutOfTreeInit(void)
{

  controllerPidInit();

  // Copy cache data from problem_data/quadrotor*.hpp
  cache.Adyn[0] = Eigen::Map<Matrix<tinytype, NSTATES, NSTATES, Eigen::RowMajor>>(Adyn_unconstrained_data);
  cache.Bdyn[0] = Eigen::Map<Matrix<tinytype, NSTATES, NINPUTS, Eigen::RowMajor>>(Bdyn_unconstrained_data);
  cache.rho[0] = rho_unconstrained_value;
  cache.Kinf[0] = Eigen::Map<Matrix<tinytype, NINPUTS, NSTATES, Eigen::RowMajor>>(Kinf_unconstrained_data);
  cache.Pinf[0] = Eigen::Map<Matrix<tinytype, NSTATES, NSTATES, Eigen::RowMajor>>(Pinf_unconstrained_data);
  cache.Quu_inv[0] = Eigen::Map<Matrix<tinytype, NINPUTS, NINPUTS, Eigen::RowMajor>>(Quu_inv_unconstrained_data);
  cache.AmBKt[0] = Eigen::Map<Matrix<tinytype, NSTATES, NSTATES, Eigen::RowMajor>>(AmBKt_unconstrained_data);
  cache.coeff_d2p[0] = Eigen::Map<Matrix<tinytype, NSTATES, NINPUTS, Eigen::RowMajor>>(coeff_d2p_unconstrained_data);

  cache.Adyn[1] = Eigen::Map<Matrix<tinytype, NSTATES, NSTATES, Eigen::RowMajor>>(Adyn_constrained_data);
  cache.Bdyn[1] = Eigen::Map<Matrix<tinytype, NSTATES, NINPUTS, Eigen::RowMajor>>(Bdyn_constrained_data);
  cache.rho[1] = rho_constrained_value;
  cache.Kinf[1] = Eigen::Map<Matrix<tinytype, NINPUTS, NSTATES, Eigen::RowMajor>>(Kinf_constrained_data);
  cache.Pinf[1] = Eigen::Map<Matrix<tinytype, NSTATES, NSTATES, Eigen::RowMajor>>(Pinf_constrained_data);
  cache.Quu_inv[1] = Eigen::Map<Matrix<tinytype, NINPUTS, NINPUTS, Eigen::RowMajor>>(Quu_inv_constrained_data);
  cache.AmBKt[1] = Eigen::Map<Matrix<tinytype, NSTATES, NSTATES, Eigen::RowMajor>>(AmBKt_constrained_data);
  cache.coeff_d2p[1] = Eigen::Map<Matrix<tinytype, NSTATES, NINPUTS, Eigen::RowMajor>>(coeff_d2p_constrained_data);

  // Copy parameter data
  params.Q[0] = Eigen::Map<tiny_VectorNx>(Q_unconstrained_data);
  params.Qf[0] = Eigen::Map<tiny_VectorNx>(Qf_unconstrained_data);
  params.R[0] = Eigen::Map<tiny_VectorNu>(R_unconstrained_data);
  params.Q[1] = Eigen::Map<tiny_VectorNx>(Q_constrained_data);
  params.Qf[1] = Eigen::Map<tiny_VectorNx>(Qf_constrained_data);
  params.R[1] = Eigen::Map<tiny_VectorNu>(R_constrained_data);
  params.u_min = tiny_VectorNu(-u_hover[0], -u_hover[1], -u_hover[2], -u_hover[3]).replicate<1, NHORIZON - 1>();
  params.u_max = tiny_VectorNu(1 - u_hover[0], 1 - u_hover[1], 1 - u_hover[2], 1 - u_hover[3]).replicate<1, NHORIZON - 1>();
  for (int i = 0; i < NHORIZON; i++)
  {
    params.x_min[i] = tiny_VectorNc::Constant(-1000); // Currently unused
    params.x_max[i] = tiny_VectorNc::Constant(1000);
    params.A_constraints[i] = tiny_MatrixNcNx::Zero();
  }
  params.Xref = tiny_MatrixNxNh::Zero();
  params.Uref = tiny_MatrixNuNhm1::Zero();
  params.cache = cache;

  // Initialize problem data to zero
  resetProblem();

  problem.primal_residual_state = 0;
  problem.primal_residual_input = 0;
  problem.dual_residual_state = 0;
  problem.dual_residual_input = 0;
  problem.abs_tol = 0.001;
  problem.status = 0;
  problem.iter = 0;
  problem.max_iter = 5;
  problem.iters_check_rho_update = 10;
  problem.cache_level = 0; // 0 to use rho corresponding to inactive constraints (1 to use rho corresponding to active constraints)

  // // Copy reference trajectory into Eigen matrix
  // Xref_total = Eigen::Map<Matrix<tinytype, NTOTAL, NSTATES, Eigen::RowMajor>>(Xref_data).transpose();
  // Xref_total = Eigen::Map<Matrix<tinytype, NTOTAL, 3, Eigen::RowMajor>>(Xref_data).transpose();
  // Xref_origin << Xref_total.col(0).head(3), 0, 0, 0, 0, 0, 0, 0, 0, 0; // Go to xyz start of traj
  // Xref_end << Xref_total.col(NTOTAL-1).head(3), 0, 0, 0, 0, 0, 0, 0, 0, 0; // Go to xyz start of traj
  // Xref_origin << Xref_total.col(0), 0, 0, 0, 0, 0, 0, 0, 0, 0; // Go to xyz start of traj
  // Xref_end << Xref_total.col(NTOTAL-1).head(3), 0, 0, 0, 0, 0, 0, 0, 0, 0; // Go to xyz start of traj
  Xref_origin << 0, 0, 1.5, 0, 0, 0, 0, 0, 0, 0, 0, 0; // Always go to 0, 0, 1 (comment out enable_traj = true check in main loop)
  params.Xref = Xref_origin.replicate<1, NHORIZON>();

  enable_traj = false;
  traj_index = 0;
  max_traj_index = NTOTAL - NHORIZON;

  /* Begin task initialization */
  runTaskSemaphore = xSemaphoreCreateBinary();
  ASSERT(runTaskSemaphore);

  dataMutex = xSemaphoreCreateMutexStatic(&dataMutexBuffer);

  STATIC_MEM_TASK_CREATE(tinympcControllerTask, tinympcControllerTask, TINYMPC_TASK_NAME, NULL, TINYMPC_TASK_PRI);

  isInit = true;
  /* End of task initialization */
}

static void UpdateHorizonReference(const setpoint_t *setpoint)
{
  if (enable_traj)
  {
    if (traj_index < max_traj_index)
    {
      // params.Xref = Xref_total.block<NSTATES, NHORIZON>(0, traj_index);
      params.Xref.block<3, NHORIZON>(0,0) = Xref_total.block<3, NHORIZON>(0, traj_index);
      traj_index++;
    }
    else if (traj_index >= max_traj_index) {
      params.Xref = Xref_end.replicate<1, NHORIZON>();
    }
    else
    {
      enable_traj = false;
    }
  }
  else
  {
    params.Xref = Xref_origin.replicate<1, NHORIZON>();
  }
}

bool controllerOutOfTreeTest()
{
  // Always return true
  return true;
}

static void tinympcControllerTask(void *parameters)
{
  // systemWaitStart();

  uint32_t nowMs = T2M(xTaskGetTickCount());
  uint32_t nextMpcMs = nowMs;

  startTimestamp = usecTimestamp();

  while (true)
  {
    // Update task data with most recent stabilizer loop data
    xSemaphoreTake(runTaskSemaphore, portMAX_DELAY);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    memcpy(&setpoint_task, &setpoint_data, sizeof(setpoint_t));
    memcpy(&sensors_task, &sensors_data, sizeof(sensorData_t));
    memcpy(&state_task, &state_data, sizeof(state_t));
    memcpy(&control_task, &control_data, sizeof(control_t));
    xSemaphoreGive(dataMutex);

    nowMs = T2M(xTaskGetTickCount());
    if (nowMs >= nextMpcMs)
    {
      nextMpcMs = nowMs + (1000.0f / MPC_RATE);

      // Comment out when avoiding dynamic obstacle
      // Uncomment if following reference trajectory
      if (usecTimestamp() - startTimestamp > 1000000 * 2 && traj_index == 0)
      {
        DEBUG_PRINT("Enable trajectory!\n");
        // enable_traj = true; 
        traj_index++;
      }

      if (problem.cache_level == 0) {
        problem.y = tiny_MatrixNuNhm1::Zero();
        problem.g = tiny_MatrixNxNh::Zero();
      }

      // TODO: predict into the future and set initial x to wherever we think we'll be
      //    by the time we're done computing the input for that state. If we just set
      //    initial x to current state then by the time we compute the optimal input for
      //    that state we'll already be at the next state and there will be a mismatch
      //    in the input we're using for our current state.
      // Set initial x to current state
      phi = quat_2_rp(normalize_quat(state_task.attitudeQuaternion)); // quaternion to Rodrigues parameters
      problem.x.col(0) << state_task.position.x, state_task.position.y, state_task.position.z,
          phi.x, phi.y, phi.z,
          state_task.velocity.x, state_task.velocity.y, state_task.velocity.z,
          radians(sensors_task.gyro.x), radians(sensors_task.gyro.y), radians(sensors_task.gyro.z);

      // Get command reference
      UpdateHorizonReference(&setpoint_task);

      r_obs = setpoint_task.acceleration.x;
      obs_center(0) = setpoint_task.position.x;
      obs_center(1) = setpoint_task.position.y;
      obs_center(2) = setpoint_task.position.z;
      
      obs_velocity(0) = setpoint_task.velocity.x;
      obs_velocity(1) = setpoint_task.velocity.y;
      obs_velocity(2) = setpoint_task.velocity.z;

      // // When avoiding obstacle while tracking trajectory
      // if (enable_traj) {
      //   // Update constraint parameters
      //   for (int i=0; i<NHORIZON; i++) {
      //     obs_predicted_center = obs_center + (obs_velocity/50 * i) * obs_velocity_scale;
      //     xc = obs_predicted_center - problem.x.col(i).head(3);
      //     a_norm = xc / xc.norm();
      //     params.A_constraints[i].head(3) = a_norm.transpose();
      //     q_c = obs_center - r_obs * a_norm;
      //     params.x_max[i](0) = a_norm.transpose() * q_c;
      //   }
      // } else {
      //   for (int i=0; i<NHORIZON; i++) {
      //       params.x_min[i] = tiny_VectorNc::Constant(-1000); // Currently unused
      //       params.x_max[i] = tiny_VectorNc::Constant(1000);
      //       params.A_constraints[i] = tiny_MatrixNcNx::Zero();
      //   }
      // }

      if (obs_velocity.norm() < .001) {
        obs_offset << 0, 0, 0;
      }
      else {
        obs_offset = (problem.x.col(0).head(3) - obs_center).norm() * obs_velocity.normalized();
      }

      // When avoiding dynamic obstacle
      for (int i = 0; i < NHORIZON; i++)
      {
        // obs_predicted_center = obs_center + (obs_velocity/50 * i) * obs_velocity_scale + (problem.x.col(0).head(3) - obs_center).norm() * obs_velocity.normalized() * use_obs_offset;
        // obs_predicted_center = obs_center + (obs_velocity/50 * i) * obs_velocity_scale + (problem.x.col(0).head(3) - obs_center).norm() * obs_velocity.normalized();
        obs_predicted_center = obs_center +  obs_offset + (obs_velocity/50 * i);
        xc = obs_predicted_center - problem.x.col(i).head(3);
        a_norm = xc / xc.norm();
        params.A_constraints[i].head(3) = a_norm.transpose();
        q_c = obs_center - r_obs * a_norm;
        params.x_max[i](0) = a_norm.transpose() * q_c;
      }


      // // Start predicting the obstacle if the distance between it and the drone is less
      // // than the distance the obstacle would travel over the course of two seconds,
      // // since the drone should be able to move out of the way in less than two seconds.
      // if ((problem.x.col(0).head(3) - obs_center).norm() < obs_velocity.norm()*2) {
      //   obs_offset = (problem.x.col(0).head(3) - obs_center).norm()*.9 * obs_velocity.normalized();
      // }
      // else {
      //   obs_offset << 0.0, 0.0, 0.0;
      // }

      // // When avoiding dynamic obstacle
      // for (int i = 0; i < NHORIZON; i++)
      // {
      //   // obs_predicted_center = obs_center + (obs_velocity/50 * i) * obs_velocity_scale + (problem.x.col(0).head(3) - obs_center).norm() * obs_velocity.normalized() * use_obs_offset;
      //   // obs_predicted_center = obs_center + (obs_velocity/50 * i) * obs_velocity_scale + (problem.x.col(0).head(3) - obs_center).norm() * obs_velocity.normalized();
      //   obs_predicted_center = obs_center + obs_offset + (obs_velocity/50 * i) * obs_velocity_scale;
      //   xc = obs_predicted_center - problem.x.col(i).head(3);
      //   a_norm = xc / xc.norm();
      //   params.A_constraints[i].head(3) = a_norm.transpose();
      //   q_c = obs_center - r_obs * a_norm;
      //   params.x_max[i](0) = a_norm.transpose() * q_c;
      // }

      // MPC solve
      problem.iter = 0;

      mpc_start_timestamp = usecTimestamp();
      solve_admm(&problem, &params);
      vTaskDelay(M2T(1));
      solve_admm(&problem, &params);
      mpc_time_us = usecTimestamp() - mpc_start_timestamp - 1000; // -1000 for each vTaskDelay(M2T(1))

      mpc_setpoint_task = problem.x.col(NHORIZON-1);

      eventTrigger_horizon_x_part1_payload.h0 = problem.x.col(0)(0);
      eventTrigger_horizon_x_part1_payload.h1 = problem.x.col(1)(0);
      eventTrigger_horizon_x_part1_payload.h2 = problem.x.col(2)(0);
      eventTrigger_horizon_x_part1_payload.h3 = problem.x.col(3)(0);
      eventTrigger_horizon_x_part1_payload.h4 = problem.x.col(4)(0);
      eventTrigger_horizon_x_part2_payload.h5 = problem.x.col(5)(0);
      eventTrigger_horizon_x_part2_payload.h6 = problem.x.col(6)(0);
      eventTrigger_horizon_x_part2_payload.h7 = problem.x.col(7)(0);
      eventTrigger_horizon_x_part2_payload.h8 = problem.x.col(8)(0);
      eventTrigger_horizon_x_part2_payload.h9 = problem.x.col(9)(0);
      eventTrigger_horizon_x_part3_payload.h10 = problem.x.col(10)(0);
      eventTrigger_horizon_x_part3_payload.h11 = problem.x.col(11)(0);
      eventTrigger_horizon_x_part3_payload.h12 = problem.x.col(12)(0);
      eventTrigger_horizon_x_part3_payload.h13 = problem.x.col(13)(0);
      eventTrigger_horizon_x_part3_payload.h14 = problem.x.col(14)(0);
      eventTrigger_horizon_x_part4_payload.h15 = problem.x.col(15)(0);
      eventTrigger_horizon_x_part4_payload.h16 = problem.x.col(16)(0);
      eventTrigger_horizon_x_part4_payload.h17 = problem.x.col(17)(0);
      eventTrigger_horizon_x_part4_payload.h18 = problem.x.col(18)(0);
      eventTrigger_horizon_x_part4_payload.h19 = problem.x.col(19)(0);

      eventTrigger_horizon_y_part1_payload.h0 = problem.x.col(0)(1);
      eventTrigger_horizon_y_part1_payload.h1 = problem.x.col(1)(1);
      eventTrigger_horizon_y_part1_payload.h2 = problem.x.col(2)(1);
      eventTrigger_horizon_y_part1_payload.h3 = problem.x.col(3)(1);
      eventTrigger_horizon_y_part1_payload.h4 = problem.x.col(4)(1);
      eventTrigger_horizon_y_part2_payload.h5 = problem.x.col(5)(1);
      eventTrigger_horizon_y_part2_payload.h6 = problem.x.col(6)(1);
      eventTrigger_horizon_y_part2_payload.h7 = problem.x.col(7)(1);
      eventTrigger_horizon_y_part2_payload.h8 = problem.x.col(8)(1);
      eventTrigger_horizon_y_part2_payload.h9 = problem.x.col(9)(1);
      eventTrigger_horizon_y_part3_payload.h10 = problem.x.col(10)(1);
      eventTrigger_horizon_y_part3_payload.h11 = problem.x.col(11)(1);
      eventTrigger_horizon_y_part3_payload.h12 = problem.x.col(12)(1);
      eventTrigger_horizon_y_part3_payload.h13 = problem.x.col(13)(1);
      eventTrigger_horizon_y_part3_payload.h14 = problem.x.col(14)(1);
      eventTrigger_horizon_y_part4_payload.h15 = problem.x.col(15)(1);
      eventTrigger_horizon_y_part4_payload.h16 = problem.x.col(16)(1);
      eventTrigger_horizon_y_part4_payload.h17 = problem.x.col(17)(1);
      eventTrigger_horizon_y_part4_payload.h18 = problem.x.col(18)(1);
      eventTrigger_horizon_y_part4_payload.h19 = problem.x.col(19)(1);

      eventTrigger_horizon_z_part1_payload.h0 = problem.x.col(0)(2);
      eventTrigger_horizon_z_part1_payload.h1 = problem.x.col(1)(2);
      eventTrigger_horizon_z_part1_payload.h2 = problem.x.col(2)(2);
      eventTrigger_horizon_z_part1_payload.h3 = problem.x.col(3)(2);
      eventTrigger_horizon_z_part1_payload.h4 = problem.x.col(4)(2);
      eventTrigger_horizon_z_part2_payload.h5 = problem.x.col(5)(2);
      eventTrigger_horizon_z_part2_payload.h6 = problem.x.col(6)(2);
      eventTrigger_horizon_z_part2_payload.h7 = problem.x.col(7)(2);
      eventTrigger_horizon_z_part2_payload.h8 = problem.x.col(8)(2);
      eventTrigger_horizon_z_part2_payload.h9 = problem.x.col(9)(2);
      eventTrigger_horizon_z_part3_payload.h10 = problem.x.col(10)(2);
      eventTrigger_horizon_z_part3_payload.h11 = problem.x.col(11)(2);
      eventTrigger_horizon_z_part3_payload.h12 = problem.x.col(12)(2);
      eventTrigger_horizon_z_part3_payload.h13 = problem.x.col(13)(2);
      eventTrigger_horizon_z_part3_payload.h14 = problem.x.col(14)(2);
      eventTrigger_horizon_z_part4_payload.h15 = problem.x.col(15)(2);
      eventTrigger_horizon_z_part4_payload.h16 = problem.x.col(16)(2);
      eventTrigger_horizon_z_part4_payload.h17 = problem.x.col(17)(2);
      eventTrigger_horizon_z_part4_payload.h18 = problem.x.col(18)(2);
      eventTrigger_horizon_z_part4_payload.h19 = problem.x.col(19)(2);

      eventTrigger_problem_data_event_payload.solvetime_us = mpc_time_us;
      eventTrigger_problem_data_event_payload.iters = problem.iter;
      eventTrigger_problem_data_event_payload.cache_level = problem.cache_level;
      eventTrigger_problem_residuals_event_payload.prim_resid_state = problem.primal_residual_state;
      eventTrigger_problem_residuals_event_payload.prim_resid_input = problem.primal_residual_input;
      eventTrigger_problem_residuals_event_payload.dual_resid_state = problem.dual_residual_state;
      eventTrigger_problem_residuals_event_payload.dual_resid_input = problem.dual_residual_input;

      eventTrigger(&eventTrigger_horizon_x_part1);
      eventTrigger(&eventTrigger_horizon_x_part2);
      eventTrigger(&eventTrigger_horizon_x_part3);
      eventTrigger(&eventTrigger_horizon_x_part4);
      eventTrigger(&eventTrigger_horizon_y_part1);
      eventTrigger(&eventTrigger_horizon_y_part2);
      eventTrigger(&eventTrigger_horizon_y_part3);
      eventTrigger(&eventTrigger_horizon_y_part4);
      eventTrigger(&eventTrigger_horizon_z_part1);
      eventTrigger(&eventTrigger_horizon_z_part2);
      eventTrigger(&eventTrigger_horizon_z_part3);
      eventTrigger(&eventTrigger_horizon_z_part4);
      eventTrigger(&eventTrigger_problem_data_event);
      eventTrigger(&eventTrigger_problem_residuals_event);

      // Copy the setpoint calculated by the task loop to the global mpc_setpoint
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      memcpy(&mpc_setpoint, &mpc_setpoint_task, sizeof(tiny_VectorNx));
      memcpy(&init_vel_z, &problem.x.col(0)(8), sizeof(float));
      xSemaphoreGive(dataMutex);
    }
  }
}

/**
 * This function is called from the stabilizer loop. It is important that this call returns
 * as quickly as possible. The dataMutex must only be locked short periods by the task.
 */
void controllerOutOfTree(control_t *control, const setpoint_t *setpoint, const sensorData_t *sensors, const state_t *state, const uint32_t tick)
{
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  memcpy(&setpoint_data, setpoint, sizeof(setpoint_t));
  memcpy(&sensors_data, sensors, sizeof(sensorData_t));
  memcpy(&state_data, state, sizeof(state_t));
  // memcpy(control, &control_data, sizeof(state_t));

  if (RATE_DO_EXECUTE(LOWLEVEL_RATE, tick))
  {
    mpc_setpoint_pid.mode.yaw = modeAbs;
    mpc_setpoint_pid.mode.x = modeAbs;
    mpc_setpoint_pid.mode.y = modeAbs;
    mpc_setpoint_pid.mode.z = modeAbs;
    mpc_setpoint_pid.position.x = mpc_setpoint(0);
    mpc_setpoint_pid.position.y = mpc_setpoint(1);
    mpc_setpoint_pid.position.z = mpc_setpoint(2);
    mpc_setpoint_pid.attitude.yaw = mpc_setpoint(5);

    // if (RATE_DO_EXECUTE(RATE_25_HZ, tick)) {
    //   // DEBUG_PRINT("z: %.4f\n", mpc_setpoint(2));
    //   DEBUG_PRINT("h: %.4f\n", mpc_setpoint(4));
    //   // DEBUG_PRINT("x: %.4f\n", setpoint->position.x);
    // }

    controllerPid(control, &mpc_setpoint_pid, sensors, state, tick);
  }

  // if (RATE_DO_EXECUTE(LQR_RATE, tick)) {

  //   phi = quat_2_rp(normalize_quat(state->attitudeQuaternion));  // quaternion to Rodrigues parameters
  //   current_state << state->position.x, state->position.y, state->position.z,
  //                     phi.x, phi.y, phi.z,
  //                     state->velocity.x, state->velocity.y, state->velocity.z,
  //                     radians(sensors->gyro.x), radians(sensors->gyro.y), radians(sensors->gyro.z);

  //   // u_lqr = -params.cache.Kinf * (current_state - mpc_setpoint);
  //   u_lqr = -params.cache.Kinf * (current_state - Xref_origin);
  //   // u_lqr = -params.cache.Kinf * (current_state - params.Xref.col(0));

  //   if (setpoint->mode.z == modeDisable) {
  //     control->normalizedForces[0] = 0.0f;
  //     control->normalizedForces[1] = 0.0f;
  //     control->normalizedForces[2] = 0.0f;
  //     control->normalizedForces[3] = 0.0f;
  //   } else {
  //     control->normalizedForces[0] = u_lqr(0) + u_hover[0];  // PWM 0..1
  //     control->normalizedForces[1] = u_lqr(1) + u_hover[1];
  //     control->normalizedForces[2] = u_lqr(2) + u_hover[2];
  //     control->normalizedForces[3] = u_lqr(3) + u_hover[3];
  //   }
  //   control->controlMode = controlModePWM;
  // }

  xSemaphoreGive(dataMutex);

  // Allows mpc task to run again
  xSemaphoreGive(runTaskSemaphore);
}

/**
 * Logging variables for the command and reference signals for the
 * MPC controller
 */

LOG_GROUP_START(tinympc)

LOG_ADD(LOG_FLOAT, initial_velocity, &init_vel_z)

LOG_GROUP_STOP(tinympc)

#ifdef __cplusplus
} /* extern "C" */
#endif
