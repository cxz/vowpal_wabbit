#include "cats.h"
#include "parse_args.h"
#include "err_constants.h"
#include "api_status.h"
#include "debug_log.h"

// Aliases
using LEARNER::single_learner;
using std::endl;
using VW::cb_continuous::continuous_label;
using VW::cb_continuous::continuous_label_elm;
using VW::config::make_option;
using VW::config::option_group_definition;
using VW::config::options_i;

// Enable/Disable indented debug statements
VW_DEBUG_ENABLE(false)

// Forward declarations
namespace VW
{
void finish_example(vw& all, example& ec);
}

namespace VW
{
namespace continuous_action
{
namespace cats
{
////////////////////////////////////////////////////
// BEGIN cats reduction and reduction methods
struct cats
{
  int learn(example& ec, api_status* status);
  int predict(example& ec, api_status* status);

  // TODO: replace with constructor after merge with master
  void init(single_learner* p_base);

 private:
  single_learner* _base = nullptr;
};

// Pass through
int cats::predict(example& ec, api_status* status = nullptr)
{
  VW_DBG(ec) << "cats::predict(), " << features_to_string(ec) << endl;
  _base->predict(ec);
  return error_code::success;
}

// Pass through
int cats::learn(example& ec, api_status* status = nullptr)
{
  assert(!ec.test_only);
  predict(ec, status);
  VW_DBG(ec) << "cats::learn(), " << cont_label_to_string(ec) << features_to_string(ec) << endl;
  _base->learn(ec);
  return error_code::success;
}

void cats::init(single_learner* p_base) { _base = p_base; }

// Free function to tie function pointers to reduction class methods
template <bool is_learn>
void predict_or_learn(cats& reduction, single_learner&, example& ec)
{
  api_status status;
  if (is_learn)
    reduction.learn(ec, &status);
  else
    reduction.predict(ec, &status);

  if (status.get_error_code() != error_code::success)
  {
    VW_DBG(ec) << status.get_error_msg() << endl;
  }
}

// END cats reduction and reduction methods
/////////////////////////////////////////////////

///////////////////////////////////////////////////
// BEGIN: functions to output progress
class reduction_output
{
 public:
  static void report_progress(vw& all, cats&, example& ec);
  static void output_predictions(v_array<int>& predict_file_descriptors, actions_pdf::action_pdf_value& prediction);

 private:
  static inline bool does_example_have_label(example& ec);
  static void print_update_cb_cont(vw& all, example& ec);
};

// Free function to tie function pointers to output class methods
void finish_example(vw& all, cats& data, example& ec)
{
  // add output example
  reduction_output::report_progress(all, data, ec);
  reduction_output::output_predictions(all.final_prediction_sink, ec.pred.a_pdf);
  VW::finish_example(all, ec);
}

void reduction_output::output_predictions(
    v_array<int>& predict_file_descriptors, actions_pdf::action_pdf_value& prediction)
{
  // output to the prediction to all files
  const std::string str = to_string(prediction, true);
  for (const int f : predict_file_descriptors)
    if (f > 0)
      io_buf::write_file_or_socket(f, str.c_str(), str.size());
}

// "average loss" "since last" "example counter" "example weight"
// "current label" "current predict" "current features"
void reduction_output::report_progress(vw& all, cats&, example& ec)
{
  const auto& cb_cont_costs = ec.l.cb_cont.costs;
  all.sd->update(ec.test_only, does_example_have_label(ec), cb_cont_costs.empty() ? 0.f : cb_cont_costs[0].cost,
      ec.weight, ec.num_features);
  all.sd->weighted_labels += ec.weight;
  print_update_cb_cont(all, ec);
}

inline bool reduction_output::does_example_have_label(example& ec)
{
  return (!ec.l.cb_cont.costs.empty() && ec.l.cb_cont.costs[0].action != FLT_MAX);
}

void reduction_output::print_update_cb_cont(vw& all, example& ec)
{
  if (all.sd->weighted_examples() >= all.sd->dump_interval && !all.quiet && !all.bfgs)
  {
    all.sd->print_update(all.holdout_set_off, all.current_pass,
        to_string(ec.l.cb_cont.costs[0]),  // Label
        to_string(ec.pred.a_pdf),          // Prediction
        ec.num_features, all.progress_add, all.progress_arg);
  }
}

// END: functions to output progress
////////////////////////////////////////////////////

// Setup reduction in stack
LEARNER::base_learner* setup(options_i& options, vw& all)
{
  option_group_definition new_options("Continuous action tree with smoothing");
  int num_actions = 0, pdf_num_actions = 0;
  new_options.add(make_option("cats", num_actions).keep().help("Continuous action tree with smoothing"))
      .add(make_option("cats_pdf", pdf_num_actions).keep().help("Continuous action tree with smoothing (pdf)"));

  options.add_and_parse(new_options);

  // If cats reduction was not invoked, don't add anything
  // to the reduction stack;
  if (!options.was_supplied("cats"))
    return nullptr;

  if (num_actions <= 0)
    THROW(error_code::num_actions_gt_zero_s);

  // cats stack = [cats -> sample_pdf -> cats_pdf ... rest specified by cats_pdf]
  if (!options.was_supplied("sample_pdf"))
    options.insert("sample_pdf", "");

  if (options.was_supplied("cats_pdf"))
  {
    if (pdf_num_actions != num_actions)
      THROW(error_code::action_counts_disagree_s);
  }
  else
  {
    std::stringstream strm;
    strm << num_actions;
    options.insert("cats_pdf", strm.str());
  }

  LEARNER::base_learner* p_base = setup_base(options, all);
  auto p_reduction = scoped_calloc_or_throw<cats>();
  p_reduction->init(as_singleline(p_base));

  LEARNER::learner<cats, example>& l = init_learner(p_reduction, as_singleline(p_base), predict_or_learn<true>,
      predict_or_learn<false>, 1, prediction_type::action_pdf_value);

  l.set_finish_example(finish_example);

  return make_base(l);
}
}  // namespace cats
}  // namespace continuous_action
}  // namespace VW
