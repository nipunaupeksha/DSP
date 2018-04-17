/*
 * DwBundleDual.cpp
 *
 *  Created on: Feb 20, 2017
 *      Author: kibaekkim
 */

//#define DSP_DEBUG

#include "cplex.h"
#include "OsiCpxSolverInterface.hpp"
#include "DantzigWolfe/DwBundleDual.h"
#include "Utility/DspUtility.h"

DwBundleDual::DwBundleDual(DwWorker* worker):
DwMaster(worker),
v_(0.0),
counter_(1),
u_(1.0),
eps_(COIN_DBL_MAX),
absp_(COIN_DBL_MAX),
alpha_(COIN_DBL_MAX),
linerr_(COIN_DBL_MAX),
prev_dualobj_(COIN_DBL_MAX),
nstalls_(0) {
}

DwBundleDual::DwBundleDual(const DwBundleDual& rhs):
	DwMaster(rhs),
	v_(rhs.v_),
	counter_(rhs.counter_),
	u_(rhs.u_),
	eps_(rhs.eps_),
	absp_(rhs.absp_),
	alpha_(rhs.alpha_),
	linerr_(rhs.linerr_),
	prev_dualobj_(rhs.prev_dualobj_),
	nstalls_(rhs.nstalls_) {
	d_ = rhs.d_;
	p_ = rhs.p_;
	primal_si_.reset(rhs.primal_si_->clone());
}

DwBundleDual& DwBundleDual::operator =(const DwBundleDual& rhs) {
	DwMaster::operator =(rhs);
	v_ = rhs.v_;
	counter_ = rhs.counter_;
	u_ = rhs.u_;
	eps_ = rhs.eps_;
	absp_ = rhs.absp_;
	alpha_ = rhs.alpha_;
	linerr_ = rhs.linerr_;
	prev_dualobj_ = rhs.prev_dualobj_;
	nstalls_ = rhs.nstalls_;
	d_ = rhs.d_;
	p_ = rhs.p_;
	primal_si_.reset(rhs.primal_si_->clone());
	return *this;
}

DwBundleDual::~DwBundleDual() {
}

DSP_RTN_CODE DwBundleDual::solve() {
	BGN_TRY_CATCH

	itercnt_ = 0;
	t_start_ = CoinGetTimeOfDay();
	t_total_ = 0.0;
	t_master_ = 0.0;
	t_colgen_ = 0.0;
	status_ = DSP_STAT_FEASIBLE;

	/** clear logs */
	log_time_.clear();
	log_bestdual_bounds_.clear();

	/** update quadratic term */
	u_ = par_->getDblParam("DW/INIT_CENTER");
	counter_ = 0;
	eps_ = COIN_DBL_MAX;
	updateCenter(u_);

	/** initial price to generate columns */
	bestdualsol_.resize(nrows_, 0.0);
	dualsol_ = bestdualsol_;
	std::fill(dualsol_.begin(), dualsol_.begin() + nrows_conv_, COIN_DBL_MAX);

	/** generate initial columns */
	double stime = CoinGetTimeOfDay();
	DSP_RTN_CHECK_RTN_CODE(generateCols());
	t_colgen_ += CoinGetTimeOfDay() - stime;

	/** subproblem solution may declare infeasibility. */
	for (auto st = status_subs_.begin(); st != status_subs_.end(); st++)
		if (*st == DSP_STAT_PRIM_INFEASIBLE) {
			status_ = DSP_STAT_PRIM_INFEASIBLE;
			message_->print(1, "Subproblem solution is infeasible.\n");
			break;
		}

	/**
	 * The codes below are experimental to see if deactivating some dual variables would help convergence.
	 */
#if 0
	/** deactivate some dual variables (by fixed to zeros) */
	std::vector<pairIntDbl> weight;
	const CoinPackedMatrix* mat = si_->getMatrixByCol();
	for (int j = nrows_conv_; j < si_->getNumCols(); ++j) {
		const CoinShallowPackedVector col = mat->getVector(j);
		double val = 0.0;
		for (int i = 0; i < col.getNumElements(); ++i)
			val -= col.getElements()[i];
		weight.push_back(std::make_pair(j,val));
	}
	std::sort(weight.begin(), weight.end(), compPair);

	/** FIXME: Let's try 90% activation */
	int ndeactive = nrows_orig_ - floor(nrows_orig_*0.9);
	for (int j = 0; j < ndeactive; ++j) {
		//printf("Fixed column(%d) bounds to zeros.\n", weight[j].first);
		si_->setColBounds(weight[j].first, 0.0, 0.0);
	}
#endif
	/** check time limit */
	t_total_ = CoinGetTimeOfDay() - t_start_;
	if (time_remains_ < t_total_) {
		message_->print(1, "Time limit reached.\n");
		status_ = DSP_STAT_LIM_ITERorTIME;
	}

	if (status_ == DSP_STAT_FEASIBLE) {
		message_->print(1, "Generated %u initial columns. Initial dual bound %.12e\n", ngenerated_, -dualobj_);
		if (dualobj_ < bestdualobj_) {
			bestdualobj_ = dualobj_;
			bestdualsol_ = dualsol_;
		}
		DSP_RTN_CHECK_RTN_CODE(gutsOfSolve());
	}

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)

	return DSP_RTN_OK;
}

DSP_RTN_CODE DwBundleDual::createProblem() {
	BGN_TRY_CATCH

	clbd_node_ = clbd_orig_;
	cubd_node_ = cubd_orig_;

	DSP_RTN_CHECK_RTN_CODE(createPrimalProblem());
	DSP_RTN_CHECK_RTN_CODE(createDualProblem());

	/** always phase2 */
	phase_ = 2;

	/** maximization in dual */
	bestdualobj_ = COIN_DBL_MAX;

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)

	return DSP_RTN_OK;
}

DSP_RTN_CODE DwBundleDual::createPrimalProblem() {
	BGN_TRY_CATCH

	/** create column-wise matrix and set number of rows */
	std::shared_ptr<CoinPackedMatrix> mat(new CoinPackedMatrix(true, 0, 0));
	mat->setDimensions(nrows_, 0);

	/** row bounds */
	std::vector<double> rlbd(nrows_);
	std::vector<double> rubd(nrows_);
	std::fill(rlbd.begin(), rlbd.begin() + nrows_conv_, 1.0);
	std::fill(rubd.begin(), rubd.begin() + nrows_conv_, 1.0);
	std::copy(rlbd_orig_.begin(), rlbd_orig_.begin() + nrows_orig_, rlbd.begin() + nrows_conv_);
	std::copy(rubd_orig_.begin(), rubd_orig_.begin() + nrows_orig_, rubd.begin() + nrows_conv_);

	/** create solver */
	primal_si_.reset(new OsiCpxSolverInterface());

	/** load problem data */
	primal_si_->loadProblem(*mat, NULL, NULL, NULL, &rlbd[0], &rubd[0]);

	/** set display */
	primal_si_->messageHandler()->setLogLevel(0);
	DSPdebug(primal_si_->messageHandler()->setLogLevel(1));

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)

	return DSP_RTN_OK;
}

void DwBundleDual::initDualSolver(
		const CoinPackedMatrix& m, 
		std::vector<double>& clbd, 
		std::vector<double>& cubd, 
		std::vector<double>& obj) {
	/** create solver */
	si_ = new OsiCpxSolverInterface();
	OsiCpxSolverInterface* cpx = dynamic_cast<OsiCpxSolverInterface*>(si_);
	/** set display */
	si_->messageHandler()->setLogLevel(std::max(-1,par_->getIntParam("LOG_LEVEL")-4));
	/** load problem data */
	si_->loadProblem(m, &clbd[0], &cubd[0], &obj[0], NULL, NULL);
}

DSP_RTN_CODE DwBundleDual::createDualProblem() {
	BGN_TRY_CATCH

	/** master problem */
	std::shared_ptr<CoinPackedMatrix> mat(nullptr);
	std::vector<double> clbd(nrows_), cubd(nrows_), obj(nrows_);

	/** necessary dual variables */
	dualsol_.resize(nrows_);
	std::fill(dualsol_.begin(), dualsol_.begin() + nrows_conv_, COIN_DBL_MAX);
	std::fill(dualsol_.begin() + nrows_conv_, dualsol_.end(), 0.0);
	bestdualsol_ = dualsol_;

	/** other initialization */
	p_.reserve(nrows_orig_+nrows_branch_);
	d_.reserve(nrows_orig_+nrows_branch_);
	std::fill(p_.begin(), p_.end(), 0.0);
	std::fill(d_.begin(), d_.end(), 0.0);

	/** create row-wise matrix and set number of rows */
	mat.reset(new CoinPackedMatrix(false, 0, 0));
	mat->setDimensions(0, nrows_);

	std::fill(clbd.begin(), clbd.begin() + nrows_conv_, -COIN_DBL_MAX);
	std::fill(cubd.begin(), cubd.begin() + nrows_conv_, +COIN_DBL_MAX);
	std::fill(obj.begin(), obj.begin() + nrows_conv_, -1.0);
	for (int i = 0; i < nrows_orig_; ++i) {
		clbd[nrows_conv_+i] = 0.0;
		cubd[nrows_conv_+i] = 0.0;
		obj[nrows_conv_+i] = -u_*bestdualsol_[nrows_conv_+i];
		if (rlbd_orig_[i] > -1.0e+20) {
			cubd[nrows_conv_+i] = COIN_DBL_MAX;
			obj[nrows_conv_+i] -= rlbd_orig_[i];
		}
		if (rubd_orig_[i] < 1.0e+20) {
			clbd[nrows_conv_+i] = -COIN_DBL_MAX;
			obj[nrows_conv_+i] -= rubd_orig_[i];
		}
	}

	/** initialize external solver */
	initDualSolver(*mat, clbd, cubd, obj);

	/** set quadratic objective term */
	updateCenter(u_);

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)

	return DSP_RTN_OK;
}

DSP_RTN_CODE DwBundleDual::updateCenter(double penalty) {
	double coef;
	OsiCpxSolverInterface* cpx = dynamic_cast<OsiCpxSolverInterface*>(si_);
	const double* rlbd = primal_si_->getRowLower();
	const double* rubd = primal_si_->getRowUpper();

	if (cpx) {
		for (int j = nrows_conv_; j < nrows_; ++j) {
			/** objective coefficient */
			coef = -penalty*bestdualsol_[j];
			if (rlbd[j] > -1.0e+20)
				coef -= rlbd[j];
			if (rubd[j] < 1.0e+20)
				coef -= rubd[j];
			si_->setObjCoeff(j, coef);
			CPXchgqpcoef(cpx->getEnvironmentPtr(), cpx->getLpPtr(OsiCpxSolverInterface::KEEPCACHED_ALL), j, j, penalty);
		}
	} else {
		CoinError("This supports OsiCpxSolverInterface only.", "updateQuadratic", "DwBundle");
		return DSP_RTN_ERR;
	}
	return DSP_RTN_OK;
}

DSP_RTN_CODE DwBundleDual::callMasterSolver() {
	OsiCpxSolverInterface* cpx = dynamic_cast<OsiCpxSolverInterface*>(si_);
	if (!cpx) {
		CoinError("Failed to case Osi to OsiCpx", "solveMaster", "DwBundle");
		return DSP_RTN_ERR;
	} else {
		CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_THREADS, par_->getIntParam("NUM_CORES"));
		//CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_BARORDER, 3);
		//CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_BARMAXCOR, par_->getIntParam("CPX_PARAM_BARMAXCOR"));
		//CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_BARALG, par_->getIntParam("CPX_PARAM_BARALG"));
		//CPXsetdblparam(cpx->getEnvironmentPtr(), CPX_PARAM_BAREPCOMP, 1e-6);
		/** use dual simplex for QP, which is numerically much more stable than Barrier */
		CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_QPMETHOD, 2);
		CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_DEPIND, par_->getIntParam("CPX_PARAM_DEPIND"));
		CPXsetintparam(cpx->getEnvironmentPtr(), CPX_PARAM_NUMERICALEMPHASIS, par_->getIntParam("CPX_PARAM_NUMERICALEMPHASIS"));
	}

	CPXqpopt(cpx->getEnvironmentPtr(), cpx->getLpPtr(OsiCpxSolverInterface::KEEPCACHED_PROBLEM));
	int cpxstat = CPXgetstat(cpx->getEnvironmentPtr(), cpx->getLpPtr(OsiCpxSolverInterface::KEEPCACHED_ALL));
	message_->print(5, "CPLEX status %d\n", cpxstat);
	switch (cpxstat) {
	case CPX_STAT_OPTIMAL:
	case CPX_STAT_NUM_BEST:
		status_ = DSP_STAT_OPTIMAL;
		break;
	case CPX_STAT_INFEASIBLE:
		status_ = DSP_STAT_DUAL_INFEASIBLE;
		message_->print(0, "Unexpected CPLEX status %d\n", cpxstat);
		break;
	case CPX_STAT_UNBOUNDED:
	case CPX_STAT_INForUNBD:
		status_ = DSP_STAT_PRIM_INFEASIBLE;
		break;
	case CPX_STAT_ABORT_OBJ_LIM:
	case CPX_STAT_ABORT_PRIM_OBJ_LIM:
		status_ = DSP_STAT_LIM_PRIM_OBJ;
		break;
	case CPX_STAT_ABORT_DUAL_OBJ_LIM:
		status_ = DSP_STAT_LIM_DUAL_OBJ;
		break;
	case CPX_STAT_ABORT_IT_LIM:
	case CPX_STAT_ABORT_TIME_LIM:
		status_ = DSP_STAT_LIM_ITERorTIME;
		break;
	case CPX_STAT_ABORT_USER:
		status_ = DSP_STAT_ABORT;
		break;
	default:
		message_->print(0, "Unexpected CPLEX status %d\n", cpxstat);
		status_ = DSP_STAT_UNKNOWN;
		break;
	}
	return status_;
}

void DwBundleDual::assignMasterSolution(std::vector<double>& sol) {
	sol.assign(si_->getColSolution(), si_->getColSolution() + si_->getNumCols());
}

DSP_RTN_CODE DwBundleDual::solveMaster() {
	BGN_TRY_CATCH

	/** get previous objective */
	prev_dualobj_ = dualobj_;

	/** call solver */
	status_ = callMasterSolver();

	/** status_ must be set at this point. */

	switch(status_) {
	case DSP_STAT_OPTIMAL:
	case DSP_STAT_FEASIBLE:
	case DSP_STAT_LIM_ITERorTIME: {

		assignMasterSolution(dualsol_);
		//DspMessage::printArray(nrows_, &dualsol_[0]);

		d_.resize(nrows_-nrows_conv_);
		p_.resize(nrows_-nrows_conv_);
		double polyapprox = 0.0;
		absp_ = 0.0;
		for (int j = nrows_conv_; j < nrows_; ++j) {
			d_[j-nrows_conv_] = dualsol_[j] - bestdualsol_[j];
			p_[j-nrows_conv_] = -u_ * d_[j-nrows_conv_];
			polyapprox += u_ * dualsol_[j] * (bestdualsol_[j] - 0.5 * dualsol_[j]);
			absp_ += fabs(p_[j-nrows_conv_]);
			//printf("j %d: dualsol %+e, bestdualsol %+e, d %+e, p %+e, polyapprox %+e\n",
			//		j, dualsol_[j], bestdualsol_[j], d_[j-nrows_conv_], p_[j-nrows_conv_], polyapprox);
		}
//		printf("u*dualsol*(bestdualsol-0.5*dualsol) = %+e\n", polyapprox);
		polyapprox += getObjValue();
		//printf("polyapprox %e\n", polyapprox);

		v_ = polyapprox - bestdualobj_;

		/** adjust v if subproblem was not solved to optimality */
		for (auto it = status_subs_.begin(); it != status_subs_.end(); it++)
			if (*it != DSP_STAT_OPTIMAL) {
				v_ = std::min(v_, -1.0e-4);
				break;
			}

		alpha_ = -v_;
		for (int j = nrows_conv_; j < nrows_; ++j)
			alpha_ += p_[j-nrows_conv_] * d_[j-nrows_conv_];

		/** get primal solutions by solving the primal master */
		DSPdebug(primal_si_->writeMps("PrimMaster"));
		primal_si_->resolve();
		if (primal_si_->isProvenOptimal()) {
			primobj_ = primal_si_->getObjValue();
			primsol_.assign(primal_si_->getColSolution(), primal_si_->getColSolution() + primal_si_->getNumCols());
		} else {
			message_->print(5, "  The primal master could not be solved to optimality.\n");
			primobj_ = COIN_DBL_MAX;
			//primsol_.assign(si_->getRowPrice(), si_->getRowPrice() + si_->getNumRows());
		}

		absgap_ = fabs(primobj_+bestdualobj_);
		relgap_ = absgap_/(1.0e-10+fabs(bestdualobj_));
		break;
	}
	default:
		break;
	}

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)
	return DSP_RTN_OK;
}

DSP_RTN_CODE DwBundleDual::updateModel() {
	BGN_TRY_CATCH

	double u = u_, newu;

	/** descent test */
	bool foundBetter = false;
	if (dualobj_ <= bestdualobj_ + mL_ * v_) {
		message_->print(2, "  Serious step: best dual %+e -> %e\n", -bestdualobj_, -dualobj_);
		foundBetter = true;
		/** reset subproblem time increment */
		worker_->resetTimeIncrement();
	}

	/** update weight */
	if (foundBetter) {
		if (dualobj_ <= bestdualobj_ + mR_ * v_ && counter_ > 0)
			u = 2 * u_ * (1 - (dualobj_ - bestdualobj_) / v_);
		else if (counter_ > 3)
			u = 0.5 * u_;
		newu = std::max(std::max(u, 0.1*u_), umin_);
		eps_ = std::max(eps_, -2*v_);
		counter_ = std::max(counter_+1,1);
		if (u_ != newu)
			counter_ = 1;
		else
			counter_++;
		u_ = newu;
		bestdualobj_ = dualobj_;
		bestdualsol_ = dualsol_;
		nstalls_ = 0;
	} else {
		/** increment number of iterations making no progress */
		nstalls_ = fabs(prev_dualobj_-dualobj_) < 1.0e-6 ? nstalls_ + 1 : 0;
		if (nstalls_ > 0)
			message_->print(3, "number of stalls: %d\n", nstalls_);

		eps_ = std::min(eps_, absp_ + alpha_);
		if (-linerr_ > std::max(eps_, -10*v_) && counter_ < -3)
			u = 2 * u_ * (1 - (dualobj_ - bestdualobj_) / v_);
//		printf("#### linerr_ %+e, eps_ %+e, -10*v_ %+e, u %+e\n", -linerr_, eps_, -10*v_, u);
		newu = std::max(std::min(u, 10*u_), umin_);
		counter_ = std::min(counter_-1,-1);
		if (u_ != newu)
			counter_ = -1;
		else if (ngenerated_ == 0 || nstalls_ > 3 ||
				(primobj_ >= 1.0e+20 && v_ >= -par_->getDblParam("DW/MIN_INCREASE"))) {
			newu = std::max(0.1*u_, umin_);
		}
		u_ = newu;
	}
	updateCenter(u_);

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)
	return DSP_RTN_OK;
}

bool DwBundleDual::terminationTest() {
	BGN_TRY_CATCH

	if (-bestdualobj_ >= bestprimobj_)
		return true;

	if (primobj_ < 1.0e+20 && relgap_ <= par_->getDblParam("DW/GAPTOL"))
		return true;

	if (primobj_ < 1.0e+20 && v_ >= -par_->getDblParam("DW/MIN_INCREASE"))
		return true;

	if (iterlim_ <= itercnt_ || nstalls_ >= 30) {
		message_->print(3, "Warning: Iteration limit reached.\n");
		status_ = DSP_STAT_LIM_ITERorTIME;
		return true;
	}

	if (time_remains_ < t_total_) {
		message_->print(3, "Warning: Time limit reached.\n");
		status_ = DSP_STAT_LIM_ITERorTIME;
		return true;
	}

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)
	return false;
}

DSP_RTN_CODE DwBundleDual::addCols(
		const double* piA,                   /**< [in] pi^T A */
		std::vector<int>& indices,           /**< [in] subproblem indices corresponding to cols*/
		std::vector<int>& statuses,          /**< [in] subproblem solution status */
		std::vector<double>& cxs,            /**< [in] solution times original objective coefficients */
		std::vector<double>& objs,           /**< [in] subproblem objective values */
		std::vector<CoinPackedVector*>& sols /**< [in] subproblem solutions */) {
	return addRows(indices, statuses, cxs, objs, sols);
}

DSP_RTN_CODE DwBundleDual::addRows(
		std::vector<int>& indices,           /**< [in] subproblem indices corresponding to cols*/
		std::vector<int>& statuses,          /**< [in] subproblem solution status */
		std::vector<double>& cxs,            /**< [in] solution times original objective coefficients */
		std::vector<double>& objs,           /**< [in] subproblem objective values */
		std::vector<CoinPackedVector*>& sols /**< [in] subproblem solutions */) {
	CoinPackedVector cutvec;
	double cutcoef, cutrhs;

	BGN_TRY_CATCH

	/** allocate memory */
	std::vector<double> Ax(nrows_orig_, 0.0);
	cutvec.reserve(nrows_);

	/** reset counter */
	ngenerated_ = 0;

	/** linearization error */
	linerr_ = 0.0;
	for (int i = nrows_conv_; i < nrows_; ++i) {
		if (primal_si_->getRowLower()[i] > -1.0e+20)
			linerr_ += primal_si_->getRowLower()[i] * d_[i-nrows_conv_];
		if (primal_si_->getRowUpper()[i] < 1.0e+20)
			linerr_ += primal_si_->getRowUpper()[i] * d_[i-nrows_conv_];
	}

	for (unsigned int s = 0; s < indices.size(); ++s) {
		int sind = indices[s]; /**< actual subproblem index */

		/** retrieve subproblem solution */
		const CoinPackedVector* x = sols[s];
#if 1
		for (int i = 0; i < x->getNumElements(); ++i) {
			int j = x->getIndices()[i];
			if (j >= ncols_orig_) break;
			if (x->getElements()[i] - cubd_node_[j] > 1.0e-6 ||
				x->getElements()[i] - clbd_node_[j] < -1.0e-6)
				printf("violation in subproblem %d (col %d, status %d): %+e <= %+e <= %+e\n",
						sind, j, statuses[s], clbd_node_[j], x->getElements()[i], cubd_node_[j]);
		}
#endif

		/** take A x^k */
		mat_orig_->times(*x, &Ax[0]);

		/** clear cut vector */
		cutvec.clear();

		/** original constraints */
		if (statuses[s] != DSP_STAT_DUAL_INFEASIBLE)
			cutvec.insert(sind, 1.0);
		for (int i = 0; i < nrows_orig_; ++i) {
			cutcoef = Ax[i];
			linerr_ -= cutcoef * d_[i];
			if (fabs(cutcoef) > 1.0e-10)
				cutvec.insert(nrows_conv_+i, cutcoef);
		}
		cutrhs = cxs[s];

		/** branching objects */
		for (int i = 0; i < nrows_branch_; ++i) {
			int j = branch_row_to_col_[nrows_core_ + i];
			int sparse_index = x->findIndex(j);
			if (sparse_index == -1) continue;
			cutcoef = x->getElements()[sparse_index];
			linerr_ -= cutcoef * d_[nrows_orig_+i];
			if (fabs(cutcoef) > 1.0e-10)
				cutvec.insert(nrows_core_+i, cutcoef);
		}

		DSPdebugMessage("subproblem %d: status %d, objective %+e, violation %+e\n", sind, statuses[s], objs[s], dualsol_[sind] - objs[s]);
		if (statuses[s] == DSP_STAT_UNKNOWN)
			throw "Unknown subproblem solution status";

		if (dualsol_[sind] > objs[s] + 1.0e-8 ||
				statuses[s] == DSP_STAT_DUAL_INFEASIBLE) {
#ifdef DSP_DEBUG_MORE
			DspMessage::printArray(&cutvec);
			printf("cutrhs %+e\n", cutrhs);
#endif

			/** add row to the dual master */
			addDualRow(cutvec, -COIN_DBL_MAX, cutrhs);

			/** add column to the primal master */
			primal_si_->addCol(cutvec, 0.0, COIN_DBL_MAX, cutrhs);

			/** store columns */
			cols_generated_.push_back(new DwCol(sind, *x, cutvec, cutrhs, 0.0, COIN_DBL_MAX));
			ngenerated_++;
		}
	}
	DSPdebugMessage("Number of columns in the pool: %u\n", cols_generated_.size());

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)

	return DSP_RTN_OK;
}

DSP_RTN_CODE DwBundleDual::getLagrangianBound(
		std::vector<double>& objs /**< [in] subproblem objective values */) {
	BGN_TRY_CATCH

	/** calculate lower bound */
	dualobj_ = -std::accumulate(objs.begin(), objs.end(), 0.0);
	DSPdebugMessage("Sum of subproblem objectives: %+e\n", -dualobj_);
	const double* rlbd = primal_si_->getRowLower();
	const double* rubd = primal_si_->getRowUpper();
	for (int j = nrows_conv_; j < nrows_; ++j) {
		if (rlbd[j] > -1.0e+20)
			dualobj_ -= dualsol_[j] * rlbd[j];
		else if (rubd[j] < 1.0e+20)
			dualobj_ -= dualsol_[j] * rubd[j];
	}

	/** linearization error */
	linerr_ += bestdualobj_ - dualobj_;

	END_TRY_CATCH_RTN(;,DSP_RTN_ERR)

	return DSP_RTN_OK;
}

void DwBundleDual::setBranchingObjects(const DspBranch* branchobj) {
	/** shouldn't be null */
	if (branchobj == NULL)
		return;

	BGN_TRY_CATCH

	/** restore column bounds */
	clbd_node_ = clbd_orig_;
	cubd_node_ = cubd_orig_;

	/** update column bounds at the current node */
	for (unsigned j = 0; j < branchobj->index_.size(); ++j) {
		clbd_node_[branchobj->index_[j]] = branchobj->lb_[j];
		cubd_node_[branchobj->index_[j]] = branchobj->ub_[j];
	}

	/** remove all columns in the primal master */
	removeAllPrimCols();

	/** remove all rows in the dual master */
	removeAllDualRows();

	/** remove branching rows and columns */
	removeBranchingRowsCols();

	/** add branching rows and columns */
	addBranchingRowsCols(branchobj);

	/** apply column bounds */
	std::vector<int> ncols_inds(ncols_orig_);
	std::iota(ncols_inds.begin(), ncols_inds.end(), 0);
	worker_->setColBounds(ncols_orig_, &ncols_inds[0], &clbd_node_[0], &cubd_node_[0]);

	/** set known best bound */
	bestdualobj_ = COIN_DBL_MAX;
	bestdualsol_ = branchobj->dualsol_;

	END_TRY_CATCH(;)
}

void DwBundleDual::removeAllPrimCols() {
	std::vector<int> delcols(primal_si_->getNumCols());
	std::iota(delcols.begin(), delcols.end(), 0.0);
	primal_si_->deleteCols(primal_si_->getNumCols(), &delcols[0]);
}

void DwBundleDual::removeAllDualRows() {
	std::vector<int> delrows(si_->getNumRows());
	std::iota(delrows.begin(), delrows.end(), 0.0);
	si_->deleteRows(si_->getNumRows(), &delrows[0]);
}

void DwBundleDual::removeBranchingRowsCols() {
	if (nrows_branch_ > 0) {
		std::vector<int> delinds(nrows_branch_);
		std::iota(delinds.begin(), delinds.end(), nrows_core_);
		primal_si_->deleteRows(nrows_branch_, &delinds[0]);
		si_->deleteCols(nrows_branch_, &delinds[0]);
		nrows_branch_ = 0;
	}
}

void DwBundleDual::addBranchingRowsCols(const DspBranch* branchobj) {
#define BRANCH_ROW
#ifdef BRANCH_ROW
	for (unsigned j = 0; j < branchobj->index_.size(); ++j) {
		if (branchobj->lb_[j] > clbd_orig_[branchobj->index_[j]]) {
			branch_row_to_col_[nrows_core_ + nrows_branch_] = branchobj->index_[j];
			primal_si_->addRow(0, NULL, NULL, branchobj->lb_[j], COIN_DBL_MAX);
			si_->addCol(0, NULL, NULL, 0.0, COIN_DBL_MAX, branchobj->lb_[j]);
			nrows_branch_++;
		}
		if (branchobj->ub_[j] < cubd_orig_[branchobj->index_[j]]) {
			branch_row_to_col_[nrows_core_ + nrows_branch_] = branchobj->index_[j];
			primal_si_->addRow(0, NULL, NULL, -COIN_DBL_MAX, branchobj->ub_[j]);
			si_->addCol(0, NULL, NULL, -COIN_DBL_MAX, 0.0, branchobj->ub_[j]);
			nrows_branch_++;
		}
	}
#endif

	/** update the number of rows */
	nrows_ = nrows_core_ + nrows_branch_;

	std::vector<int> rind; /**< column indices for branching row */
	std::vector<double> rval; /** column values for branching row */

	/** add branching columns and rows */
	for (auto it = cols_generated_.begin(); it != cols_generated_.end(); it++) {
		(*it)->active_ = true;
		for (unsigned j = 0, i = 0; j < branchobj->index_.size(); ++j) {
			int sparse_index = (*it)->x_.findIndex(branchobj->index_[j]);
			double val = 0.0;
			if (sparse_index > -1)
				val = (*it)->x_.getElements()[sparse_index];
			if (val < branchobj->lb_[j] || val > branchobj->ub_[j]) {
				(*it)->active_ = false;
				break;
			}
		}

		if ((*it)->active_) {
			/** create a column for core rows */
			rind.clear();
			rval.clear();
			for (int i = 0; i < (*it)->col_.getNumElements(); ++i) {
				if ((*it)->col_.getIndices()[i] < nrows_core_) {
					rind.push_back((*it)->col_.getIndices()[i]);
					rval.push_back((*it)->col_.getElements()[i]);
				}
			}

#ifdef BRANCH_ROW
			/** append column elements for the branching rows */
			for (unsigned j = 0, i = 0; j < branchobj->index_.size(); ++j) {
				int sparse_index = (*it)->x_.findIndex(branchobj->index_[j]);
				double val = 0.0;
				if (sparse_index > -1)
					val = (*it)->x_.getElements()[sparse_index];
				if (branchobj->lb_[j] > clbd_orig_[branchobj->index_[j]]) {
					if (fabs(val) > 1.0e-10) {
						rind.push_back(nrows_core_+i);
						rval.push_back(val);
					}
					i++;
				}
				if (branchobj->ub_[j] < cubd_orig_[branchobj->index_[j]]) {
					if (fabs(val) > 1.0e-10) {
						rind.push_back(nrows_core_+i);
						rval.push_back(val);
					}
					i++;
				}
			}
#endif

			/** assign the core-row column */
			(*it)->col_.setVector(rind.size(), &rind[0], &rval[0]);

			/** add row and column */
			addDualRow((*it)->col_, -COIN_DBL_MAX, (*it)->obj_);
			primal_si_->addCol((*it)->col_, 0.0, COIN_DBL_MAX, (*it)->obj_);
		}
	}
	message_->print(3, "Appended dynamic columns in the master (%d / %u cols).\n", si_->getNumRows(), cols_generated_.size());
}

void DwBundleDual::printIterInfo() {
	message_->print(1, "Iteration %3d: DW Bound %+e, ", itercnt_, primobj_);
	message_->print(3, "Dual %+e, Approx %+e, ", -dualobj_, -bestdualobj_-v_);
	message_->print(1, "Best Dual %+e ", -bestdualobj_);
	if (relgap_ < 1000)
		message_->print(1, "(gap %.2f %%), ", relgap_*100);
	else
		message_->print(1, "(gap Large %%), ");
	message_->print(1, "nrows %d, ncols %d, ", si_->getNumRows(), si_->getNumCols());
	message_->print(1, "timing (total %.2f, master %.2f, gencols %.2f), statue %d\n",
			t_total_, t_master_, t_colgen_, status_);
	message_->print(3, "  predicted ascent %+e, |p| %+e, alpha %+e, linerr %+e, eps %+e, u %+e, counter %d\n",
			-v_, absp_, alpha_, -linerr_, eps_, u_, counter_);

	/** log */
	log_time_.push_back(CoinGetTimeOfDay());
	log_bestdual_bounds_.push_back(-bestdualobj_);
}