#include "reductions.h"
#include "pmf_to_pdf.h"
#include "explore.h"
#include "vw.h"
#include "debug_log.h"

using namespace LEARNER;
using namespace VW;
using namespace VW::config;

VW_DEBUG_ENABLE(false)

namespace VW { namespace pmf_to_pdf {

  pdf_data::~pdf_data()
  {
    temp_lbl_cb.costs.delete_v();
    temp_pred_a_s.delete_v();
  }

  void transform_pred_pdf(pmf_to_pdf::pdf_data& data, example& ec)
  {
    auto& continuous_scores = data.scores;
    continuous_scores.clear();
    continuous_scores.resize(data.num_actions, 0.f);
    float continuous_range = data.max_value - data.min_value;
    for (uint32_t i = 0; i < data.num_actions; i++)
    {
      auto& a_s = data.temp_pred_a_s[i];
      uint32_t min_h = (std::max)((int)0, static_cast<int>(i) - static_cast<int>(data.bandwidth));
      uint32_t max_h = (std::min)(data.num_actions, i + data.bandwidth);
      uint32_t bandwidth_range = max_h - min_h;
      float continuous_mass = a_s.score * data.num_actions / ((float)bandwidth_range * continuous_range);
      for (uint32_t j = min_h; j < max_h; j++)
      {
        continuous_scores[j] += continuous_mass;
      }
    }
    auto& p_dist = ec.pred.prob_dist;
    p_dist.clear();
    for (uint32_t i = 0; i < data.num_actions; i++)
    {
      float action = data.min_value + i * continuous_range / data.num_actions;
      p_dist.push_back({action, continuous_scores[i]});
    }
    p_dist.push_back({data.max_value, 0.f});
  }

  void predict(pmf_to_pdf::pdf_data& data, single_learner& base, example& ec)
  {
    auto temp_pred = ec.pred;
    auto temp_lbl = ec.l;
    ec.pred.a_s = data.temp_pred_a_s;
    ec.l.cb = data.temp_lbl_cb;

    base.predict(ec);

    VW_DBG(ec) << "pmf_to_pdf::predict base.predict()" << a_s_pred_to_string(ec) << std::endl;

    data.temp_pred_a_s = ec.pred.a_s;
    ec.pred = temp_pred;
    transform_pred_pdf(data, ec);
    data.temp_lbl_cb = ec.l.cb;
    ec.l = temp_lbl;
    VW_DBG(ec) << "pmf_to_pdf::predict transform()" << prob_dist_pred_to_string(ec) << std::endl;
  }

  void learn(pmf_to_pdf::pdf_data& data, single_learner& base, example& ec)
  {
    float cost = ec.l.cb_cont.costs[0].cost;
    float prob = ec.l.cb_cont.costs[0].probability;
    float continuous_range = data.max_value - data.min_value;
    float action_cont = ec.l.cb_cont.costs[0].action;
    float ac = (action_cont - data.min_value) * data.num_actions / continuous_range;
    int ic = (int)floor(ac);
    bool cond1 = data.min_value + ic * continuous_range / data.num_actions <= action_cont;
    bool cond2 = action_cont < data.min_value + (ic + 1) * continuous_range / data.num_actions;

    if (!cond1 || !cond2)
    {
      if (!cond1)
      {
        ic--;
      }

      if (!cond2)
      {
        ic++;
      }
    }

    uint32_t min_value = (std::max)(0, ic - (int)data.bandwidth + 1);
    uint32_t max_value = (std::min)(data.num_actions - 1, ic + data.bandwidth);

    auto saved_lbl = ec.l;
    ec.l.cb = data.temp_lbl_cb;
    ec.l.cb.costs.clear();
    for (uint32_t j = min_value; j <= max_value; j++)
    {
      uint32_t min_h = (std::max)(0, (int)j - (int)data.bandwidth);
      uint32_t max_h = (std::min)(data.num_actions, j + data.bandwidth);
      uint32_t bandwidth_range = max_h - min_h;
      ec.l.cb.costs.push_back({cost, j + 1, prob * bandwidth_range, 0.0f});
    }

    auto saved_pred = ec.pred;
    ec.pred.a_s = data.temp_pred_a_s;

    base.learn(ec);

    data.temp_pred_a_s = ec.pred.a_s;
    data.temp_lbl_cb = ec.l.cb;
    ec.pred = saved_pred;
    ec.l = saved_lbl;
  }

  void finish(pdf_data& data) {
    data.~pdf_data();
  }

  void print_update(vw& all, bool is_test, example& ec, std::stringstream& pred_string)
  {
    if (all.sd->weighted_examples() >= all.sd->dump_interval && !all.quiet && !all.bfgs)
    {
      std::stringstream label_string;
      if (is_test)
        label_string << " unknown";
      else
      {
        const auto& cost = ec.l.cb.costs[0];
        label_string << cost.action << ":" << cost.cost << ":" << cost.probability;
      }
      all.sd->print_update(all.holdout_set_off, all.current_pass, label_string.str(), pred_string.str(), ec.num_features,
          all.progress_add, all.progress_arg);
    }
  }

  inline bool observed_cost(CB::cb_class* cl)
  {
    // cost observed for this action if it has non zero probability and cost != FLT_MAX
    return (cl != nullptr && cl->cost != FLT_MAX && cl->probability > .0);
  }

  CB::cb_class* get_observed_cost(CB::label& ld)
  {
    for (auto& cl : ld.costs)
      if (observed_cost(&cl))
        return &cl;
    return nullptr;
  }

  void output_example(vw& all, pdf_data& , example& ec, CB::label& ld)
  {
    float loss = 0.;

    if (get_observed_cost(ec.l.cb) != nullptr)
      for (auto& cbc: ec.l.cb.costs)
        for (uint32_t i = 0; i < ec.pred.prob_dist.size(); i++)
          loss += (cbc.cost / cbc.probability) * ec.pred.prob_dist[i].value;

    all.sd->update(ec.test_only, get_observed_cost(ld) != nullptr, loss, 1.f, ec.num_features);

    char temp_str[20];
    std::stringstream ss, sso;
    float maxprob = 0.;
    uint32_t maxid = 0;
    for (uint32_t i = 0; i < ec.pred.prob_dist.size(); i++)
    {
      sprintf(temp_str, "%f ", ec.pred.prob_dist[i].value);
      ss << temp_str;
      if (ec.pred.prob_dist[i].value > maxprob)
      {
        maxprob = ec.pred.prob_dist[i].value;
        maxid = i + 1;
      }
    }

    sprintf(temp_str, "%d:%f", maxid, maxprob);
    sso << temp_str;

    for (int sink : all.final_prediction_sink) all.print_text(sink, ss.str(), ec.tag);

    print_update(all, CB::cb_label.test_label(&ld), ec, sso);
  }

  void finish_example(vw& all, pdf_data& c, example& ec)
  {
    output_example(all, c, ec, ec.l.cb);
    VW::finish_example(all, ec);
  }

  base_learner* pmf_to_pdf_setup(options_i& options, vw& all)
  {
    auto data = scoped_calloc_or_throw<pmf_to_pdf::pdf_data>();

    option_group_definition new_options("CB Continuous");
    new_options
        .add(make_option("cb_continuous", data->num_actions)
                 .default_value(0)
                 .keep()
                 .help("Convert discrete PDF into continuous PDF."))
        .add(make_option("min_value", data->min_value).keep().help("Minimum continuous value"))
        .add(make_option("max_value", data->max_value).keep().help("Maximum continuous value"))
        .add(make_option("bandwidth", data->bandwidth)
                 .default_value(1)
                 .keep()
                 .help("Bandwidth (radius) of randomization around discrete actions in number of actions."));
    options.add_and_parse(new_options);

    if (data->num_actions == 0)
      return nullptr;
    if (!options.was_supplied("cb_continuous"))
      return nullptr;
    if (!options.was_supplied("cb_explore"))
    {
      std::stringstream ss;
      ss << data->num_actions;
      options.insert("cb_explore", ss.str());
    }
    if (!options.was_supplied("min_value") || !options.was_supplied("max_value"))
    {
      THROW("error: min and max values must be supplied with cb_continuous");
    }
    if (data->bandwidth <= 0)
    {
      THROW("error: Bandwidth must be >= 1");
    }

    learner<pmf_to_pdf::pdf_data, example>& l = init_learner(data, as_singleline(setup_base(options, all)), learn, predict, 1, prediction_type_t::prob_dist);

    l.set_finish(finish);

    return make_base(l);
  }

}}  // namespace VW::pmf_to_pdf
