/*
 * DwBranchNonant.cpp
 *
 *  Created on: May 8, 2018
 *      Author: Kibaek Kim
 */

//#define DSP_DEBUG
#include <DantzigWolfe/DwBranchNonant2.h>
#include <Model/TssModel.h>

void DwBranchNonant2::getRefSol(std::vector<double>& refsol) {
	refsol = model_->getPrimalSolution();
}

void DwBranchNonant2::getDevSol(std::vector<double>& refsol, std::vector<double>& devsol) {
	devsol.resize(tss_->getNumCols(0), 0.0);
	std::vector<double> maxsol(tss_->getNumCols(0), -COIN_DBL_MAX);
	std::vector<double> minsol(tss_->getNumCols(0), +COIN_DBL_MAX);

	for (auto it = master_->cols_generated_.begin(); it != master_->cols_generated_.end(); it++) {
		if ((*it)->active_) {
			int master_index = (*it)->master_index_;
			if (fabs(master_->getPrimalSolution()[master_index]) > 1.0e-10) {
				for (int i = 0; i < (*it)->x_.getNumElements(); ++i) {
					if ((*it)->x_.getIndices()[i] < tss_->getNumCols(0) * tss_->getNumScenarios()) {
						int k = (*it)->x_.getIndices()[i] % tss_->getNumCols(0);
						maxsol[k] = CoinMax(maxsol[k], (*it)->x_.getElements()[i]);
						minsol[k] = CoinMin(minsol[k], (*it)->x_.getElements()[i]);
					}
				}
			}
		}
	}
	for (int k = 0; k < tss_->getNumCols(0); ++k)
		devsol[k] = maxsol[k] - minsol[k];

	/** The following calculates the dispersion as a variance. */
#if 0
	for (auto it = master_->cols_generated_.begin(); it != master_->cols_generated_.end(); it++)
		if ((*it)->active_) {
			int master_index = (*it)->master_index_;
			if (fabs(master_->getPrimalSolution()[master_index]) > 1.0e-10) {
				for (int i = 0; i < (*it)->x_.getNumElements(); ++i) {
					if ((*it)->x_.getIndices()[i] < tss_->getNumCols(0) * tss_->getNumScenarios()) {
						int k = (*it)->x_.getIndices()[i] % tss_->getNumCols(0);
						devsol[k] += pow((*it)->x_.getElements()[i] - refsol[k],2) * master_->getPrimalSolution()[master_index];
					}
				}
			}
		}
#endif
}
