/* ----------------------------------------------------------------------- *//**
 *
 * @file Decision_Tree_impl.hpp
 *
 *//* ----------------------------------------------------------------------- */

#ifndef MADLIB_MODULES_RP_DT_IMPL_HPP
#define MADLIB_MODULES_RP_DT_IMPL_HPP

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <iterator>

#include <limits>  // std::numeric_limits

#include <dbconnector/dbconnector.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/variate_generator.hpp>
#include "DT_proto.hpp"

namespace madlib {

// Use Eigen
using namespace dbal::eigen_integration;
using boost::uniform_int;
using boost::variate_generator;

namespace modules {

namespace recursive_partitioning {

namespace {

typedef std::pair<int, double> argsort_pair;

bool argsort_comp(const argsort_pair& left,
                  const argsort_pair& right) {
    return left.second > right.second;
}

IntegerVector argsort(const ColumnVector & x) {
    IntegerVector indices(x.size());
    std::vector<argsort_pair> data(x.size());
    for(int i=0; i < x.size(); i++) {
        data[i].first = i;
        data[i].second = x(i);
    }
    std::sort(data.begin(), data.end(), argsort_comp);
    for(size_t i=0; i < data.size(); i++) {
        indices(i) = data[i].first;
    }
    return indices;
}

string
escape_quotes(const string &before) {
    // From http://stackoverflow.com/questions/1162619/fastest-quote-escaping-implementation
    string after;
    after.reserve(before.length() + 4);

    for (string::size_type i = 0; i < before.length(); ++i) {
        switch (before[i]) {
            case '"':
            case '\\':
                after += '\\';
                // Fall through.
            default:
                after += before[i];
        }
    }
    return after;
}
// ------------------------------------------------------------

double
computeEntropy(const double &p) {
    if (p < 0.) { throw std::runtime_error("unexpected negative probability"); }
    if (p == 0.) { return 0.; }
    return -p * log2(p);
}
// ------------------------------------------------------------

// Extract a string from ArrayHandle<text*>
inline
string
get_text(ArrayHandle<text*> &strs, size_t i) {
    return std::string(VARDATA_ANY(strs[i]), VARSIZE_ANY(strs[i]) - VARHDRSZ);
}

} // anonymous namespace

// ------------------------------------------------------------------------
// Definitions for class Decision Tree
// ------------------------------------------------------------------------

template <class Container>
inline
DecisionTree<Container>::DecisionTree():
        Base(defaultAllocator().allocateByteString<
                dbal::FunctionContext, dbal::DoZero, dbal::ThrowBadAlloc>(0)) {
    this->initialize();
}
// ------------------------------------------------------------

template <class Container>
inline
DecisionTree<Container>::DecisionTree(
        Init_type& inInitialization): Base(inInitialization) {
    this->initialize();
}
// ------------------------------------------------------------

template <class Container>
inline
void
DecisionTree<Container>::bind(ByteStream_type& inStream) {

    inStream >> tree_depth
             >> n_y_labels
             >> max_n_surr
             >> is_regression
             >> impurity_type;

    size_t n_nodes = 0;
    size_t n_labels = 0;
    size_t max_surrogates = 0;
    if (!tree_depth.isNull()) {
        n_nodes = static_cast<size_t>(pow(2, tree_depth) - 1);
        // for classification n_labels = n_y_labels + 1 since the last element
        // is the count of actual (unweighted) tuples landing on a node
        // for regression, n_y_labels is same as REGRESS_N_STATS
        if (is_regression)
            n_labels = static_cast<size_t>(n_y_labels);
        else
            n_labels = static_cast<size_t>(n_y_labels + 1);
        max_surrogates = max_n_surr;
    }

    inStream
        >> feature_indices.rebind(n_nodes)
        >> feature_thresholds.rebind(n_nodes)
        >> is_categorical.rebind(n_nodes)
        >> nonnull_split_count.rebind(n_nodes * 2)
        >> surr_indices.rebind(n_nodes * max_surrogates)
        >> surr_thresholds.rebind(n_nodes * max_surrogates)
        >> surr_status.rebind(n_nodes * max_surrogates)
        >> surr_agreement.rebind(n_nodes * max_surrogates)
        >> predictions.rebind(n_nodes, n_labels)
        ;
}
// ------------------------------------------------------------

template <class Container>
inline
DecisionTree<Container>&
DecisionTree<Container>::rebind(const uint16_t in_tree_depth,
                                const uint16_t in_y_labels,
                                const uint16_t in_max_n_surr,
                                const bool in_is_regression) {
    tree_depth = in_tree_depth;
    n_y_labels = in_y_labels;
    max_n_surr = in_max_n_surr;
    is_regression = in_is_regression;
    this->resize();
    return *this;
}
// ------------------------------------------------------------

template <class Container>
inline
DecisionTree<Container>&
DecisionTree<Container>::incrementInPlace() {
    // back up current tree
    size_t n_orig_nodes = static_cast<size_t>(pow(2, tree_depth) - 1);
    DecisionTree<Container> orig = DecisionTree<Container>();
    orig.rebind(tree_depth, n_y_labels, max_n_surr, is_regression);
    orig.copy(*this);

    // increment one level
    tree_depth ++;
    this->resize();

    // restore from backup
    is_regression = orig.is_regression;
    impurity_type = orig.impurity_type;
    feature_indices.segment(0, n_orig_nodes) = orig.feature_indices;
    feature_thresholds.segment(0, n_orig_nodes) = orig.feature_thresholds;
    is_categorical.segment(0, n_orig_nodes) = orig.is_categorical;
    nonnull_split_count.segment(0, n_orig_nodes*2) = orig.nonnull_split_count;
    if (max_n_surr > 0){
        surr_indices.segment(0, n_orig_nodes*max_n_surr) = orig.surr_indices;
        surr_thresholds.segment(0, n_orig_nodes*max_n_surr) = orig.surr_thresholds;
        surr_status.segment(0, n_orig_nodes*max_n_surr) = orig.surr_status;
        surr_agreement.segment(0, n_orig_nodes*max_n_surr) = orig.surr_agreement;
    }

    for (Index i = 0; i < orig.predictions.rows(); i++){
        // resize adds rows at the end of predictions
        predictions.row(i) = orig.predictions.row(i);
    }

    // mark all newly allocated leaves as non-existing nodes, they will be
    //  categorized as leaf nodes by the parent during expansion
    size_t n_new_leaves = n_orig_nodes + 1;
    feature_indices.segment(n_orig_nodes, n_new_leaves).setConstant(NODE_NON_EXISTING);
    feature_thresholds.segment(n_orig_nodes, n_new_leaves).setConstant(0);
    is_categorical.segment(n_orig_nodes, n_new_leaves).setConstant(0);
    nonnull_split_count.segment(n_orig_nodes*2, n_new_leaves*2).setConstant(0);

    if (max_n_surr > 0){
        surr_indices.segment(n_orig_nodes*max_n_surr,
                             n_new_leaves*max_n_surr).setConstant(SURR_NON_EXISTING);
        surr_thresholds.segment(n_orig_nodes*max_n_surr,
                                n_new_leaves*max_n_surr).setConstant(0);
        surr_status.segment(n_orig_nodes*max_n_surr,
                                n_new_leaves*max_n_surr).setConstant(0);
        surr_agreement.segment(n_orig_nodes*max_n_surr,
                                    n_new_leaves*max_n_surr).setConstant(0);
    }
    for (size_t i = n_orig_nodes; i < n_orig_nodes + n_new_leaves; i++){
        predictions.row(i).setConstant(0);
    }

   return *this;
}
// ------------------------------------------------------------

template <class Container>
inline
uint64_t
DecisionTree<Container>::getMajorityCount(Index node_index) const {
    // majority count is defined as the greater of the tuples passed in to
    // the two split branches. We only count tuples that have a non-null
    // value for the primary split of the node.
    if (feature_indices(node_index) < 0)
        throw std::runtime_error("Requested count for a leaf/non-existing node");
    uint64_t true_count = static_cast<uint64_t>(nonnull_split_count(node_index*2));
    uint64_t false_count = static_cast<uint64_t>(nonnull_split_count(node_index*2 + 1));
    return true_count >= false_count ? true_count : false_count;
}

template <class Container>
inline
bool
DecisionTree<Container>::getMajoritySplit(Index node_index) const {
    // majority count is defined as the greater of the tuples passed in to
    // the two split branches. We only count tuples that have a non-null
    // value for the primary split of the node.
    if (feature_indices(node_index) < 0)
        throw std::runtime_error("Requested count for a leaf/non-existing node");
    uint64_t true_count = static_cast<uint64_t>(nonnull_split_count(node_index*2));
    uint64_t false_count = static_cast<uint64_t>(nonnull_split_count(node_index*2 + 1));
    return (true_count >= false_count);
}
// -------------------------------------------------------------------------

template <class Container>
inline
bool
DecisionTree<Container>::getSurrSplit(
        Index node_index,
        MappedIntegerVector cat_features,
        MappedColumnVector con_features) const {

    Index surr_base_index;
    for (surr_base_index = node_index * max_n_surr;
            surr_base_index < (node_index+1) * max_n_surr; surr_base_index++){

        Index surr_feat_index = surr_indices(surr_base_index);
        if (surr_feat_index < 0)
            break;

        double surr_feat_threshold = surr_thresholds(surr_base_index);
        bool split_response;
        if (std::abs(surr_status(surr_base_index)) == 1){
            if (!isNull(cat_features(surr_feat_index), true)){
                split_response = cat_features(surr_feat_index) <= surr_feat_threshold;
                // negative status is a reverse split (> relation)
                return ((surr_status(surr_base_index) > 0) ?
                            (split_response) : (!split_response));
            }
        } else {
            if (!isNull(con_features(surr_feat_index), false)){
                split_response = con_features(surr_feat_index) <= surr_feat_threshold;
                // negative status is a reverse split (> relation)
                return ((surr_status(surr_base_index) > 0) ?
                            (split_response) : (!split_response));
            }
        }
    }
    return getMajoritySplit(node_index);
}
// -------------------------------------------------------------------------

template <class Container>
inline
Index
DecisionTree<Container>::search(MappedIntegerVector cat_features,
                                MappedColumnVector con_features) const {
    Index current = 0;
    int feature_index = feature_indices(current);
    while (feature_index != IN_PROCESS_LEAF && feature_index != FINISHED_LEAF) {
        assert(feature_index != NODE_NON_EXISTING);
        bool is_split_true = false;
        if (is_categorical(current) != 0) {
            if (isNull(cat_features(feature_index), true)){
                is_split_true = getSurrSplit(current, cat_features, con_features);
            } else{
                is_split_true = (cat_features(feature_index)
                                    <= feature_thresholds(current));
            }
        } else {
            if (isNull(con_features(feature_index), false)){
                is_split_true = getSurrSplit(current, cat_features, con_features);

            } else{
                is_split_true = (con_features(feature_index)
                                    <= feature_thresholds(current));
            }
        }
        /*       (i)
               /     \
           (2i+1)  (2i+2)
         */
        current = is_split_true ? trueChild(current) : falseChild(current);
        feature_index = feature_indices(current);
    }
    return current;
}
// ------------------------------------------------------------

template <class Container>
inline
ColumnVector
DecisionTree<Container>::predict(MappedIntegerVector cat_features,
                                 MappedColumnVector con_features) const {
    Index leaf_index = search(cat_features, con_features);
    return statPredict(predictions.row(leaf_index));
}
// ------------------------------------------------------------

template <class Container>
inline
double
DecisionTree<Container>::predict_response(
        MappedIntegerVector cat_features,
        MappedColumnVector con_features) const {
    ColumnVector curr_prediction = predict(cat_features, con_features);

    if (is_regression){
        return curr_prediction(0);
    } else {
        Index max_label;
        curr_prediction.maxCoeff(&max_label);
        return static_cast<double>(max_label);
    }
}
// ------------------------------------------------------------

template <class Container>
inline
double
DecisionTree<Container>::predict_response(Index leaf_index) const {
    ColumnVector curr_prediction = statPredict(predictions.row(leaf_index));
    if (is_regression){
        return curr_prediction(0);
    } else {
        Index max_label;
        curr_prediction.maxCoeff(&max_label);
        return static_cast<double>(max_label);
    }
}
// ------------------------------------------------------------

template <class Container>
inline
double
DecisionTree<Container>::impurity(const ColumnVector &stats) const {
    if (is_regression){
        // only mean-squared error metric is supported
        // variance is a measure of the mean-squared distance to all points
        return stats(2) / stats(0) - pow(stats(1) / stats(0), 2);
    } else {
        ColumnVector proportions = statPredict(stats);
        if (impurity_type == GINI){
            return 1 - proportions.cwiseProduct(proportions).sum();
        } else if (impurity_type == ENTROPY){
            return proportions.unaryExpr(std::ptr_fun(computeEntropy)).sum();
        } else if (impurity_type == MISCLASS){
            return 1 - proportions.maxCoeff();
        } else
            throw std::runtime_error("No impurity function set for a classification tree");
    }
}
// ------------------------------------------------------------

template <class Container>
inline
double
DecisionTree<Container>::impurityGain(const ColumnVector &combined_stats,
                                      const uint16_t &stats_per_split) const {

    double true_count = statWeightedCount(combined_stats.segment(0, stats_per_split));
    double false_count = statWeightedCount(combined_stats.segment(stats_per_split, stats_per_split));
    double total_count = true_count + false_count;

    if (true_count == 0 || false_count == 0) {
        // no gain if all fall into one side
        return 0.;
    }
    double true_weight = true_count / total_count;
    double false_weight = false_count / total_count;
    ColumnVector stats_sum = combined_stats.segment(0, stats_per_split) +
            combined_stats.segment(stats_per_split, stats_per_split);
    return (impurity(stats_sum) -
            true_weight * impurity(combined_stats.segment(0, stats_per_split)) -
            false_weight *
                impurity(combined_stats.segment(stats_per_split, stats_per_split))
            );
}

// ------------------------------------------------------------
template <class Container>
inline
bool
DecisionTree<Container>::updatePrimarySplit(
        const Index node_index,
        const int & max_feat,
        const double & max_threshold,
        const bool & max_is_cat,
        const uint16_t & min_split,
        const ColumnVector &true_stats,
        const ColumnVector &false_stats) {

    // current node
    feature_indices(node_index) = max_feat;
    is_categorical(node_index) = max_is_cat ? 1 : 0;
    feature_thresholds(node_index) = max_threshold;

    // update feature indices and prediction for children
    feature_indices(trueChild(node_index)) = IN_PROCESS_LEAF;
    predictions.row(trueChild(node_index)) = true_stats;
    feature_indices(falseChild(node_index)) = IN_PROCESS_LEAF;
    predictions.row(falseChild(node_index)) = false_stats;

    // true_stats and false_stats only include the tuples for which the primary
    // split is NULL. The number of tuples in these stats need to be stored to
    // compute a majority branch during surrogate training.
    uint64_t true_count = statCount(true_stats);
    uint64_t false_count = statCount(false_stats);
    nonnull_split_count(node_index*2) = static_cast<double>(true_count);
    nonnull_split_count(node_index*2 + 1) = static_cast<double>(false_count);

    // current node's children won't split if,
    // 1. children are pure (responses are too similar to split further)
    // 2. children are too small to split further (count < min_split)
    bool children_wont_split = (isChildPure(true_stats) &&
                                isChildPure(false_stats) &&
                                true_count < min_split &&
                                false_count < min_split
                                );
    return children_wont_split;
}
// -------------------------------------------------------------------------


template <class Container>
template <class Accumulator>
inline
bool
DecisionTree<Container>::expand(const Accumulator &state,
                                const MappedMatrix &con_splits,
                                const uint16_t &min_split,
                                const uint16_t &min_bucket,
                                const uint16_t &max_depth) {
    uint16_t n_non_leaf_nodes = static_cast<uint16_t>(state.n_leaf_nodes - 1);
    bool children_not_allocated = true;
    bool children_wont_split = true;

    const uint16_t &sps = state.stats_per_split;  // short form for brevity
    for (Index i=0; i < state.n_leaf_nodes; i++) {
        Index current = n_non_leaf_nodes + i;
        if (feature_indices(current) == IN_PROCESS_LEAF) {
            // 1. Set the prediction for current node from stats of all rows
            predictions.row(current) = state.node_stats.row(i);

            // 2. Compute the best feature to split current node by

            // if a leaf node exists, compute the gain in impurity for each split
            // pick split  with maximum gain and update node with split value
            int max_feat = -1;
            Index max_bin = -1;
            bool max_is_cat = false;
            double max_impurity_gain = -std::numeric_limits<double>::infinity();
            ColumnVector max_stats;
            // go through all categorical stats
            int cumsum = 0;
            for (int f=0; f < state.n_cat_features; ++f){ // each feature
                for (int v=0; cumsum < state.cat_levels_cumsum(f); ++v, ++cumsum){
                    // each value of feature
                    Index fv_index = state.indexCatStats(f, v, true);
                    double gain = impurityGain(
                        state.cat_stats.row(i).segment(fv_index, sps * 2), sps);
                    if (gain > max_impurity_gain){
                        max_impurity_gain = gain;
                        max_feat = f;
                        max_bin = v;
                        max_is_cat = true;
                        max_stats = state.cat_stats.row(i).segment(fv_index,
                                                                   sps * 2);
                    }
                }
            }
            // go through all continuous stats
            for (int f=0; f < state.n_con_features; ++f){  // each feature
                for (Index b=0; b < state.n_bins; ++b){
                    // each bin of feature
                    Index fb_index = state.indexConStats(f, b, true);
                    double gain = impurityGain(
                        state.con_stats.row(i).segment(fb_index, sps * 2), sps);
                    if (gain > max_impurity_gain){
                        max_impurity_gain = gain;
                        max_feat = f;
                        max_bin = b;
                        max_is_cat = false;
                        max_stats = state.con_stats.row(i).segment(fb_index,
                                                                   sps * 2);
                    }
                }
            }

            // 3. Create and update child nodes if splitting current
            if (max_impurity_gain > 0 &&
                    shouldSplit(max_stats, min_split, min_bucket, sps, max_depth)) {

                double max_threshold;
                if (max_is_cat)
                    max_threshold = static_cast<double>(max_bin);
                else
                    max_threshold = con_splits(max_feat, max_bin);

                if (children_not_allocated) {
                    // allocate the memory for child nodes if not allocated already
                    incrementInPlace();
                    children_not_allocated = false;
                }
                children_wont_split &=
                    updatePrimarySplit(
                        current, static_cast<int>(max_feat),
                        max_threshold, max_is_cat,
                        min_split,
                        max_stats.segment(0, sps),   // true_stats
                        max_stats.segment(sps, sps)  // false_stats
                    );

            } else {
                feature_indices(current) = FINISHED_LEAF;
            }
        } // if leaf exists
    } // for each leaf

    // return true if tree expansion is finished
    //      we check (tree_depth = max_depth + 1) since internally
    //      tree_depth starts from 1 though max_depth expects root node depth as 0
    bool training_finished = (children_not_allocated ||
                              tree_depth >= (max_depth + 1) ||
                              children_wont_split);
    if (training_finished){
        // label any remaining IN_PROCESS_LEAF as FINISHED_LEAF
        for (Index i=0; i < feature_indices.size(); i++) {
            if (feature_indices(i) == IN_PROCESS_LEAF)
                feature_indices(i) = FINISHED_LEAF;
        }
    }

    return training_finished;
}
// -------------------------------------------------------------------------


template <class Container>
template <class Accumulator>
inline
void
DecisionTree<Container>::pickSurrogates(
        const Accumulator &state,
        const MappedMatrix &con_splits) {

    uint16_t n_cats = state.n_cat_features;
    uint16_t n_cons = state.n_con_features;
    uint16_t n_bins = state.n_bins;
    uint32_t n_cat_splits = state.total_n_cat_levels;
    uint32_t n_con_splits = n_cons * n_bins;

    // add every two columns in cat stats and con_stats
    // this assumes that stats_per_split = 2, hence cat_stats and con_stats have
    // even number of cols
    // we use the *_agg_matrix to add every two alternate columns of the
    // *_stats matrix to create a forward and reverse agreement metric
    // for each split.
    // eg. in cat_stats,
    //      we add columns 1 and 3 to get the <= split agreement for 1st cat split.
    //      we add columns 2 and 4 to get the > split agreement for 1st cat split.
    //      we add columns 5 and 7 to get the <= split agreement for 2nd cat split.
    //      we add columns 6 and 8 to get the > split agreement for 2nd cat split.
    ColumnVector fwd_agg_vec(4);
    fwd_agg_vec << 1, 0, 1, 0;
    ColumnVector rev_agg_vec(4);
    rev_agg_vec << 0, 1, 0, 1;

    Matrix cat_agg_matrix = Matrix::Zero(n_cat_splits*4, n_cat_splits * 2);
    for (Index i=0; i < cat_agg_matrix.cols(); i+=2){
        cat_agg_matrix.col(i).segment(2*i, 4) = fwd_agg_vec;
        cat_agg_matrix.col(i+1).segment(2*i, 4) = rev_agg_vec;
    }

    Matrix con_agg_matrix = Matrix::Zero(n_con_splits*4, n_con_splits*2);
    for (Index i=0; i < con_agg_matrix.cols(); i+=2){
        con_agg_matrix.col(i).segment(2*i, 4) = fwd_agg_vec;
        con_agg_matrix.col(i+1).segment(2*i, 4) = rev_agg_vec;
    }

    assert(state.cat_stats.cols() == cat_agg_matrix.rows());
    assert(state.con_stats.cols() == con_agg_matrix.rows());
    Matrix cat_stats_counts(state.cat_stats * cat_agg_matrix);
    Matrix con_stats_counts(state.con_stats * con_agg_matrix);

    // cat_stats_counts size = n_nodes x n_cats*2
    // con_stats_counts size = n_nodes x n_cons*2
    // *_stats_counts now contains the agreement count for each split where
    // each even col represents forward surrogate split count and
    // each odd col represents reverse surrogate split count.

    // Number of nodes in a last layer = 2^(tree_depth-1). (since depth starts from 1)
    // For n_surr_nodes, we need number of nodes in 2nd last layer,
    // so we use 2^(tree_depth-2)
    uint16_t n_surr_nodes = static_cast<uint16_t>(pow(2, tree_depth - 2));
    uint16_t n_ancestors = static_cast<uint16_t>(n_surr_nodes - 1);

    for (Index i=0; i < n_surr_nodes; i++){
        Index curr_node = n_ancestors + i;
        assert(curr_node >= 0 && curr_node < feature_indices.size());

        if (feature_indices(curr_node) >= 0){
            // 1. Compute the max count and corresponding split threshold for
            // each categorical and continuous feature
            ColumnVector cat_max_thres = ColumnVector::Zero(n_cats);
            ColumnVector cat_max_count = ColumnVector::Zero(n_cats);
            IntegerVector cat_max_is_reverse = IntegerVector::Zero(n_cats);
            Index prev_cum_levels = 0;
            for (Index each_cat=0; each_cat < n_cats; each_cat++){
                Index n_levels = state.cat_levels_cumsum(each_cat) - prev_cum_levels;
                Index max_label;
                (cat_stats_counts.row(i).segment(
                    prev_cum_levels * 2, n_levels * 2)).maxCoeff(&max_label);
                cat_max_thres(each_cat) = static_cast<double>(max_label / 2);
                cat_max_count(each_cat) =
                        cat_stats_counts(i, prev_cum_levels*2 + max_label);
                // every odd col is for reverse, hence i % 2 == 1 for reverse index i
                cat_max_is_reverse(each_cat) = (max_label % 2 == 1) ? 1 : 0;
                prev_cum_levels = state.cat_levels_cumsum(each_cat);
            }

            ColumnVector con_max_thres = ColumnVector::Zero(n_cons);
            ColumnVector con_max_count = ColumnVector::Zero(n_cons);
            IntegerVector con_max_is_reverse = IntegerVector::Zero(n_cons);
            for (Index each_con=0; each_con < n_cons; each_con++){
                Index max_label;
                (con_stats_counts.row(i).segment(
                        each_con*n_bins*2, n_bins*2)).maxCoeff(&max_label);
                con_max_thres(each_con) = con_splits(each_con, max_label / 2);
                con_max_count(each_con) =
                        con_stats_counts(i, each_con*n_bins*2 + max_label);
                con_max_is_reverse(each_con) = (max_label % 2 == 1) ? 1 : 0;
            }

            // 2. Combine the best counts and sort them to get best
            // variable/splits in descending order
            ColumnVector all_counts(n_cats + n_cons);
            all_counts.segment(0, n_cats) = cat_max_count;
            all_counts.segment(n_cats, n_cons) = con_max_count;
            IntegerVector sorted_surr_indices = argsort(all_counts);

            // 3. Store the top max_n_surr (or fewer) surrogates in appropriate
            // data structures
            Index max_size = sorted_surr_indices.size() < max_n_surr  ?
                                sorted_surr_indices.size() : max_n_surr;
            Index surr_count = 0;
            for(Index j=0; j < max_size; j++){
                Index curr_surr = sorted_surr_indices(j);
                if (all_counts(curr_surr) < getMajorityCount(curr_node)){
                    break;
                }
                Index to_update_surr = curr_node * max_n_surr + surr_count;
                if (curr_surr < n_cats){    // curr_surr is categorical
                    // ensure primary not same as surrogate
                    if ((is_categorical(curr_node) != 1) ||
                            (feature_indices(curr_node) != curr_surr)) {
                        surr_indices(to_update_surr) = static_cast<int>(curr_surr);
                        surr_thresholds(to_update_surr) = cat_max_thres(curr_surr);
                        // reverse splits have negative status
                        surr_status(to_update_surr) =
                            (cat_max_is_reverse(curr_surr) == 1) ?
                                static_cast<int>(-1) : static_cast<int>(1);
                        surr_agreement(to_update_surr) = static_cast<int>(cat_max_count(curr_surr));
                        surr_count++;
                    }
                } else {  // curr_surr is continuous
                    curr_surr -= n_cats;  // continuous indices after the categorical

                    // ensure primary not same as surrogate
                    if ((is_categorical(curr_node) != 0) ||
                            (feature_indices(curr_node) != curr_surr)) {
                        surr_indices(to_update_surr) = static_cast<int>(curr_surr);
                        surr_thresholds(to_update_surr) = con_max_thres(curr_surr);
                        // reverse splits have negative status
                        surr_status(to_update_surr) =
                            (con_max_is_reverse(curr_surr) == 1) ?
                                static_cast<int>(-2) : static_cast<int>(2);
                        surr_agreement(to_update_surr) = static_cast<int>(con_max_count(curr_surr));
                        surr_count++;
                    }
                }
            }
        }
    }
}

// -------------------------------------------------------------------------

template <class Container>
template <class Accumulator>
inline
bool
DecisionTree<Container>::expand_by_sampling(const Accumulator &state,
                                const MappedMatrix &con_splits,
                                const uint16_t &min_split,
                                const uint16_t &min_bucket,
                                const uint16_t &max_depth,
                                const int &n_random_features) {

    uint16_t n_non_leaf_nodes = static_cast<uint16_t>(state.n_leaf_nodes - 1);
    bool children_not_allocated = true;
    bool children_wont_split = true;

    //select cat and con features to be sampled
    int total_cat_con_features = state.n_cat_features + state.n_con_features;
    const uint16_t &sps = state.stats_per_split;  // short form for brevity

    //store indicies from 0 to total_cat_con_features-1
    int *cat_con_feature_indices = new int[total_cat_con_features];
    NativeRandomNumberGenerator generator;
    uniform_int<int> uni_dist;
    variate_generator<NativeRandomNumberGenerator, uniform_int<int> > rvt(generator, uni_dist);

    for (Index i=0; i < state.n_leaf_nodes; i++) {
        Index current = n_non_leaf_nodes + i;
        if (feature_indices(current) == IN_PROCESS_LEAF) {
            // 1. Set the prediction for current node from stats of all rows
            predictions.row(current) = state.node_stats.row(i);

            for (int j=0; j<total_cat_con_features; j++) {
                cat_con_feature_indices[j] = j;
             }
            //randomly shuffle cat_con_feature_indices
            std::random_shuffle(cat_con_feature_indices,
                    cat_con_feature_indices + total_cat_con_features, rvt);
            // if a leaf node exists, compute the gain in impurity for each split
            // pick split  with maximum gain and update node with split value
            int max_feat = -1;
            Index max_bin = -1;
            bool max_is_cat = false;
            double max_impurity_gain = -std::numeric_limits<double>::infinity();
            ColumnVector max_stats;

            for (int index=0; index<n_random_features; index++) {
                int f = cat_con_feature_indices[index];
                if (f >= 0 && f < state.n_cat_features) {
                    //categorical feature
                    int v_end = f < 1 ? \
                            state.cat_levels_cumsum(0) : \
                            state.cat_levels_cumsum(f) - state.cat_levels_cumsum(f-1);
                    for (int v=0; v < v_end; ++v){
                        // each value of feature
                        Index fv_index = state.indexCatStats(f, v, true);
                        double gain = impurityGain(
                            state.cat_stats.row(i).segment(fv_index, sps * 2), sps);
                        if (gain > max_impurity_gain){
                            max_impurity_gain = gain;
                            max_feat = f;
                            max_bin = v;
                            max_is_cat = true;
                            max_stats = state.cat_stats.row(i).segment(fv_index,
                                                                       sps * 2);
                        }
                    }


                } else { //f >= state.n_cat.features
                    //continuous feature
                    f -= state.n_cat_features;
                    for (Index b=0; b < state.n_bins; ++b){
                        // each bin of feature
                        Index fb_index = state.indexConStats(f, b, true);
                        double gain = impurityGain(
                            state.con_stats.row(i).segment(fb_index, sps * 2), sps);
                        if (gain > max_impurity_gain){
                            max_impurity_gain = gain;
                            max_feat = f;
                            max_bin = b;
                            max_is_cat = false;
                            max_stats = state.con_stats.row(i).segment(fb_index,
                                                                       sps * 2);
                        }
                    }
                }
            }

            // create and update child nodes if splitting current
            if (max_impurity_gain > 0 &&
                    shouldSplit(max_stats, min_split, min_bucket, sps, max_depth)) {

                double max_threshold;
                if (max_is_cat)
                    max_threshold = static_cast<double>(max_bin);
                else
                    max_threshold = con_splits(max_feat, max_bin);

                if (children_not_allocated) {
                    // allocate the memory for child nodes if not allocated already
                    incrementInPlace();
                    children_not_allocated = false;
                }

                children_wont_split &=
                    updatePrimarySplit(
                        current, static_cast<int>(max_feat),
                        max_threshold, max_is_cat,
                        min_split,
                        max_stats.segment(0, sps),   // true_stats
                        max_stats.segment(sps, sps)  // false_stats
                    );

            } else {
                feature_indices(current) = FINISHED_LEAF;
            }
        } // if leaf exists
    } // for each leaf

    // return true if tree expansion is finished
    //      we check (tree_depth = max_depth + 1) since internally
    //      tree_depth starts from 1 though max_depth expects root node depth as 0
    bool training_finished = (children_not_allocated ||
                              tree_depth >= (max_depth + 1) ||
                              children_wont_split);
    if (training_finished){
        // label any remaining IN_PROCESS_LEAF as FINISHED_LEAF
        for (Index i=0; i < feature_indices.size(); i++) {
            if (feature_indices(i) == IN_PROCESS_LEAF)
                feature_indices(i) = FINISHED_LEAF;
        }
    }
    delete[] cat_con_feature_indices;
    return training_finished;
}

template <class Container>
inline
ColumnVector
DecisionTree<Container>::statPredict(const ColumnVector &stats) const {

    // stats is assumed to be of size = stats_per_split
    if (is_regression){
        // regression stat -> (0) = sum of weights, (1) = weighted sum of responses
        // we return the average response as prediction
        return ColumnVector(stats.segment(1, 1) / stats(0));
    } else {
        // classification stat ->  (i) = num of tuples for class i
        // we return the proportion of each label
        ColumnVector statsCopy(stats);
        return statsCopy.head(n_y_labels) / static_cast<double>(stats.head(n_y_labels).sum());
    }
}
// -------------------------------------------------------------------------

/**
 * @brief Return the number of tuples accounted in a 'stats' vector
 */
template <class Container>
inline
uint64_t
DecisionTree<Container>::statCount(const ColumnVector &stats) const{
    // stats is assumed to be of size = stats_per_split
    // for both regression and classification, the last element is the number
    // of tuples landing on this node.
    return static_cast<uint64_t>(stats.tail(1)(0));  // tail(n) returns a slice with last n elements
}
// -------------------------------------------------------------------------

/**
 * @brief Return the number of weighted tuples accounted in a 'stats' vector
 */
template <class Container>
inline
double
DecisionTree<Container>::statWeightedCount(const ColumnVector &stats) const{
    // stats is assumed to be of size = stats_per_split
    if (is_regression)
        return stats(0);
    else
        return stats.head(n_y_labels).sum();
}
// -------------------------------------------------------------------------

/**
 * @brief Return the number of tuples that landed on given node
 */
template <class Container>
inline
uint64_t
DecisionTree<Container>::nodeCount(const Index node_index) const{
    return statCount(predictions.row(node_index));
}
// -------------------------------------------------------------------------


/**
 * @brief Return the number of tuples (normalized using weights) that landed on given node
 */
template <class Container>
inline
double
DecisionTree<Container>::nodeWeightedCount(const Index node_index) const{
    return statWeightedCount(predictions.row(node_index));
}
// -------------------------------------------------------------------------


/*
 * Compute misclassification for prediction from a node in a classification tree.
 * For regression, a zero value is returned.
 * For classification, the difference between sum of weighted count and the max coefficient.
 *
 * @param node_index: Index of node for which to compute misclassification
 */
template <class Container>
inline
double
DecisionTree<Container>::computeMisclassification(Index node_index) const {
    if (is_regression) {
        return 0;
    } else {
        return predictions.row(node_index).head(n_y_labels).sum() -
                predictions.row(node_index).head(n_y_labels).maxCoeff();
    }
}
// -------------------------------------------------------------------------


/*
 * Compute risk for a node of the tree.
 * For regression, risk is the variance of the reponse at that node.
 * For classification, risk is the number of misclassifications at that node.
 *
 * @param node_index: Index of node for which to compute risk
 */
template <class Container>
inline
double
DecisionTree<Container>::computeRisk(const Index node_index) const {
    if (is_regression) {
        double wt_tot = predictions.row(node_index)(0);
        double y_avg = predictions.row(node_index)(1);
        double y2_avg = predictions.row(node_index)(2);

        if (wt_tot <= 0)
            return 0;
        else
            return (y2_avg - (y_avg * y_avg / wt_tot));
    } else {
        return computeMisclassification(node_index);
    }
}
// -------------------------------------------------------------------------


/**
 * @brief Return if a child node is pure
 */
template <class Container>
inline
bool
DecisionTree<Container>::isChildPure(const ColumnVector &stats) const{
    // stats is assumed to be of size = stats_per_split
    double epsilon = 1e-5;
    if (is_regression){
        // child is pure if variance is extremely small compared to mean
        double mean = stats(1) / stats(0);
        double variance = stats(2) / stats(0) - std::pow(mean, 2);
        return variance < epsilon * mean * mean;
    } else {
        // child is pure if most are of same class
        // return (statPredict(stats) / stats.head(n_y_labels).maxCoeff()).sum() < 100 * epsilon;
        double total_count = stats.head(n_y_labels).sum();
        double non_max_vals =  total_count - stats.head(n_y_labels).maxCoeff();
        return (non_max_vals/total_count) < 100*epsilon;
    }
}
// -------------------------------------------------------------------------

template <class Container>
inline
bool
DecisionTree<Container>::shouldSplit(const ColumnVector &combined_stats,
                                      const uint16_t &min_split,
                                      const uint16_t &min_bucket,
                                      const uint16_t &stats_per_split,
                                      const uint16_t &max_depth) const {

    // combined_stats is assumed to be of size = stats_per_split
    // we always want at least 1 tuple going into a child node. Hence the
    // minimum value for min_bucket is 1
    uint64_t thresh_min_bucket = (min_bucket == 0) ? 1u : min_bucket;
    uint64_t true_count = statCount(combined_stats.segment(0, stats_per_split));
    uint64_t false_count = statCount(combined_stats.segment(stats_per_split, stats_per_split));

    return ((true_count + false_count) >= min_split &&
            true_count >= thresh_min_bucket &&
            false_count >= thresh_min_bucket &&
            tree_depth <= max_depth);
}
// ------------------------------------------------------------------------

template <class Container>
inline
bool
DecisionTree<Container>::shouldSplitWeights(const ColumnVector &combined_stats,
                                      const uint16_t &min_split,
                                      const uint16_t &min_bucket,
                                      const uint16_t &stats_per_split) const {

    // combined_stats is assumed to be of size = stats_per_split
    // number of tuples landing on a node is equal to the sum of weights for
    // that node. we therefore use statWeightedCount
    // we always want at least 1 tuple going into a child node. Hence the
    // minimum value for min_bucket is 1
    uint64_t thresh_min_bucket = (min_bucket == 0) ? 1u : min_bucket;
    double true_count = statWeightedCount(combined_stats.segment(0, stats_per_split));
    double false_count = statWeightedCount(combined_stats.segment(stats_per_split, stats_per_split));
    return ((true_count + false_count) >= min_split &&
            true_count >= thresh_min_bucket &&
            false_count >= thresh_min_bucket);
}
// ------------------------------------------------------------------------

template <class Container>
inline
uint16_t
DecisionTree<Container>::recomputeTreeDepth() const{
    if (feature_indices.size() <= 1 || tree_depth <= 1)
        return tree_depth;

    for(uint16_t depth_counter = 2; depth_counter <= tree_depth; depth_counter++){
        uint32_t n_leaf_nodes = static_cast<uint16_t>(pow(2, depth_counter - 1));
        uint32_t leaf_start_index = n_leaf_nodes - 1;
        bool all_non_existing = true;
        for (uint32_t leaf_index=0; leaf_index < n_leaf_nodes; leaf_index++){
            if (feature_indices(leaf_start_index + leaf_index) != NODE_NON_EXISTING){
                all_non_existing = false;
                break;
            }
        }
        if (all_non_existing){
            // The previous level was the right depth since current has all
            // non-existing nodes. Return 1 less than current
            return static_cast<uint16_t>(depth_counter - 1);
        }
    }
    return tree_depth;
}
// -------------------------------------------------------------------------


/**
 * @brief Display the decision tree in dot format
 */
template <class Container>
inline
string
DecisionTree<Container>::displayLeafNode(
            Index id,
            ArrayHandle<text*> &dep_levels,
            const std::string & id_prefix){
    std::stringstream predict_str;
    if (static_cast<bool>(is_regression)){
        predict_str << predict_response(id);
    }
    else{
        std::string dep_value = get_text(dep_levels, static_cast<int>(predict_response(id)));
        predict_str << escape_quotes(dep_value);
    }

    std::stringstream display_str;
    display_str << "\"" << id_prefix << id << "\" [label=\"" << predict_str.str();

    // // uncomment below if distribution of rows is required in leaf node
    // display_str << "\\n[";
    // if (is_regression)
    //      display_str << statCount(predictions.row(id)) << ", "
    //                  << statPredict(predictions.row(id));
    // else
    //     display_str << predictions.row(id);
    // display_str << "]";
    display_str << "\",shape=box]" << ";";
    return display_str.str();
}
// -------------------------------------------------------------------------

/*
    @brief Display the decision tree in dot format
*/
template <class Container>
inline
string
DecisionTree<Container>::displayInternalNode(
            Index id,
            ArrayHandle<text*> &cat_features_str,
            ArrayHandle<text*> &con_features_str,
            ArrayHandle<text*> &cat_levels_text,
            ArrayHandle<int> &cat_n_levels,
            const std::string & id_prefix
            ){

    string feature_name;
    std::stringstream label_str;
    if (is_categorical(id) == 0) {
        feature_name = get_text(con_features_str, feature_indices(id));
        label_str << escape_quotes(feature_name) << " <= " << feature_thresholds(id);
    } else {
        feature_name = get_text(cat_features_str, feature_indices(id));
        label_str << escape_quotes(feature_name) << " in "
                   << getCatLabels(feature_indices(id),
                                   static_cast<Index>(0),
                                   static_cast<Index>(feature_thresholds(id)),
                                   cat_levels_text, cat_n_levels);
    }

    std::stringstream display_str;
    display_str << "\"" << id_prefix << id << "\" [label=\"" << label_str.str();
    // // uncomment below if distribution of rows is required in internal node
    // display_str << "\\n[";
    // if (is_regression)
    //      display_str << statCount(predictions.row(id)) << ", "
    //                  << statPredict(predictions.row(id));
    // else
    //     display_str << predictions.row(id);
    // display_str << "]";
    display_str <<"\", shape=ellipse]" << ";";
   return display_str.str();
}
// -------------------------------------------------------------------------

/**
 * @brief Display the decision tree in dot format
 */
template <class Container>
inline
string
DecisionTree<Container>::display(
        ArrayHandle<text*> &cat_features_str,
        ArrayHandle<text*> &con_features_str,
        ArrayHandle<text*> &cat_levels_text,
        ArrayHandle<int> &cat_n_levels,
        ArrayHandle<text*> &dependent_levels,
        const std::string &id_prefix) {

    std::stringstream display_string;
    if (feature_indices(0) == FINISHED_LEAF){
        display_string << displayLeafNode(0, dependent_levels, id_prefix)
                       << std::endl;
    }
    else{
        for(Index index = 0; index < feature_indices.size() / 2; index++) {
            if (feature_indices(index) != NODE_NON_EXISTING &&
                    feature_indices(index) != IN_PROCESS_LEAF &&
                    feature_indices(index) != FINISHED_LEAF) {

                display_string << displayInternalNode(
                        index, cat_features_str, con_features_str,
                        cat_levels_text, cat_n_levels, id_prefix) << std::endl;

                // Display the children
                Index tc = trueChild(index);
                if (feature_indices(tc) != NODE_NON_EXISTING) {
                    display_string << "\"" << id_prefix << index << "\" -> "
                                   << "\"" << id_prefix << tc << "\"";

                     // edge going left is "true" node
                    display_string << "[label=\"yes\"];" << std::endl;

                    if (feature_indices(tc) == IN_PROCESS_LEAF ||
                        feature_indices(tc) == FINISHED_LEAF)
                        display_string
                            << displayLeafNode(tc, dependent_levels, id_prefix)
                            << std::endl;
                }

                Index fc = falseChild(index);
                if (feature_indices(fc) != NODE_NON_EXISTING) {
                    display_string << "\"" << id_prefix << index << "\" -> "
                                   << "\"" << id_prefix << fc << "\"";

                    // root edge going right is "false" node
                    display_string << "[label=\"no\"];" << std::endl;

                    if (feature_indices(fc) == IN_PROCESS_LEAF ||
                        feature_indices(fc) == FINISHED_LEAF)
                        display_string
                            << displayLeafNode(fc, dependent_levels, id_prefix)
                            << std::endl;
                }
            }
        } // end of for loop
    }
    return display_string.str();
}
// -------------------------------------------------------------------------

template <class Container>
inline
string
DecisionTree<Container>::print_split(
        bool is_cat,
        bool is_reverse,
        Index feat_index,
        double feat_threshold,
        ArrayHandle<text*> &cat_features_str,
        ArrayHandle<text*> &con_features_str,
        ArrayHandle<text*> &cat_levels_text,
        ArrayHandle<int> &cat_n_levels){

    string feature_name;
    std::stringstream label_str;
    std::string compare;

    if (!is_cat) {
        if (!is_reverse)
            compare = " <= ";
        else
            compare = " > ";
        feature_name = get_text(con_features_str, feat_index);
        label_str << feature_name << compare << feat_threshold;
    } else {
        Index start_threshold;
        Index end_threshold;
        if (!is_reverse){
            start_threshold = 0;
            end_threshold = static_cast<Index>(feat_threshold);
        }
        else{
            start_threshold = static_cast<Index>(feat_threshold + 1);
            end_threshold = cat_n_levels[feat_index] - 1;
        }

        feature_name = get_text(cat_features_str, feat_index);
        label_str << feature_name << " in "
                  << getCatLabels(feat_index, start_threshold, end_threshold,
                                  cat_levels_text, cat_n_levels);
    }
    return label_str.str();
}
// -------------------------------------------------------------------------

template <class Container>
inline
string
DecisionTree<Container>::print(
        Index current,
        ArrayHandle<text*> &cat_features_str,
        ArrayHandle<text*> &con_features_str,
        ArrayHandle<text*> &cat_levels_text,
        ArrayHandle<int> &cat_n_levels,
        ArrayHandle<text*> &dep_levels,
        uint16_t recursion_depth){

    if (feature_indices(current) == NODE_NON_EXISTING){
        return "";
    }
    std::stringstream print_string;

    // print current node + prediction
    print_string << "(" << current << ")";
    print_string << "[";
    if (is_regression){
        print_string << nodeWeightedCount(current) << ", "
                     << statPredict(predictions.row(current));
    }
    else{
        print_string << predictions.row(current).head(n_y_labels);
    }
    print_string << "]  ";

    if (feature_indices(current) >= 0){
        string label_str = print_split(static_cast<bool>(is_categorical(current)),
                                       false,
                                       feature_indices(current),
                                       feature_thresholds(current),
                                       cat_features_str,
                                       con_features_str,
                                       cat_levels_text,
                                       cat_n_levels);

        print_string << label_str << std::endl;
        std::string indentation(recursion_depth * 3, ' ');
        print_string
            << indentation
            << print(trueChild(current), cat_features_str,
                     con_features_str, cat_levels_text,
                     cat_n_levels, dep_levels, static_cast<uint16_t>(recursion_depth + 1));

        print_string
            << indentation
            << print(falseChild(current), cat_features_str,
                     con_features_str, cat_levels_text,
                     cat_n_levels, dep_levels, static_cast<uint16_t>(recursion_depth + 1));
    } else {
        print_string << "*";
        if (!is_regression){
            std::string dep_value = get_text(dep_levels,
                                             static_cast<int>(predict_response(current)));
            print_string << " --> " << dep_value;
        }
        print_string << std::endl;
    }
    return print_string.str();
}
// -------------------------------------------------------------------------

template <class Container>
inline
string
DecisionTree<Container>::getCatLabels(Index cat_index,
                                      Index start_value,
                                      Index end_value,
                                      ArrayHandle<text*> &cat_levels_text,
                                      ArrayHandle<int> &cat_n_levels) {
    size_t to_skip = 0;
    for (Index i=0; i < cat_index; i++) {
        to_skip += cat_n_levels[i];
    }
    std::stringstream cat_levels;
    size_t start_index;
    cat_levels << "{";
    for (start_index = to_skip + start_value;
            start_index < to_skip + end_value &&
            start_index < cat_levels_text.size();
            start_index++) {
        cat_levels << get_text(cat_levels_text, start_index) << ",";
    }
    cat_levels << get_text(cat_levels_text, start_index) << "}";
    return cat_levels.str();
}
// -------------------------------------------------------------------------

template <class Container>
inline
int
DecisionTree<Container>::encodeIndex(const int &feature_index,
        const int &is_categorical, const int &n_cat_features) const {
    if (is_categorical != 0) {
        return feature_index;
    } else {
        if (feature_index >= 0) {
            return feature_index + n_cat_features;
        } else {
            return feature_index;
        }
    }
}
// -------------------------------------------------------------------------

template <class Container>
inline
string
DecisionTree<Container>::surr_display(
        ArrayHandle<text*> &cat_features_str,
        ArrayHandle<text*> &con_features_str,
        ArrayHandle<text*> &cat_levels_text,
        ArrayHandle<int> &cat_n_levels){

    if (max_n_surr <= 0 )
        return "";

    std::stringstream display_string;
    std::string indentation(5, ' ');
    for(Index curr_node=0; curr_node < feature_indices.size() / 2; curr_node++){
        Index feat_index = feature_indices(curr_node);
        if (feat_index != NODE_NON_EXISTING && feat_index != IN_PROCESS_LEAF &&
                feat_index != FINISHED_LEAF) {
            string feature_str = print_split(is_categorical(curr_node),
                                             false,
                                             feat_index,
                                             feature_thresholds(curr_node),
                                             cat_features_str,
                                             con_features_str,
                                             cat_levels_text,
                                             cat_n_levels);
            display_string << "(" << curr_node << ") ";
            display_string << feature_str
                << std::endl;
            Index surr_base = curr_node * max_n_surr;
            for(Index i = 0;
                    i < max_n_surr && surr_indices(surr_base + i) >= 0;
                    i++){

                Index curr_surr = surr_base + i;
                if (surr_indices(curr_surr) >= 0){
                    bool is_cat = std::abs(surr_status(curr_surr)) == 1? true : false;
                    bool is_reverse = surr_status(curr_surr) < 0 ? true : false;
                    string surr_str = print_split(is_cat,
                                                  is_reverse,
                                                  surr_indices(curr_surr),
                                                  surr_thresholds(curr_surr),
                                                  cat_features_str,
                                                  con_features_str,
                                                  cat_levels_text,
                                                  cat_n_levels);
                    display_string << indentation;
                    display_string << i + 1 << ": ";
                    display_string << surr_str
                        << "    [common rows = " << surr_agreement(curr_surr)<< "]"
                        << std::endl;
                }
            }
            display_string << indentation
                << "[Majority branch = " << getMajorityCount(curr_node) << " ]"
                << std::endl
                << std::endl;
        }
    }
    return display_string.str();
}

// ------------------------------------------------------------------------
// Definitions for class TreeAccumulator
// ------------------------------------------------------------------------
template <class Container, class DTree>
inline
TreeAccumulator<Container, DTree>::TreeAccumulator(
        Init_type& inInitialization): Base(inInitialization){
    this->initialize();
}

/**
 * @brief Bind all elements of the state to the data in the stream
 *
 * The bind() is special in that even after running operator>>() on an element,
 * there is no guarantee yet that the element can indeed be accessed. It is
 * cruicial to first check this.
 *
 * Provided that this methods correctly lists all member variables, all other
 * methods can, however, rely on that fact that all variables are correctly
 * initialized and accessible.
 */
template <class Container, class DTree>
inline
void
TreeAccumulator<Container, DTree>::bind(ByteStream_type& inStream) {
    // update with actual parameters
    inStream >> n_rows
             >> terminated
             >> n_bins
             >> n_cat_features
             >> n_con_features
             >> total_n_cat_levels
             >> n_leaf_nodes
             >> stats_per_split
             >> weights_as_rows ;

    uint16_t n_bins_tmp = 0;
    uint16_t n_cat = 0;
    uint16_t n_con = 0;
    uint32_t tot_levels = 0;
    uint16_t n_leafs = 0;
    uint16_t n_stats = 0;

    if (!n_rows.isNull()){
        n_bins_tmp = n_bins;
        n_cat = n_cat_features;
        n_con = n_con_features;
        tot_levels = total_n_cat_levels;
        n_leafs = n_leaf_nodes;
        n_stats = stats_per_split;
    }

    inStream
        >> cat_levels_cumsum.rebind(n_cat)
        >> cat_stats.rebind(n_leafs, tot_levels * n_stats * 2)
        >> con_stats.rebind(n_leafs, n_con * n_bins_tmp * n_stats * 2)
        >> node_stats.rebind(n_leafs, n_stats);
}
// -------------------------------------------------------------------------

/**
 * @brief Rebind all elements of the state when dimensionality elements are
 *  available
 *
 */
template <class Container, class DTree>
inline
void
TreeAccumulator<Container, DTree>::rebind(
        uint16_t in_n_bins, uint16_t in_n_cat_feat,
        uint16_t in_n_con_feat, uint32_t in_n_total_levels,
        uint16_t tree_depth, uint16_t in_n_stats, bool in_weights_as_rows) {

    n_bins = in_n_bins;
    n_cat_features = in_n_cat_feat;
    n_con_features = in_n_con_feat;
    total_n_cat_levels = in_n_total_levels;
    weights_as_rows = in_weights_as_rows;
    if (tree_depth > 0)
        n_leaf_nodes = static_cast<uint16_t>(pow(2, tree_depth - 1));
    else
        n_leaf_nodes = 1;
    stats_per_split = in_n_stats;
    this->resize();
}
// -------------------------------------------------------------------------

/**
 * @brief Update the accumulation state by feeding a tuple
 */
template <class Container, class DTree>
inline
TreeAccumulator<Container, DTree>&
TreeAccumulator<Container, DTree>::operator<<(const tuple_type& inTuple) {
    tree_type dt = std::get<0>(inTuple);
    const MappedIntegerVector& cat_features = std::get<1>(inTuple);
    const MappedColumnVector& con_features = std::get<2>(inTuple);
    const double& response = std::get<3>(inTuple);
    const double& weight = std::get<4>(inTuple);
    const MappedIntegerVector& cat_levels = std::get<5>(inTuple);
    const MappedMatrix& con_splits = std::get<6>(inTuple);

    // The following checks were introduced with MADLIB-138. It still seems
    // useful to have clear error messages in case of infinite input values.
    if (!terminated){
        if (!std::isfinite(response)) {
            warning("Decision tree response variable values are not finite.");
        } else if ((cat_features.size() + con_features.size()) >
                        std::numeric_limits<uint16_t>::max()) {
            warning("Number of independent variables cannot be larger than 65535.");
        } else if (n_cat_features != static_cast<uint16_t>(cat_features.size())) {
            warning("Inconsistent numbers of categorical independent variables.");
        } else if (n_con_features != static_cast<uint16_t>(con_features.size())) {
            warning("Inconsistent numbers of continuous independent variables.");
        } else{
            uint16_t n_non_leaf_nodes = static_cast<uint16_t>(n_leaf_nodes - 1);
            Index dt_search_index = dt.search(cat_features, con_features);
            if (dt.feature_indices(dt_search_index) != dt.FINISHED_LEAF &&
                 dt.feature_indices(dt_search_index) != dt.NODE_NON_EXISTING) {
                Index row_index = dt_search_index - n_non_leaf_nodes;
                assert(row_index >= 0);
                // add this row into the stats for the node
                updateNodeStats(static_cast<bool>(dt.is_regression), row_index,
                                response, weight);

                // update stats for categorical feature values in the current row
                for (Index i=0; i < n_cat_features; ++i){
                    for (int j=0; j < cat_levels(i); ++j){
                        if (!dt.isNull(cat_features(i), true)){
                            Index col_index = indexCatStats(
                                    i, j, (cat_features(i) <= j));
                            updateStats(static_cast<bool>(dt.is_regression), true,
                                    row_index, col_index, response, weight);
                        }
                    }
                }
                // update stats for continuous feature values in the current row
                for (Index i=0; i < n_con_features; ++i){
                    for (Index j=0; j < n_bins; ++j){
                        if (!dt.isNull(con_features(i), false)){
                            Index col_index = indexConStats(i, j,
                                    (con_features(i) <= con_splits(i, j)));
                            updateStats(static_cast<bool>(dt.is_regression), false,
                                    row_index, col_index, response, weight);
                        }
                    }
                }
            }
            n_rows++;
            return *this;
        }
        // error case for current group
        terminated = true;
    }
    return *this;
}
// -------------------------------------------------------------------------


/**
 * @brief Update the accumulation state for surrogate statistics by feeding a tuple
 */
template <class Container, class DTree>
inline
TreeAccumulator<Container, DTree>&
TreeAccumulator<Container, DTree>::operator<<(const surr_tuple_type& inTuple) {

    tree_type dt = std::get<0>(inTuple);
    const MappedIntegerVector& cat_features = std::get<1>(inTuple);
    const MappedColumnVector& con_features = std::get<2>(inTuple);
    const MappedIntegerVector& cat_levels = std::get<3>(inTuple);
    const MappedMatrix& con_splits = std::get<4>(inTuple);
    const int dup_count = std::get<5>(inTuple);

    if ((cat_features.size() + con_features.size()) >
                    std::numeric_limits<uint16_t>::max()) {
        warning("Number of independent variables cannot be larger than 65535.");
    } else if (n_cat_features != static_cast<uint16_t>(cat_features.size())) {
        warning("Inconsistent numbers of categorical independent variables.");
    } else if (n_con_features != static_cast<uint16_t>(con_features.size())) {
        warning("Inconsistent numbers of continuous independent variables.");
    } else{
        // the accumulator is setup to train for the 2nd last layer
        // hence the n_leaf_nodes is same as n_surr_nodes
        uint16_t n_surr_nodes = n_leaf_nodes;
        uint16_t n_non_surr_nodes = static_cast<uint16_t>(n_surr_nodes - 1);

        Index dt_parent_index = dt.parentIndex(dt.search(cat_features, con_features));

        Index primary_index = dt.feature_indices(dt_parent_index);
        bool is_primary_cat = static_cast<bool>(dt.is_categorical(dt_parent_index));
        double primary_val = is_primary_cat ? cat_features(primary_index) :
                                              con_features(primary_index);

        // We only capture statistics for rows that:
        //  1. lead to leaf nodes in the last layer. Surrogates for other nodes
        //      have already been trained.
        //  2. have non-null values for the primary split.
        if (dt_parent_index >= n_non_surr_nodes &&
                !dt.isNull(primary_val, is_primary_cat)) {

            double primary_threshold = dt.feature_thresholds(dt_parent_index);
            bool is_primary_true = (primary_val <= primary_threshold);

            if (dt.feature_indices(dt_parent_index) >= 0){
                Index row_index = dt_parent_index - n_non_surr_nodes;

                assert(row_index >= 0 && row_index < cat_stats.rows() &&
                       row_index < con_stats.rows());

                for (Index i=0; i < n_cat_features; ++i){
                    if (is_primary_cat && i == primary_index)
                        continue;
                    for (int j=0; j < cat_levels(i); ++j){
                        if (!dt.isNull(cat_features(i), true)){
                            // we don't capture stats when surrogate is NULL
                            bool is_surrogate_true = (cat_features(i) <= j);
                            Index col_index = indexCatStats(i, j, is_surrogate_true);
                            updateSurrStats(true,
                                            is_primary_true == is_surrogate_true,
                                            row_index,
                                            col_index, 
                                            dup_count);
                        }
                    }
                }
                for (Index i=0; i < n_con_features; ++i){
                    if (!is_primary_cat && i == primary_index)
                        continue;
                    for (Index j=0; j < n_bins; ++j){
                        if (!dt.isNull(con_features(i), false)){
                            // we don't capture stats when surrogate is NULL
                            bool is_surrogate_true = (con_features(i) <= con_splits(i, j));
                            Index col_index = indexConStats(i, j, is_surrogate_true);
                            updateSurrStats(false,
                                            is_primary_true == is_surrogate_true,
                                            row_index,
                                            col_index,
                                            dup_count);
                        }
                    }
                }
            }
            n_rows++;
        }
    }
    return *this;
}

/**
 * @brief Merge with another accumulation state
 */
template <class Container, class DTree>
template <class C, class DT>
inline
TreeAccumulator<Container, DTree>&
TreeAccumulator<Container, DTree>::operator<<(
        const TreeAccumulator<C, DT>& inOther) {
    // assuming that (*this) is not empty. This check needs to happen before
    // this function is called.
    if (!inOther.empty()) {
        if ((n_bins != inOther.n_bins) ||
               (n_cat_features != inOther.n_cat_features) ||
               (n_con_features != inOther.n_con_features)) {
            warning("Inconsistent states during merge.");
            terminated = true;
        } else {
            cat_stats += inOther.cat_stats;
            con_stats += inOther.con_stats;
            node_stats += inOther.node_stats;
        }
    }
    return *this;
}
// -------------------------------------------------------------------------

/**
 * @brief Update the node statistics for given node
 */
template <class Container, class DTree>
inline
void
TreeAccumulator<Container, DTree>::updateNodeStats(bool is_regression,
                                                  Index node_index,
                                                  const double response,
                                                  const double weight) {
    ColumnVector stats(stats_per_split);
    stats.fill(0);
    int n_rows = this->weights_as_rows ? static_cast<int>(weight) : 1; 
    if (is_regression){
        double w_response = weight * response;
        stats << weight, w_response, w_response * response, n_rows;
    } else {
        assert(response >= 0);
        stats(static_cast<uint16_t>(response)) = weight;
        stats.tail(1)(0) = n_rows;
    }
    node_stats.row(node_index) += stats;
}
// -------------------------------------------------------------------------

/**
 * @brief Update the leaf node statistics for current row in given feature/bin
 */
template <class Container, class DTree>
inline
void
TreeAccumulator<Container, DTree>::updateStats(bool is_regression,
                                               bool  is_cat,
                                               Index row_index,
                                               Index stats_index,
                                               const double response,
                                               const double weight) {
    ColumnVector stats(stats_per_split);
    stats.fill(0);
    int n_rows = this->weights_as_rows ? static_cast<int>(weight) : 1; 
    if (is_regression){
        double w_response = weight * response;
        stats << weight, w_response, w_response * response, n_rows;
    } else {
        stats(static_cast<uint16_t>(response)) = weight;
        stats.tail(1)(0) = n_rows;
    }

    if (is_cat) {
        cat_stats.row(row_index).segment(stats_index, stats_per_split) += stats;
    } else {
        con_stats.row(row_index).segment(stats_index, stats_per_split) += stats;
    }
}
// -------------------------------------------------------------------------

/**
 * @brief Update the surrogate statistics for current row in given feature/bin
 */
template <class Container, class DTree>
inline
void
TreeAccumulator<Container, DTree>::updateSurrStats(
        const bool is_cat, const bool surr_agrees,
        Index row_index, Index stats_index, const int dup_count) {

    // Note: the below works only if stats_per_split = 2
    // 1st position for <= surrogate split and
    // 2nd position for > split
    ColumnVector stats(stats_per_split);
    if (surr_agrees)
        stats << dup_count, 0;
    else
        stats << 0, dup_count;

    if (is_cat) {
        cat_stats.row(row_index).segment(stats_index, stats_per_split) += stats;
    } else {
        con_stats.row(row_index).segment(stats_index, stats_per_split) += stats;
    }
}
// -------------------------------------------------------------------------

template <class Container, class DTree>
inline
Index
TreeAccumulator<Container, DTree>::indexConStats(Index feature_index,
                                                 Index bin_index,
                                                 bool  is_split_true) const {
    assert(feature_index < n_con_features);
    assert(bin_index < n_bins);
    return computeSubIndex(feature_index * n_bins, bin_index, is_split_true);
}
// -------------------------------------------------------------------------

template <class Container, class DTree>
inline
Index
TreeAccumulator<Container, DTree>::indexCatStats(Index feature_index,
                                                 int   cat_value,
                                                 bool  is_split_true) const {
    // cat_stats is a matrix
    //   size = (n_leaf_nodes) x (total_n_cat_levels * stats_per_split * 2)
    assert(feature_index < n_cat_features);
    unsigned int cat_cumsum_value = (feature_index == 0) ? 0 : cat_levels_cumsum(feature_index - 1);
    return computeSubIndex(static_cast<Index>(cat_cumsum_value),
                           static_cast<Index>(cat_value),
                           is_split_true);
}
// -------------------------------------------------------------------------

template <class Container, class DTree>
inline
Index
TreeAccumulator<Container, DTree>::computeSubIndex(Index start_index,
                                                   Index relative_index,
                                                   bool is_split_true) const {
    Index col_index = static_cast<Index>(stats_per_split * 2 *
                                         (start_index + relative_index));
    return is_split_true ? col_index : col_index + stats_per_split;
}
//-------------------------------------------------------------------------

} // namespace recursive_partitioning
} // namespace modules
} // namespace madlib

#endif // defined(MADLIB_MODULES_RP_DT_IMPL_HPP)

