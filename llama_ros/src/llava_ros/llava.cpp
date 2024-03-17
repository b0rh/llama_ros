// MIT License

// Copyright (c) 2024  Miguel Ángel González Santamarta

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

#include "base64.hpp"
#include "common.h"
#include "llava_ros/llava.hpp"

using namespace llava_ros;

Llava::Llava(rclcpp::Logger logger, std::shared_ptr<struct gpt_params> params,
             bool debug)
    : llama_ros::Llama(logger, params, debug) {

  // load clip model
  const char *clip_path = this->params->mmproj.c_str();
  auto ctx_clip = clip_model_load(clip_path, 1);
  this->image_embed = nullptr;

  // create llava ctx
  this->ctx_llava = (struct llava_context *)malloc(sizeof(llava_context));

  this->ctx_llava->ctx_llama = this->ctx;
  this->ctx_llava->ctx_clip = ctx_clip;
  this->ctx_llava->model = this->model;
}

Llava::~Llava() {
  if (this->ctx_llava->ctx_clip) {
    clip_free(this->ctx_llava->ctx_clip);
    this->ctx_llava->ctx_clip = NULL;
  }

  this->free_image();

  this->llama_ros::Llama::~Llama();

  free(this->ctx_llava);
}

/*
*****************************
*        LOAD IMAGE         *
*****************************
*/
bool Llava::load_prompt(const std::string &input_prompt, bool add_pfx_sfx) {
  if (input_prompt.size() == 0 && this->image_embed != nullptr) {
    return true;
  }

  return Llama::load_prompt(input_prompt, add_pfx_sfx);
}

bool Llava::load_image(std::string base64_str) {

  this->free_image();

  this->image_embed = this->base64_image_to_embed(base64_str);

  if (this->image_embed == nullptr) {
    RCLCPP_ERROR(this->logger, "Can't load base64 image");
    return false;
  }

  return true;
}

void Llava::free_image() {
  if (this->image_embed != nullptr) {
    llava_image_embed_free(this->image_embed);
    this->image_embed = nullptr;
  }
}

struct llava_image_embed *
Llava::base64_image_to_embed(const std::string &base64_str) {

  auto required_bytes = base64::required_encode_size(base64_str.size());
  auto img_bytes = std::vector<unsigned char>(required_bytes);
  base64::decode(base64_str.begin(), base64_str.end(), img_bytes.begin());

  auto embed = llava_image_embed_make_with_bytes(
      this->ctx_llava->ctx_clip, this->params->n_threads, img_bytes.data(),
      img_bytes.size());

  if (!embed) {
    RCLCPP_ERROR(this->logger, "Could not load image from base64 string");
    return nullptr;
  }

  return embed;
}

/*
*****************************
*        EVAL IMAGE         *
*****************************
*/
bool Llava::eval_image(struct llava_image_embed *image_embed) {

  int n_embd = this->get_n_embd();
  bool succ = true;

  for (int i = 0; i < image_embed->n_image_pos; i += this->params->n_batch) {

    int n_eval = image_embed->n_image_pos - i;

    if (n_eval > this->params->n_batch) {
      n_eval = this->params->n_batch;
    }

    llama_batch batch = {
        int32_t(n_eval),
        nullptr,
        (image_embed->embed + i * n_embd),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        this->n_past,
        1,
        0,
    };

    if (!this->eval(batch)) {
      RCLCPP_ERROR(this->logger, "Failed in image eval");
      succ = false;
      break;
    }
  }

  this->free_image();
  return succ;
}

bool Llava::eval_prompt() {

  // eval the image
  if (this->image_embed != nullptr) {
    RCLCPP_INFO(this->logger, "Evaluating the image");

    if (!this->eval_image(this->image_embed)) {
      return false;
    }
  }

  // eval the rest of the prompt
  return llama_ros::Llama::eval_prompt();
}
