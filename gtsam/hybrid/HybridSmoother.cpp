/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    HybridSmoother.cpp
 * @brief   An incremental smoother for hybrid factor graphs
 * @author  Varun Agrawal
 * @date    October 2022
 */

#include <gtsam/hybrid/HybridSmoother.h>

#include <algorithm>
#include <unordered_set>

// #define DEBUG_SMOOTHER
namespace gtsam {

/* ************************************************************************* */
Ordering HybridSmoother::getOrdering(const HybridGaussianFactorGraph &factors,
                                     const KeySet &lastKeysToEliminate) {
  // Get all the discrete keys from the factors
  KeySet allDiscrete = factors.discreteKeySet();

  // Create KeyVector with continuous keys followed by discrete keys.
  KeyVector lastKeys;

  // Insert continuous keys first.
  for (auto &k : lastKeysToEliminate) {
    if (!allDiscrete.exists(k)) {
      lastKeys.push_back(k);
    }
  }

  // Insert discrete keys at the end
  std::copy(allDiscrete.begin(), allDiscrete.end(),
            std::back_inserter(lastKeys));

  // Get an ordering where the new keys are eliminated last
  Ordering ordering = Ordering::ColamdConstrainedLast(
      factors, KeyVector(lastKeys.begin(), lastKeys.end()), true);

  return ordering;
}

/* ************************************************************************* */
void HybridSmoother::update(const HybridGaussianFactorGraph &newFactors,
                            std::optional<size_t> maxNrLeaves,
                            const std::optional<Ordering> given_ordering) {
  const KeySet originalNewFactorKeys = newFactors.keys();
#ifdef DEBUG_SMOOTHER
  std::cout << "hybridBayesNet_ size before: " << hybridBayesNet_.size()
            << std::endl;
  std::cout << "newFactors size: " << newFactors.size() << std::endl;
#endif
  HybridGaussianFactorGraph updatedGraph;
  // Add the necessary conditionals from the previous timestep(s).
  std::tie(updatedGraph, hybridBayesNet_) =
      addConditionals(newFactors, hybridBayesNet_);
#ifdef DEBUG_SMOOTHER
  // print size of newFactors, updatedGraph, hybridBayesNet_
  std::cout << "updatedGraph size: " << updatedGraph.size() << std::endl;
  std::cout << "hybridBayesNet_ size after: " << hybridBayesNet_.size()
            << std::endl;
  std::cout << "total size: " << updatedGraph.size() + hybridBayesNet_.size()
            << std::endl;
#endif

  Ordering ordering;
  // If no ordering provided, then we compute one
  if (!given_ordering.has_value()) {
    // Get the keys from the new factors
    KeySet continuousKeysToInclude;  // Scheme 1: empty, 15sec/2000, 64sec/3000 (69s without TF)
    // continuousKeysToInclude = newFactors.keys();  // Scheme 2: all, 8sec/2000, 160sec/3000
    // continuousKeysToInclude = updatedGraph.keys();  // Scheme 3: all, stopped after 80sec/2000

    // Since updatedGraph now has all the connected conditionals,
    // we can get the correct ordering.
    ordering = this->getOrdering(updatedGraph, continuousKeysToInclude);
  } else {
    ordering = *given_ordering;
  }

  // Eliminate.
  HybridBayesNet bayesNetFragment = *updatedGraph.eliminateSequential(ordering);

#ifdef DEBUG_SMOOTHER_DETAIL
  for (auto conditional : bayesNetFragment) {
    auto e = std::dynamic_pointer_cast<HybridConditional::BaseConditional>(
        conditional);
    GTSAM_PRINT(*e);
  }
#endif

#ifdef DEBUG_SMOOTHER
  // Print discrete keys in the bayesNetFragment:
  std::cout << "Discrete keys in bayesNetFragment: ";
  for (auto &key : HybridFactorGraph(bayesNetFragment).discreteKeySet()) {
    std::cout << DefaultKeyFormatter(key) << " ";
  }
#endif

  /// Prune
  if (maxNrLeaves) {
    // `pruneBayesNet` sets the leaves with 0 in discreteFactor to nullptr in
    // all the conditionals with the same keys in bayesNetFragment.
    DiscreteValues newlyFixedValues;
    bayesNetFragment = bayesNetFragment.prune(*maxNrLeaves, marginalThreshold_,
                                              &newlyFixedValues);
    fixedValues_.insert(newlyFixedValues);
  }

#ifdef DEBUG_SMOOTHER
  // Print discrete keys in the bayesNetFragment:
  std::cout << "\nAfter pruning: ";
  for (auto &key : HybridFactorGraph(bayesNetFragment).discreteKeySet()) {
    std::cout << DefaultKeyFormatter(key) << " ";
  }
  std::cout << std::endl << std::endl;
#endif

#ifdef DEBUG_SMOOTHER_DETAIL
  for (auto conditional : bayesNetFragment) {
    auto c = std::dynamic_pointer_cast<HybridConditional::BaseConditional>(
        conditional);
    GTSAM_PRINT(*c);
  }
#endif

  // Add the partial bayes net to the posterior bayes net.
  hybridBayesNet_.add(bayesNetFragment);
}

/* ************************************************************************* */
std::pair<HybridGaussianFactorGraph, HybridBayesNet>
HybridSmoother::addConditionals(const HybridGaussianFactorGraph &newFactors,
                                const HybridBayesNet &hybridBayesNet) const {
  HybridGaussianFactorGraph graph(newFactors);
  HybridBayesNet updatedHybridBayesNet(hybridBayesNet);

  KeySet involvedKeys = newFactors.keys();
  auto involved = [&involvedKeys](const Key &key) {
    return involvedKeys.find(key) != involvedKeys.end();
  };

  // If hybridBayesNet is not empty,
  // it means we have conditionals to add to the factor graph.
  if (!hybridBayesNet.empty()) {
    // We add all relevant hybrid conditionals on the last continuous variable
    // in the previous `hybridBayesNet` to the graph

    // New conditionals to add to the graph
    gtsam::HybridBayesNet newConditionals;

    // NOTE(Varun) Using a for-range loop doesn't work since some of the
    // conditionals are invalid pointers

    // First get all the keys involved.
    // We do this by iterating over all conditionals, and checking if their
    // frontals are involved in the factor graph. If yes, then also make the
    // parent keys involved in the factor graph.
    for (size_t i = 0; i < hybridBayesNet.size(); i++) {
      auto conditional = hybridBayesNet.at(i);

      for (auto &key : conditional->frontals()) {
        if (involved(key)) {
          // Add the conditional parents to involvedKeys
          // so we add those conditionals too.
          for (auto &&parentKey : conditional->parents()) {
            involvedKeys.insert(parentKey);
          }
          // Break so we don't add parents twice.
          break;
        }
      }
    }
#ifdef DEBUG_SMOOTHER
    PrintKeySet(involvedKeys);
#endif

    for (size_t i = 0; i < hybridBayesNet.size(); i++) {
      auto conditional = hybridBayesNet.at(i);

      for (auto &key : conditional->frontals()) {
        if (involved(key)) {
          newConditionals.push_back(conditional);

          // Remove the conditional from the updated Bayes net
          auto it = find(updatedHybridBayesNet.begin(),
                         updatedHybridBayesNet.end(), conditional);
          updatedHybridBayesNet.erase(it);

          break;
        }
      }
    }

    graph.push_back(newConditionals);
  }

  return {graph, updatedHybridBayesNet};
}

/* ************************************************************************* */
HybridGaussianConditional::shared_ptr HybridSmoother::gaussianMixture(
    size_t index) const {
  return hybridBayesNet_.at(index)->asHybrid();
}

/* ************************************************************************* */
const HybridBayesNet &HybridSmoother::hybridBayesNet() const {
  return hybridBayesNet_;
}

/* ************************************************************************* */
HybridValues HybridSmoother::optimize() const {
  // Solve for the MPE
  DiscreteValues mpe = hybridBayesNet_.mpe();

  // Add fixed values to the MPE.
  mpe.insert(fixedValues_);

  // Given the MPE, compute the optimal continuous values.
  GaussianBayesNet gbn = hybridBayesNet_.choose(mpe);
  const VectorValues continuous = gbn.optimize();
  if (std::find(gbn.begin(), gbn.end(), nullptr) != gbn.end()) {
    throw std::runtime_error("At least one nullptr factor in hybridBayesNet_");
  }
  return HybridValues(continuous, mpe);
}

}  // namespace gtsam
