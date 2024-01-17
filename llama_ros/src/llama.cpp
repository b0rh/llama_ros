// MIT License

// Copyright (c) 2023  Miguel Ángel González Santamarta

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cassert>
#include <cmath>
#include <thread>

#include "common.h"
#include "llama_ros/llama.hpp"

using namespace llama_ros;

Llama::Llama(rclcpp::Logger logger, const struct gpt_params &params, bool debug)
    : logger(logger), params(params), debug(debug) {

  // disable llama.cpp logs
  log_disable();

  // load the model
  llama_backend_init(this->params.numa);
  std::tie(this->model, this->ctx) = llama_init_from_gpt_params(this->params);
  this->ctx_sampling = nullptr;

  // show system information
  RCLCPP_INFO(this->logger, "System_info: n_threads = %d / %d | %s",
              this->params.n_threads, std::thread::hardware_concurrency(),
              llama_print_system_info());

  // number of tokens to keep when resetting context
  if (this->params.n_keep == -1) {
    this->params.n_keep = (int)this->prompt_tokens.size();
  }

  this->canceled = false;
  this->n_past = 0;
  this->n_remain = this->params.n_predict;
  this->n_consumed = 0;
  this->ga_i = 0;

  // show info
  RCLCPP_INFO(this->logger,
              "Generate: n_ctx = %d, n_batch = %d, n_predict = %d, n_keep = %d",
              this->get_n_ctx(), this->params.n_batch, this->params.n_predict,
              this->params.n_keep);

  if (this->params.grp_attn_n != 1) {
    if (this->params.grp_attn_n > 0) {
      GGML_ASSERT("grp_attn_n must be positive\n");
    }

    if (this->params.grp_attn_w % this->params.grp_attn_n != 0) {
      GGML_ASSERT("grp_attn_w must be a multiple of grp_attn_n\n");
    }
  }

  RCLCPP_INFO(this->logger,
              "self-extend: n_ctx_train = %d, grp_attn_n = %d, grp_attn_w = %d",
              this->get_n_ctx_train(), this->params.grp_attn_n,
              this->params.grp_attn_w);
}

Llama::~Llama() {

  if (this->ctx_sampling != nullptr) {
    llama_sampling_free(this->ctx_sampling);
  }

  llama_free(this->ctx);
  llama_free_model(this->model);
  llama_backend_free();
}

std::vector<llama_token> Llama::tokenize(const std::string &text, bool add_bos,
                                         bool special) {
  std::lock_guard<std::recursive_mutex> lk(this->mutex);
  return llama_tokenize(this->ctx, text, add_bos, special);
}

std::string Llama::detokenize(const std::vector<llama_token> &tokens) {
  std::lock_guard<std::recursive_mutex> lk(this->mutex);
  return llama_detokenize_bpe(this->ctx, tokens);
}

void Llama::reset() {

  llama_kv_cache_seq_rm(this->ctx, -1, 0, -1);

  if (this->ctx_sampling != nullptr) {
    llama_sampling_reset(this->ctx_sampling);
  }

  this->canceled = false;
  this->n_past = 0;
  this->n_remain = this->params.n_predict;
  this->n_consumed = 0;
  this->ga_i = 0;

  this->prompt_tokens.clear();
  this->params.sparams.n_prev = this->get_n_ctx();
}

void Llama::cancel() { this->canceled = true; }

std::vector<float> Llama::generate_embeddings(const std::string &input_prompt) {

  std::lock_guard<std::recursive_mutex> lk(this->mutex);

  const int n_embd = this->get_n_embd();

  if (!this->is_embedding()) {
    RCLCPP_ERROR(
        this->logger,
        "Llama must be created with embedding=true to create embeddings");
    return std::vector<float>(n_embd, 0.0f);
  }

  auto tokens =
      this->tokenize(input_prompt, this->should_add_bos_token(), false);

  if ((int)tokens.size() > this->get_n_ctx()) {
    RCLCPP_ERROR(this->logger, "Prompt too long %ld, context size is %d",
                 tokens.size(), this->get_n_ctx());
    return std::vector<float>(n_embd, 0.0f);
  }

  int embd_n_past = 0;
  int embd_n_eval = 0;
  llama_kv_cache_seq_rm(this->ctx, -1, 0, -1);

  for (size_t i = 0; i < tokens.size(); i += this->params.n_batch) {

    embd_n_eval = (int)tokens.size() - i;
    if (embd_n_eval > this->params.n_batch) {
      embd_n_eval = this->params.n_batch;
    }

    if (llama_decode(this->ctx, llama_batch_get_one(&tokens[i], embd_n_eval,
                                                    embd_n_past, 0))) {
      RCLCPP_ERROR(this->logger, "Failed to eval");
      return std::vector<float>(n_embd, 0.0f);
    }
    embd_n_past += embd_n_eval;
  }

  const auto embeddings = llama_get_embeddings(this->ctx);
  std::vector<float> embeddings_list(embeddings, embeddings + n_embd);

  return embeddings_list;
}

std::vector<struct completion_output>
Llama::generate_response(const std::string &input_prompt, bool add_pfx_sfx,
                         GenerateResponseCallback callback) {

  std::lock_guard<std::recursive_mutex> lk(this->mutex);

  this->canceled = false;
  std::vector<llama_token> batch_tokens;

  struct completion_output completion_result;
  std::vector<struct completion_output> response;
  std::vector<struct completion_output> completion_result_list;

  // load params
  if (this->ctx_sampling == nullptr) {
    this->ctx_sampling = llama_sampling_init(this->params.sparams);
  } else {
    this->update_sampling_params(this->params.sparams);
  }

  // load prompt
  if (!this->load_prompt(input_prompt, add_pfx_sfx)) {
    return response;
  }

  // show sampling info
  RCLCPP_INFO(this->logger,
              "Sampling: temp = %f, "
              "top_k = %d, "
              "top_p = %f, "
              "penalty_last_n = %i, "
              "repeat_penalty = %f",
              params.sparams.temp, params.sparams.top_k, params.sparams.top_p,
              params.sparams.penalty_last_n, params.sparams.penalty_repeat);

  RCLCPP_INFO(this->logger, "Starting Response Generation");

  // initial eval
  if (!this->init_eval()) {
    return response;
  }

  // generation loop
  while (this->n_remain != 0) {

    stop_type stopping =
        this->find_stop(completion_result_list, this->params.antiprompt);

    if (stopping == FULL_STOP) {
      break;

    } else if (stopping == PARTIAL_STOP) {
      RCLCPP_INFO(this->logger, "Partial stopping word found");

    } else if (stopping == NO_STOP) {
      if (completion_result_list.size()) {
        for (auto completion_ele : completion_result_list) {
          if (callback != nullptr) {
            callback(completion_ele);
          }
          response.push_back(completion_ele);
        }
        completion_result_list.clear();
      }
    }

    // sample next token
    completion_result = this->sample();
    completion_result_list.push_back(completion_result);
    batch_tokens.push_back(completion_result.token);
    --this->n_remain;

    // next eval
    if (!this->eval(batch_tokens)) {
      break;
    }

    batch_tokens.clear();
  }

  RCLCPP_INFO(this->logger, "Finish Response Generation");
  return response;
}

bool Llama::load_prompt(const std::string &input_prompt, bool add_pfx_sfx) {

  std::vector<llama_token> inp_pfx = this->tokenize(
      this->params.input_prefix,
      this->should_add_bos_token() && !this->prompt_tokens.size(), true);
  std::vector<llama_token> inp_sfx =
      this->tokenize(this->params.input_suffix, false, true);

  std::string prompt(input_prompt);
  std::vector<llama_token> line_inp;

  if (prompt.size() <= 0) {
    return false;
  }

  if (!this->prompt_tokens.size() && !add_pfx_sfx) {
    line_inp = this->tokenize(prompt, this->should_add_bos_token(), true);
  } else {
    line_inp = this->tokenize(prompt, false, false);
  }

  int prompt_size = this->prompt_tokens.size() + line_inp.size();
  if (add_pfx_sfx && this->params.input_prefix.size()) {
    prompt_size += inp_pfx.size() + inp_sfx.size();
  }

  // insert prefix
  if (add_pfx_sfx && this->params.input_prefix.size()) {

    const int n_prev = 64;
    const std::string last_output =
        llama_sampling_prev_str(this->ctx_sampling, this->ctx, n_prev);

    // check if prefix is already added
    if (last_output.find(
            this->params.input_prefix.c_str(),
            last_output.length() - this->params.input_prefix.length(),
            this->params.input_prefix.length()) == std::string::npos) {

      this->prompt_tokens.insert(this->prompt_tokens.end(), inp_pfx.begin(),
                                 inp_pfx.end());
    }
  }

  this->prompt_tokens.insert(this->prompt_tokens.end(), line_inp.begin(),
                             line_inp.end());

  // insert suffix
  if (add_pfx_sfx && this->params.input_suffix.size()) {
    this->prompt_tokens.insert(this->prompt_tokens.end(), inp_sfx.begin(),
                               inp_sfx.end());
  }

  this->n_remain -= line_inp.size();
  return true;
}

stop_type
Llama::find_stop(std::vector<struct completion_output> completion_result_list,
                 std::vector<std::string> stopping_words) {

  // check if stop appears at the end of the output
  const int n_prev = 32;
  const std::string last_output =
      llama_sampling_prev_str(this->ctx_sampling, this->ctx, n_prev);

  if (this->params.antiprompt.at(0).size() &&
      last_output.find(
          this->params.antiprompt.at(0).c_str(),
          last_output.length() - this->params.antiprompt.at(0).length(),
          this->params.antiprompt.at(0).length()) != std::string::npos) {
    return FULL_STOP;
  }

  // eos
  if (llama_sampling_last(this->ctx_sampling) == this->get_token_eos()) {
    return FULL_STOP;
  }

  // action server is canceled
  if (this->canceled) {
    RCLCPP_INFO(this->logger, "Canceling llama.cpp");
    return FULL_STOP;
  }

  // respect the maximum number of tokens
  if (this->n_remain <= 0 && this->params.n_predict != -1) {
    this->n_remain = this->params.n_predict;
    return FULL_STOP;
  }

  if (this->n_past > this->get_n_ctx() && this->params.n_predict == -2) {
    return FULL_STOP;
  }

  for (auto w : stopping_words) {
    stop_type s = this->find_stop_word(completion_result_list, w);
    if (s != NO_STOP) {
      return s;
    }
  }

  return NO_STOP;
}

stop_type Llama::find_stop_word(
    std::vector<struct completion_output> completion_result_list,
    std::string stopping_word) {

  // check new token sequence size
  if (completion_result_list.size() <= stopping_word.size() &&
      stopping_word.size()) {

    std::string completion_text = "";
    for (auto c : completion_result_list) {
      completion_text.append(this->detokenize({c.token}));
    }

    for (size_t i = 0; i < completion_text.size(); i++) {
      if (completion_text.at(i) != stopping_word.at(i)) {
        return NO_STOP;
      }
    }

    if (completion_text.size() == stopping_word.size()) {
      return FULL_STOP;
    } else {
      return PARTIAL_STOP;
    }
  }

  return NO_STOP;
}

bool Llama::init_eval() {
  std::vector<llama_token> batch_tokens;

  while (((int)this->prompt_tokens.size() > this->n_consumed)) {

    while (((int)this->prompt_tokens.size() > this->n_consumed) &&
           ((int)batch_tokens.size() < this->params.n_batch)) {
      batch_tokens.push_back(this->prompt_tokens[this->n_consumed]);
      llama_sampling_accept(this->ctx_sampling, this->ctx,
                            this->prompt_tokens[this->n_consumed], false);
      ++this->n_consumed;
    }

    if (!this->eval(batch_tokens)) {
      return false;
    }

    batch_tokens.clear();
  }

  return true;
}

bool Llama::eval(std::vector<llama_token> batch_tokens) {

  if (batch_tokens.size() > 0) {

    // shift context
    if (this->params.grp_attn_n == 1) {
      if (this->n_past + (int)batch_tokens.size() > this->get_n_ctx()) {

        const int n_left = this->n_past - this->params.n_keep - 1;
        const int n_discard = n_left / 2;

        llama_kv_cache_seq_rm(this->ctx, 0, this->params.n_keep + 1,
                              this->params.n_keep + n_discard + 1);
        llama_kv_cache_seq_shift(this->ctx, 0,
                                 this->params.n_keep + 1 + n_discard, n_past,
                                 -n_discard);

        this->n_past -= n_discard;
      }

    } else {
      // context extension via Self-Extend
      int ga_n = this->params.grp_attn_n;
      int ga_w = this->params.grp_attn_w;

      while (this->n_past >= this->ga_i + ga_w) {
        const int ib = (ga_n * this->ga_i) / ga_w;
        const int bd = (ga_w / ga_n) * (ga_n - 1);
        const int dd = (ga_w / ga_n) - ib * bd - ga_w;

        llama_kv_cache_seq_shift(this->ctx, 0, this->ga_i, this->n_past,
                                 ib * bd);
        llama_kv_cache_seq_div(this->ctx, 0, this->ga_i + ib * bd,
                               this->ga_i + ib * bd + ga_w, ga_n);
        llama_kv_cache_seq_shift(this->ctx, 0, this->ga_i + ib * bd + ga_w,
                                 this->n_past + ib * bd, dd);

        this->n_past -= bd;

        this->ga_i += ga_w / ga_n;
      }
    }

    // evaluate tokens in batches
    // batch_tokens is typically prepared beforehand to fit within a batch
    // but not always
    int n_eval = 0;
    for (size_t i = 0; i < batch_tokens.size(); i += this->params.n_batch) {

      n_eval = (int)batch_tokens.size() - i;
      if (n_eval > this->params.n_batch) {
        n_eval = this->params.n_batch;
      }

      if (this->debug) {
        spinner.spin("EVALUATING " + std::to_string(n_eval) + " TOKENS");
      }

      if (llama_decode(this->ctx, llama_batch_get_one(&batch_tokens[i], n_eval,
                                                      this->n_past, 0))) {
        RCLCPP_ERROR(this->logger, "Failed to eval");
        return false;
      }
      this->n_past += n_eval;
    }
  }

  return true;
}

std::vector<token_prob> Llama::get_probs() {
  std::vector<token_prob> probs;

  llama_token_data_array cur_p = {this->ctx_sampling->cur.data(),
                                  this->ctx_sampling->cur.size(), false};

  const int32_t n_probs = this->params.sparams.n_probs;
  if (this->params.sparams.temp <= 0 && n_probs > 0) {
    // For llama_sample_token_greedy we need to sort candidates
    llama_sample_softmax(this->ctx, &cur_p);
  }

  for (size_t i = 0; i < std::min(cur_p.size, (size_t)n_probs); ++i) {
    probs.push_back({cur_p.data[i].id, cur_p.data[i].p});
  }

  return probs;
}

struct completion_output Llama::sample() {

  // sample token
  llama_token id = llama_sampling_sample(this->ctx_sampling, this->ctx, NULL);
  llama_sampling_accept(this->ctx_sampling, this->ctx, id, true);

  // create output
  struct completion_output result;
  result.token = id;
  result.probs = this->get_probs();

  // return result
  return result;
}

void Llama::update_sampling_params(const struct llama_sampling_params &params) {

  this->ctx_sampling->params = params;

  // reload grammar
  if (this->ctx_sampling->grammar != nullptr) {
    llama_grammar_free(this->ctx_sampling->grammar);
  }
  this->ctx_sampling->grammar = nullptr;

  // if there is a grammar, parse it
  if (!params.grammar.empty()) {
    this->ctx_sampling->parsed_grammar =
        grammar_parser::parse(params.grammar.c_str());

    // will be empty (default) if there are parse errors
    if (this->ctx_sampling->parsed_grammar.rules.empty()) {
      RCLCPP_ERROR(this->logger, "Failed to parse grammar");
      return;
    }

    std::vector<const llama_grammar_element *> grammar_rules(
        this->ctx_sampling->parsed_grammar.c_rules());

    this->ctx_sampling->grammar = llama_grammar_init(
        grammar_rules.data(), grammar_rules.size(),
        this->ctx_sampling->parsed_grammar.symbol_ids.at("root"));
  }
}