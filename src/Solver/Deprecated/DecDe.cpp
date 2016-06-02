/*
 * DecDe.cpp
 *
 *  Created on: Sep 24, 2014
 *      Author: kibaekkim, ctjandra
 */

//#define DSP_DEBUG
//#define DSP_TIMING

/** DSP */
#include "Solver/DecDe.h"

#include <Utility/DspMessage.h>
#include "SolverInterface/SolverInterfaceClp.h"
#include "SolverInterface/SolverInterfaceScip.h"
#include "Model/TssModel.h"

DecDe::DecDe():
DecSolver(),
si_(NULL)
{
	/** nothing to do */
}

DecDe::~DecDe()
{
	FREE_PTR(si_)
}

/** solve */
DSP_RTN_CODE DecDe::solve()
{
#define FREE_MEMORY       \
	FREE_PTR(mat)         \
	FREE_ARRAY_PTR(clbd)  \
	FREE_ARRAY_PTR(cubd)  \
	FREE_ARRAY_PTR(ctype) \
	FREE_ARRAY_PTR(obj)   \
	FREE_ARRAY_PTR(rlbd)  \
	FREE_ARRAY_PTR(rubd)

	assert(model_);

	/** model info */
	CoinPackedMatrix * mat = NULL;
	double * clbd   = NULL;
	double * cubd   = NULL;
	double * obj    = NULL;
	char *   ctype  = NULL;
	double * rlbd   = NULL;
	double * rubd   = NULL;

	BGN_TRY_CATCH

	double stime;

#ifdef DSP_TIMING
	stime = CoinGetTimeOfDay();
#endif
	/** get De model */
	DSP_RTN_CHECK_THROW(
			model_->getFullModel(mat, clbd, cubd, ctype, obj, rlbd, rubd),
			"getFullModel", "DecModel");
#ifdef DSP_TIMING
	printf("Constructed extensive form data, elapsed time %.2f seconds\n", CoinGetTimeOfDay() - stime);
	stime = CoinGetTimeOfDay();
#endif

	int nIntegers = model_->getNumIntegers();

	if (model_->isStochastic())
	{
		TssModel * tssModel;
		try
		{
			tssModel = dynamic_cast<TssModel *>(model_);
		}
		catch (const std::bad_cast& e)
		{
			printf("Model claims to be stochastic when it is not");
			return DSP_RTN_ERR;
		}

		/** relax integrality? */
		if (parRelaxIntegrality_[0])
		{
			for (int j = 0; j < tssModel->getNumCols(0); ++j)
			{
				if (ctype[j] != 'C')
					nIntegers--;
				ctype[j] = 'C';
			}
		}
		if (parRelaxIntegrality_[1])
		{
			for (int j = 0; j < tssModel->getNumCols(1); ++j)
			{
				if (ctype[tssModel->getNumCols(0) + j] != 'C')
					nIntegers--;
			}
			CoinFillN(ctype + tssModel->getNumCols(0), tssModel->getNumScenarios() * tssModel->getNumCols(1), 'C');
		}
	}
	else
	{
		if (parRelaxIntegrality_[0] || parRelaxIntegrality_[1])
		{
			for (int j = 0; j < mat->getNumCols(); j++)
			{
				if (ctype[j] != 'C')
					nIntegers--;
				ctype[j] = 'C';
			}
		}
	}

	if (nIntegers > 0)
	{
		si_ = new SolverInterfaceScip(par_);
		/** print level */
		si_->setPrintLevel(CoinMin(par_->getIntParam("LOG_LEVEL") + 2, 5));
	}
	else
	{
		si_ = new SolverInterfaceClp(par_);
		/** print level */
		si_->setPrintLevel(parLogLevel_);
	}

	/** load problem */
	si_->loadProblem(mat, clbd, cubd, obj, ctype, rlbd, rubd, "DecDe");
#ifdef DSP_TIMING
	printf("Loaded problem to solver, elapsed time %.2f seconds\n", CoinGetTimeOfDay() - stime);
#endif

	/** time limit */
	si_->setTimeLimit(parScipTimeLim_);

	/** set iteration limit */
	si_->setIterLimit(parIterLim_);

	/** set node limit */
	si_->setNodeLimit(parNodeLim_);

	/** solution time */
	stime = clockType_ ? CoinGetTimeOfDay() : CoinCpuTime();

	/** solve */
	si_->solve();

	/** solution time */
	solutionTime_ = (clockType_ ? CoinGetTimeOfDay() : CoinCpuTime()) - stime;

	/** solution status */
	status_ = si_->getStatus();

	/** get solutions */
	if (status_ == DSP_STAT_OPTIMAL ||
		status_ == DSP_STAT_STOPPED_TIME ||
		status_ == DSP_STAT_STOPPED_NODE ||
		status_ == DSP_STAT_STOPPED_GAP)
	{
		/** objective bounds */
		primalBound_ = si_->getPrimalBound();
		dualBound_ = si_->getDualBound();

		/** solution */
		assert(solution_);
		if (si_->getSolution())
			CoinCopyN(si_->getSolution(), si_->getNumCols(), solution_);

		/** statistics */
		numIterations_ = si_->getIterationCount();
		numNodes_ = si_->getNumNodes();
	}

	/** save memory */
	FREE_MEMORY

	END_TRY_CATCH_RTN(FREE_MEMORY,DSP_RTN_ERR)

	return DSP_RTN_OK;

#undef FREE_MEMORY
}
