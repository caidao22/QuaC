
#include "operators_p.h"
#include "operators.h"
#include "solver.h"
#include "kron_p.h"
#include "dm_utilities.h"
#include <stdlib.h>
#include <stdio.h>

static PetscReal default_rtol     = 1e-11;
static PetscInt  default_restart  = 100;
static int       stab_added       = 0;
static int       matrix_assembled = 0;
PetscErrorCode RHSFunction (TS,PetscReal,Vec,Vec,void*); // Move to header?
PetscErrorCode (*_ts_monitor)(TS,PetscInt,PetscReal,Vec,void*) = NULL;
  
/*
 * steady_state solves for the steady_state of the system
 * that was previously setup using the add_to_ham and add_lin
 * routines. Solver selection and parameterscan be controlled via PETSc
 * command line options.
 */
void steady_state(Vec x){
  PetscViewer    mat_view;
  PC             pc;
  Vec            b;
  KSP            ksp; /* linear solver context */
  PetscInt       row,col,its,j,i,Istart,Iend;
  PetscScalar    mat_tmp;
  long           dim;


  dim = total_levels*total_levels;

  if (!stab_added){
    if (nid==0) printf("Adding stabilization...\n");
    /*
     * Add elements to the matrix to make the normalization work
     * I have no idea why this works, I am copying it from qutip
     * We add 1.0 in the 0th spot and every n+1 after
     */
    if (nid==0) {
      row = 0;
      for (i=0;i<total_levels;i++){
        col = i*(total_levels+1);
        mat_tmp = 1.0 + 0.*PETSC_i;
        MatSetValue(full_A,row,col,mat_tmp,ADD_VALUES);
      }

      /* Print dense ham, if it was asked for */
      if (_print_dense_ham){
        FILE *fp_ham;

        fp_ham = fopen("ham","w");

        if (nid==0){
          for (i=0;i<total_levels;i++){
            for (j=0;j<total_levels;j++){
              fprintf(fp_ham,"%e ",_hamiltonian[i][j]);
            }
            fprintf(fp_ham,"\n");
          }
        }
        fclose(fp_ham);
        for (i=0;i<total_levels;i++){
          free(_hamiltonian[i]);
        }
        free(_hamiltonian);
        _print_dense_ham = 0;
      }
    }
    stab_added = 1;
  }

  //  if (!matrix_assembled) {
    MatGetOwnershipRange(full_A,&Istart,&Iend);
    /*
     * Explicitly add 0.0 to all diagonal elements;
     * this fixes a 'matrix in wrong state' message that PETSc
     * gives if the diagonal was never initialized.
     */
    if (nid==0) printf("Adding 0 to diagonal elements...\n");
    for (i=Istart;i<Iend;i++){
      mat_tmp = 0 + 0.*PETSC_i;
      MatSetValue(full_A,i,i,mat_tmp,ADD_VALUES);
    }
    
    
    /* Tell PETSc to assemble the matrix */
    MatAssemblyBegin(full_A,MAT_FINAL_ASSEMBLY);
    MatAssemblyEnd(full_A,MAT_FINAL_ASSEMBLY);
    if (nid==0) printf("Matrix Assembled.\n");
    matrix_assembled = 1;
    //  }
  /* Print information about the matrix. */
  PetscViewerASCIIOpen(PETSC_COMM_WORLD,NULL,&mat_view);
  PetscViewerPushFormat(mat_view,PETSC_VIEWER_ASCII_INFO);
  MatView(full_A,mat_view);
  PetscViewerDestroy(&mat_view);
  /*
   * Create parallel vectors.
   * - When using VecCreate(), VecSetSizes() and VecSetFromOptions(),
   * we specify only the vector's global
   * dimension; the parallel partitioning is determined at runtime.
   * - Note: We form 1 vector from scratch and then duplicate as needed.
   */
  VecCreate(PETSC_COMM_WORLD,&b);
  VecSetSizes(b,PETSC_DECIDE,dim);
  VecSetFromOptions(b);

  //  VecDuplicate(b,&x); Assume x is passed in

  /*
   * Set rhs, b, and solution, x to 1.0 in the first
   * element, 0.0 elsewhere.
   */
  VecSet(b,0.0);
  VecSet(x,0.0);
  
  if(nid==0) {
    row = 0;
    mat_tmp = 1.0 + 0.0*PETSC_i;
    VecSetValue(x,row,mat_tmp,INSERT_VALUES);
    VecSetValue(b,row,mat_tmp,INSERT_VALUES);
  }
  
  /* Assemble x and b */
  VecAssemblyBegin(x);
  VecAssemblyEnd(x);

  VecAssemblyBegin(b);
  VecAssemblyEnd(b);

    /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -*
     *           Create the linear solver and set various options         *
     *- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  /*
   * Create linear solver context
   */
  KSPCreate(PETSC_COMM_WORLD,&ksp);

  /*
   * Set operators. Here the matrix that defines the linear system
   * also serves as the preconditioning matrix.
   */
  KSPSetOperators(ksp,full_A,full_A);
  
  /*
   * Set good default options for solver
   */
  /* relative tolerance */
  KSPSetTolerances(ksp,default_rtol,PETSC_DEFAULT,PETSC_DEFAULT,PETSC_DEFAULT);

  /* bjacobi preconditioner */
  KSPGetPC(ksp,&pc);
  PCSetType(pc,PCASM);

  /* gmres solver with 100 restart*/
  KSPSetType(ksp,KSPGMRES);
  KSPGMRESSetRestart(ksp,default_restart);
  /* 
   * Set runtime options, e.g.,
   *     -ksp_type <type> -pc_type <type> -ksp_monitor -ksp_rtol <rtol>
   */
  KSPSetFromOptions(ksp);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
                      Solve the linear system
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  if (nid==0) printf("KSP set. Solving for steady state...\n");
  KSPSolve(ksp,b,x);
  /* Pass -1.0 to flag the routine to print the final populations to stdout */
  get_populations(x,-1.0);

  KSPGetIterationNumber(ksp,&its);

  PetscPrintf(PETSC_COMM_WORLD,"Iterations %D\n",its);
  
  /* Free work space */
  KSPDestroy(&ksp);
  //  VecDestroy(&x);
  VecDestroy(&b);

  return;
}

/*
 * time_step solves for the time_dependence of the system
 * that was previously setup using the add_to_ham and add_lin
 * routines. Solver selection and parameters can be controlled via PETSc
 * command line options. Default solver is TSRK3BS
 *
 * Inputs:
 *       Vec     x:       The density matrix, with appropriate inital conditions
 *       double dt:       initial timestep. For certain explicit methods, this timestep 
 *                        can be changed, as those methods have adaptive time steps
 *       double time_max: the maximum time to integrate to
 *       int steps_max:   max number of steps to take
 */
void time_step(Vec x, PetscReal time_max,PetscReal dt,PetscInt steps_max){
  PetscViewer    mat_view;
  //  Vec            x;
  TS             ts; /* timestepping context */
  PetscInt       i,j,Istart,Iend,steps,row,col;
  PetscScalar    mat_tmp;
  /* long           dim; */

  /* dim = total_levels*total_levels; */

  /* Possibly print dense ham. No stabilization is needed? */
  if (nid==0) {
    /* Print dense ham, if it was asked for */
    if (_print_dense_ham){
      FILE *fp_ham;

      fp_ham = fopen("ham","w");
      
      if (nid==0){
        for (i=0;i<total_levels;i++){
          for (j=0;j<total_levels;j++){
            fprintf(fp_ham,"%e ",_hamiltonian[i][j]);
          }
          fprintf(fp_ham,"\n");
        }
      }
      fclose(fp_ham);
      for (i=0;i<total_levels;i++){
        free(_hamiltonian[i]);
      }
      free(_hamiltonian);
      _print_dense_ham = 0;
    }
  }


  /* Remove stabilization if it was previously added */
  if (stab_added){
    if (nid==0) printf("Removing stabilization...\n");
    /*
     * We add 1.0 in the 0th spot and every n+1 after
     */
    if (nid==0) {
      row = 0;
      for (i=0;i<total_levels;i++){
        col = i*(total_levels+1);
        mat_tmp = -1.0 + 0.*PETSC_i;
        MatSetValue(full_A,row,col,mat_tmp,ADD_VALUES);
      }
    }
  }

  MatGetOwnershipRange(full_A,&Istart,&Iend);
  /*
   * Explicitly add 0.0 to all diagonal elements;
   * this fixes a 'matrix in wrong state' message that PETSc
   * gives if the diagonal was never initialized.
   */
  if (nid==0) printf("Adding 0 to diagonal elements...\n");
  for (i=Istart;i<Iend;i++){
    mat_tmp = 0 + 0.*PETSC_i;
    MatSetValue(full_A,i,i,mat_tmp,ADD_VALUES);
  }
  
  
  /* Tell PETSc to assemble the matrix */
  MatAssemblyBegin(full_A,MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd(full_A,MAT_FINAL_ASSEMBLY);
  if (nid==0) printf("Matrix Assembled.\n");

  /* Print information about the matrix. */
  PetscViewerASCIIOpen(PETSC_COMM_WORLD,NULL,&mat_view);
  PetscViewerPushFormat(mat_view,PETSC_VIEWER_ASCII_INFO);
  MatView(full_A,mat_view);
  PetscViewerDestroy(&mat_view);
  /*
   * Create parallel vectors.
   * - When using VecCreate(), VecSetSizes() and VecSetFromOptions(),
   * we specify only the vector's global
   * dimension; the parallel partitioning is determined at runtime.
   * - Note: We form 1 vector from scratch and then duplicate as needed.
   */
  /* VecCreate(PETSC_COMM_WORLD,&x); */
  /* VecSetSizes(x,PETSC_DECIDE,dim); */

  //  create_dm(&x,total_levels); //Assume 
  /* VecSetFromOptions(x); */

  /* VecSet(x,0.0); */

  //  _set_initial_density_matrix(x);
  
  /* Assemble x and b */
  //  VecAssemblyBegin(x);
  //  VecAssemblyEnd(x);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -*
   *       Create the timestepping solver and set various options       *
   *- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  /*
   * Create timestepping solver context
   */
  TSCreate(PETSC_COMM_WORLD,&ts);
  TSSetProblemType(ts,TS_LINEAR);


  /*
   * Set function to get information at every timestep
   */
  if (_ts_monitor!=NULL){
    TSMonitorSet(ts,_ts_monitor,NULL,NULL);
  }
  /*
   * Set up ODE system
   */

   /* TSSetRHSFunct
ion(ts,NULL,RHSFunction,NULL); */
  TSSetRHSFunction(ts,NULL,TSComputeRHSFunctionLinear,NULL);
  TSSetRHSJacobian(ts,full_A,full_A,TSComputeRHSJacobianConstant,NULL);

  TSSetInitialTimeStep(ts,0.0,dt);

  /*
   * Set default options, can be changed at runtime
   */

  TSSetDuration(ts,steps_max,time_max);
  TSSetExactFinalTime(ts,TS_EXACTFINALTIME_STEPOVER);
  TSSetType(ts,TSRK);
  TSRKSetType(ts,TSRK3BS);
  //  TSSetSolution(ts,x);
  TSSetFromOptions(ts);
  TSSolve(ts,x);
  TSGetTimeStepNumber(ts,&steps);

  /* Pass -1.0 to flag the routine to print the final populations to stdout */
  get_populations(x,-1.0);

  PetscPrintf(PETSC_COMM_WORLD,"Steps %D\n",steps);

  /* Free work space */
  TSDestroy(&ts);
  //  destroy_dm(x);
  /* VecDestroy(&x); */

  return;
}

void set_ts_monitor(PetscErrorCode (*monitor)(TS,PetscInt,PetscReal,Vec,void*)){
  _ts_monitor = (*monitor);
}


PetscErrorCode RHSFunction (TS ts, PetscReal t, Vec array_in, Vec array_out, void *s){
  MatMult(full_A,array_in,array_out);
  return 0;
}

/*
 * ts_monitor is the catchall routine which will look at the data
 * at every time step, such as printing observables/populations.
 *
 * Inputs:
 *    ts     - the timestep context
 *    step   - the count of the current step (with 0 meaning the
 *             initial condition)
 *    time   - the current time
 *    u      - the solution at this timestep
 *    ctx    - the user-provided context for this monitoring routine.
 */

/* PetscErrorCode ts_monitor(TS ts,PetscInt step,PetscReal time,Vec u,void *ctx) { */
  
/*   get_populations(u); */
/*   PetscFunctionReturn(0); */
/* } */

