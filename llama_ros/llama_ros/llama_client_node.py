#!/usr/bin/env python3

# MIT License

# Copyright (c) 2023  Miguel Ángel González Santamarta

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.


import time
import rclpy
from rclpy.node import Node
from rclpy.action import ActionClient
from llama_msgs.action import GenerateResponse


class LlamaClientNode(Node):

    def __init__(self) -> None:
        super().__init__("llama_client_node")

        self.declare_parameter(
            "prompt", "Do you know the city of León from Spain? Can you tell me a bit about its history?")
        self.prompt = self.get_parameter(
            "prompt").get_parameter_value().string_value
        self.prompt = self.prompt.replace("\\n", "\n")

        self.tokens = 0
        self.initial_time = -1
        self.eval_time = -1

        self._get_result_future = None
        self._action_client = ActionClient(
            self, GenerateResponse, "/llama/generate_response")

    def text_cb(self, msg) -> None:

        if self.eval_time < 0:
            self.eval_time = time.time()

        feedback: GenerateResponse.Feedback = msg.feedback
        self.tokens += 1
        print(feedback.partial_response.text, end="", flush=True)

    def send_prompt(self) -> None:

        goal = GenerateResponse.Goal()
        goal.prompt = self.prompt
        goal.sampling_config.temp = 0.2
        goal.sampling_config.penalty_last_n = 8

        self._action_client.wait_for_server()
        send_goal_future = self._action_client.send_goal_async(
            goal, feedback_callback=self.text_cb)

        rclpy.spin_until_future_complete(self, send_goal_future)
        get_result_future = send_goal_future.result().get_result_async()

        self.initial_time = time.time()

        rclpy.spin_until_future_complete(self, get_result_future)
        # result: GenerateResponse.Result = get_result_future.result().result

        self.get_logger().info("END")
        end_time = time.time()
        self.get_logger().info(
            f"Time to eval: {self.eval_time - self.initial_time} s")
        self.get_logger().info(
            f"Prediction speed: {self.tokens / (end_time - self.eval_time)} t/s")


def main():

    rclpy.init()
    node = LlamaClientNode()
    node.send_prompt()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
