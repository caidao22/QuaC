#ifndef SOLVER_H_
#define SOLVER_H_

#include <petscksp.h>
#include <petscts.h>
#include <petscsys.h>

void steady_state();
void time_step(PetscReal,PetscReal,PetscInt);
PetscErrorCode time_step_adjoint(PetscReal,PetscReal,PetscInt,PetscBool);
void set_ts_monitor(PetscErrorCode (*monitor)(TS,PetscInt,PetscReal,Vec,void*));

#endif
