/*
 * Copyright 2021 Google LLC.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Registers generic engines for decision forest models.
//
#include "yggdrasil_decision_forests/serving/decision_forest/register_engines.h"

#include "yggdrasil_decision_forests/model/abstract_model.h"
#include "yggdrasil_decision_forests/model/fast_engine_factory.h"
#include "yggdrasil_decision_forests/model/gradient_boosted_trees/gradient_boosted_trees.h"
#include "yggdrasil_decision_forests/serving/decision_forest/decision_forest.h"
#include "yggdrasil_decision_forests/serving/decision_forest/quick_scorer_extended.h"
#include "yggdrasil_decision_forests/serving/example_set_model_wrapper.h"
#include "yggdrasil_decision_forests/utils/compatibility.h"

namespace yggdrasil_decision_forests {
namespace model {

namespace {

// Maximum number of nodes in any tree.
int64_t MaxNumberOfNodesPerTree(
    const std::vector<std::unique_ptr<decision_tree::DecisionTree>>& trees) {
  int64_t max_num_nodes = 0;
  for (const auto& tree : trees) {
    max_num_nodes = std::max(max_num_nodes, tree->NumNodes());
  }
  return max_num_nodes;
}

}  // namespace

class GradientBoostedTreesGenericFastEngineFactory : public FastEngineFactory {
 public:
  using SourceModel = gradient_boosted_trees::GradientBoostedTreesModel;

  std::string name() const override {
    return serving::gradient_boosted_trees::kGeneric;
  }

  bool IsCompatible(const AbstractModel* const model) const override {
    auto* gbt_model = dynamic_cast<const SourceModel*>(model);
    // This implementation is the most generic and least efficient engine.
    if (gbt_model == nullptr) {
      return false;
    }
    return gbt_model->IsMissingValueConditionResultFollowGlobalImputation();
  }

  std::vector<std::string> IsBetterThan() const override { return {}; }

  utils::StatusOr<std::unique_ptr<serving::FastEngine>> CreateEngine(
      const AbstractModel* const model) const override {
    auto* gbt_model = dynamic_cast<const SourceModel*>(model);
    if (!gbt_model) {
      return absl::InvalidArgumentError("The model is not a GBDT.");
    }

    const bool need_uint32_node_index =
        MaxNumberOfNodesPerTree(gbt_model->decision_trees()) >=
        std::numeric_limits<uint16_t>::max();
    // More than 65k nodes in a single tree is a likely indication of a problem
    // with the model.

    std::unique_ptr<serving::FastEngine> engine;
    switch (gbt_model->task()) {
      case proto::CLASSIFICATION:
        if (gbt_model->label_col_spec()
                .categorical()
                .number_of_unique_values() == 3) {
          // Binary classification.
          if (need_uint32_node_index) {
            auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
                serving::decision_forest::
                    GenericGradientBoostedTreesBinaryClassification<uint32_t>,
                serving::decision_forest::Predict>>();
            RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
            return engine;
          } else {
            auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
                serving::decision_forest::
                    GradientBoostedTreesBinaryClassification,
                serving::decision_forest::Predict>>();
            RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
            return engine;
          }
        } else {
          // Multi-class classification.
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::
                  GradientBoostedTreesMulticlassClassification,
              serving::decision_forest::Predict>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
          return engine;
        }

      case proto::REGRESSION: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::GradientBoostedTreesRegression,
            serving::decision_forest::Predict>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
        return engine;
      }

      case proto::RANKING: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::GradientBoostedTreesRanking,
            serving::decision_forest::Predict>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
        return engine;
      }

      default:
        return absl::InvalidArgumentError("Non supported GBDT model");
    }
  }
};

REGISTER_FastEngineFactory(GradientBoostedTreesGenericFastEngineFactory,
                           serving::gradient_boosted_trees::kGeneric);

class GradientBoostedTreesQuickScorerFastEngineFactory
    : public FastEngineFactory {
 public:
  using SourceModel = gradient_boosted_trees::GradientBoostedTreesModel;

  std::string name() const override {
    return serving::gradient_boosted_trees::kQuickScorerExtended;
  }

  bool IsCompatible(const AbstractModel* const model) const override {
    auto* gbt_model = dynamic_cast<const SourceModel*>(model);
    if (gbt_model == nullptr) {
      return false;
    }

    if (!gbt_model->IsMissingValueConditionResultFollowGlobalImputation()) {
      return false;
    }

    if (gbt_model->NumTrees() > serving::decision_forest::internal::
                                    QuickScorerExtendedModel::kMaxTrees) {
      return false;
    }

    for (const auto& src_tree : gbt_model->decision_trees()) {
      if (src_tree->NumLeafs() > serving::decision_forest::internal::
                                     QuickScorerExtendedModel::kMaxLeafs) {
        return false;
      }
    }

    switch (gbt_model->task()) {
      case proto::CLASSIFICATION:
        return gbt_model->label_col_spec()
                   .categorical()
                   .number_of_unique_values() == 3;
      case proto::REGRESSION:
      case proto::RANKING:
        return true;
      default:
        return false;
    }
  }

  std::vector<std::string> IsBetterThan() const override {
    return {serving::gradient_boosted_trees::kGeneric,
            serving::gradient_boosted_trees::kOptPred};
  }

  utils::StatusOr<std::unique_ptr<serving::FastEngine>> CreateEngine(
      const AbstractModel* const model) const override {
    auto* gbt_model = dynamic_cast<const SourceModel*>(model);
    if (!gbt_model) {
      return absl::InvalidArgumentError("The model is not a GBDT.");
    }

    switch (gbt_model->task()) {
      case proto::CLASSIFICATION:
        if (gbt_model->label_col_spec()
                .categorical()
                .number_of_unique_values() == 3) {
          // Binary classification.
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::
                  GradientBoostedTreesBinaryClassificationQuickScorerExtended,
              serving::decision_forest::Predict>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
          return engine;
        } else {
          return absl::InvalidArgumentError("Non supported GBDT model");
        }

      case proto::REGRESSION: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::
                GradientBoostedTreesRegressionQuickScorerExtended,
            serving::decision_forest::Predict>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
        return engine;
      }

      case proto::RANKING: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::
                GradientBoostedTreesRankingQuickScorerExtended,
            serving::decision_forest::Predict>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
        return engine;
      }

      default:
        return absl::InvalidArgumentError("Non supported GBDT model");
    }
  }
};

REGISTER_FastEngineFactory(
    GradientBoostedTreesQuickScorerFastEngineFactory,
    serving::gradient_boosted_trees::kQuickScorerExtended);

class GradientBoostedTreesOptPredFastEngineFactory : public FastEngineFactory {
 public:
  using SourceModel = gradient_boosted_trees::GradientBoostedTreesModel;

  std::string name() const override {
    return serving::gradient_boosted_trees::kOptPred;
  }

  bool IsCompatible(const AbstractModel* const model) const override {
    auto* gbt_model = dynamic_cast<const SourceModel*>(model);
    if (gbt_model == nullptr) {
      return false;
    }

    if (!gbt_model->IsMissingValueConditionResultFollowGlobalImputation()) {
      return false;
    }

    for (const auto& src_tree : gbt_model->decision_trees()) {
      if (src_tree->NumLeafs() > std::numeric_limits<uint16_t>::max()) {
        return false;
      }
    }

    for (const auto feature_idx : gbt_model->input_features()) {
      const auto& feature = gbt_model->data_spec().columns(feature_idx);
      switch (feature.type()) {
        case dataset::proto::ColumnType::NUMERICAL:
          break;
        case dataset::proto::ColumnType::CATEGORICAL:
          if (feature.categorical().number_of_unique_values() > 32) {
            return false;
          }
          break;
        default:
          return false;
      }
    }

    switch (gbt_model->task()) {
      case proto::CLASSIFICATION:
        return gbt_model->label_col_spec()
                   .categorical()
                   .number_of_unique_values() == 3;
      case proto::REGRESSION:
      case proto::RANKING:
        return true;
      default:
        return false;
    }
  }

  std::vector<std::string> IsBetterThan() const override {
    return {serving::gradient_boosted_trees::kGeneric};
  }

  utils::StatusOr<std::unique_ptr<serving::FastEngine>> CreateEngine(
      const AbstractModel* const model) const override {
    auto* gbt_model = dynamic_cast<const SourceModel*>(model);
    if (!gbt_model) {
      return absl::InvalidArgumentError("The model is not a GBDT.");
    }

    switch (gbt_model->task()) {
      case proto::CLASSIFICATION:
        if (gbt_model->label_col_spec()
                .categorical()
                .number_of_unique_values() == 3) {
          // Binary classification.
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::
                  GradientBoostedTreesBinaryClassificationNumericalAndCategorical,
              serving::decision_forest::PredictWithExampleSet>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
          return engine;
        } else {
          return absl::InvalidArgumentError("Non supported GBDT model");
        }

      case proto::REGRESSION: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::
                GradientBoostedTreesRegressionNumericalAndCategorical,
            serving::decision_forest::PredictWithExampleSet>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
        return engine;
      }

      case proto::RANKING: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::
                GradientBoostedTreesRankingNumericalAndCategorical,
            serving::decision_forest::PredictWithExampleSet>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*gbt_model));
        return engine;
      }

      default:
        return absl::InvalidArgumentError("Non supported GBDT model");
    }
  }
};

REGISTER_FastEngineFactory(GradientBoostedTreesOptPredFastEngineFactory,
                           serving::gradient_boosted_trees::kOptPred);

class RandomForestGenericFastEngineFactory : public model::FastEngineFactory {
 public:
  using SourceModel = random_forest::RandomForestModel;

  std::string name() const override { return serving::random_forest::kGeneric; }

  bool IsCompatible(const AbstractModel* const model) const override {
    auto* rf_model = dynamic_cast<const SourceModel*>(model);
    // This implementation is the most generic and least efficient engine.
    if (rf_model == nullptr) {
      return false;
    }
    return rf_model->IsMissingValueConditionResultFollowGlobalImputation();
  }

  std::vector<std::string> IsBetterThan() const override { return {}; }

  utils::StatusOr<std::unique_ptr<serving::FastEngine>> CreateEngine(
      const AbstractModel* const model) const override {
    auto* rf_model = dynamic_cast<const SourceModel*>(model);
    if (!rf_model) {
      return absl::InvalidArgumentError("The model is not a RF.");
    }

    const bool need_uint32_node_index =
        MaxNumberOfNodesPerTree(rf_model->decision_trees()) >=
        std::numeric_limits<uint16_t>::max();

    switch (rf_model->task())
    case model::proto::CLASSIFICATION:
      if (rf_model->label_col_spec().categorical().number_of_unique_values() ==
          3) {
        // Binary classification.
        if (need_uint32_node_index) {
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::GenericRandomForestBinaryClassification<
                  uint32_t>,
              serving::decision_forest::Predict>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
          return engine;
        } else {
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::GenericRandomForestBinaryClassification<
                  uint16_t>,
              serving::decision_forest::Predict>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
          return engine;
        }
      } else {
        // Multi-class classification.
        if (need_uint32_node_index) {
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::
                  GenericRandomForestMulticlassClassification<uint32_t>,
              serving::decision_forest::Predict>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
          return engine;
        } else {
          auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
              serving::decision_forest::
                  GenericRandomForestMulticlassClassification<uint16_t>,
              serving::decision_forest::Predict>>();
          RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
          return engine;
        }

        case model::proto::REGRESSION:
          if (need_uint32_node_index) {
            auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
                serving::decision_forest::GenericRandomForestRegression<
                    uint32_t>,
                serving::decision_forest::Predict>>();
            RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
            return engine;
          } else {
            auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
                serving::decision_forest::GenericRandomForestRegression<
                    uint16_t>,
                serving::decision_forest::Predict>>();
            RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
            return engine;
          }

        default:
          return absl::InvalidArgumentError("Non supported RF model");
      }
  }
};

REGISTER_FastEngineFactory(RandomForestGenericFastEngineFactory,
                           serving::random_forest::kGeneric);

class RandomForestOptPredFastEngineFactory : public model::FastEngineFactory {
 public:
  using SourceModel = random_forest::RandomForestModel;

  std::string name() const override { return serving::random_forest::kOptPred; }

  bool IsCompatible(const AbstractModel* const model) const override {
    auto* rf_model = dynamic_cast<const SourceModel*>(model);
    // This implementation is the most generic and least efficient engine.
    if (rf_model == nullptr) {
      return false;
    }
    if (!rf_model->IsMissingValueConditionResultFollowGlobalImputation()) {
      return false;
    }

    for (const auto& src_tree : rf_model->decision_trees()) {
      if (src_tree->NumLeafs() > std::numeric_limits<uint16_t>::max()) {
        return false;
      }
    }

    for (const auto feature_idx : rf_model->input_features()) {
      const auto& feature = rf_model->data_spec().columns(feature_idx);
      switch (feature.type()) {
        case dataset::proto::ColumnType::NUMERICAL:
          break;
        case dataset::proto::ColumnType::CATEGORICAL:
          if (feature.categorical().number_of_unique_values() > 32) {
            return false;
          }
          break;
        default:
          return false;
      }
    }

    switch (rf_model->task()) {
      case proto::CLASSIFICATION:
        return rf_model->label_col_spec()
                   .categorical()
                   .number_of_unique_values() == 3;
      case proto::REGRESSION:
      case proto::RANKING:
        return true;
      default:
        return false;
    }
  }

  std::vector<std::string> IsBetterThan() const override {
    return {serving::random_forest::kGeneric};
  }

  utils::StatusOr<std::unique_ptr<serving::FastEngine>> CreateEngine(
      const AbstractModel* const model) const override {
    auto* rf_model = dynamic_cast<const SourceModel*>(model);
    if (!rf_model) {
      return absl::InvalidArgumentError("The model is not a RF.");
    }

    switch (rf_model->task()) {
      case model::proto::CLASSIFICATION: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::
                RandomForestBinaryClassificationNumericalAndCategoricalFeatures,
            serving::decision_forest::PredictWithExampleSet>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
        return engine;
      }

      case model::proto::REGRESSION: {
        auto engine = absl::make_unique<serving::ExampleSetModelWrapper<
            serving::decision_forest::
                RandomForestRegressionNumericalAndCategorical,
            serving::decision_forest::PredictWithExampleSet>>();
        RETURN_IF_ERROR(engine->LoadModel<SourceModel>(*rf_model));
        return engine;
      }

      default:
        return absl::InvalidArgumentError("Non supported RF model");
    }
  }
};

REGISTER_FastEngineFactory(RandomForestOptPredFastEngineFactory,
                           serving::random_forest::kOptPred);

}  // namespace model
}  // namespace yggdrasil_decision_forests