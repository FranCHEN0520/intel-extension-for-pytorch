#!/usr/bin/env python
# coding=utf-8
# Copyright 2020 The HuggingFace Inc. team. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
from pathlib import Path
import argparse
from typing import List, Optional
from transformers import (
    AutoConfig,
)
import subprocess


def main(args_in: Optional[List[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="Generation script")

    # general arguments.
    parser.add_argument(
        "-m",
        "--model-name-or-path",
        type=str,
        help="huggingface model id or local directory containing model files",
    )
    parser.add_argument(
        "--config-file",
        default=None,
        type=str,
        help="local specific model configuration file",
    )
    parser.add_argument(
        "--dtype",
        type=str,
        choices=["float32", "bfloat16"],
        default="bfloat16",
        help="bfloat16, float32",
    )
    parser.add_argument("--ipex", action="store_true")
    parser.add_argument("--output-dir", nargs="?", default="./saved_results")

    # quantization related arguments.
    parser.add_argument("--int8", action="store_true")
    parser.add_argument(
        "--int8-bf16-mixed",
        action="store_true",
        help="by default it is int8-fp32 mixed, to enable int8 mixed amp bf16 (work on platforms like SPR)",
    )
    parser.add_argument("--quantized-model-path", default="")

    parser.add_argument("--dataset", nargs="?", default="NeelNanda/pile-10k")
    parser.add_argument("--ipex-smooth-quant", action="store_true")
    parser.add_argument("--alpha", default=0.5, type=float, help="alpha value for smoothquant")
    parser.add_argument(
        "--ipex-weight-only-quantization",
        action="store_true",
        help="use ipex weight-only quantization",
    )

    parser.add_argument(
        "--lowp-mode",
        choices=["AUTO", "BF16", "FP32", "INT8", "FP16"],
        default="AUTO",
        type=str,
        help="low precision mode for weight only quantization. "
        "It indicates data type for computation for speedup at the cost "
        "of accuracy. Unrelated to activation or weight data type."
        "It is not supported yet to use lowp_mode=INT8 for INT8 weight, "
        "falling back to lowp_mode=BF16 implicitly in this case."
        "If set to AUTO, lowp_mode is determined by weight data type: "
        "lowp_mode=BF16 is used for INT8 weight "
        "and lowp_mode=INT8 used for INT4 weight",
    )
    parser.add_argument(
        "--weight-dtype",
        choices=["INT8", "INT4"],
        default="INT8",
        type=str,
        help="weight data type for weight only quantization. Unrelated to activation"
        " data type or lowp-mode. If `--low-precision-checkpoint` is given, weight"
        " data type is always INT4 and this argument is not needed.",
    )
    parser.add_argument(
        "--low-precision-checkpoint",
        default="",
        type=str,
        help="Low precision checkpoint file generated by calibration, such as GPTQ. It contains"
        " modified weights, scales, zero points, etc. For better accuracy of weight only"
        " quantization with INT4 weight.",
    )
    parser.add_argument("--gptq", action="store_true")

    # inference related arguments.
    parser.add_argument(
        "--max-new-tokens", default=32, type=int, help="output max new tokens"
    )
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--input-tokens", default="32", type=str)
    parser.add_argument("--prompt", default=None, type=str)
    parser.add_argument("--num-iter", default=100, type=int, help="num iter")
    parser.add_argument("--num-warmup", default=10, type=int, help="num warmup")
    parser.add_argument("--batch-size", default=1, type=int, help="batch size")
    parser.add_argument("--token-latency", action="store_true")
    parser.add_argument("--greedy", action="store_true")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--deployment-mode", action="store_true")

    # deepspeed inference related arguments.
    parser.add_argument("--autotp", action="store_true")
    parser.add_argument("--shard-model", action="store_true")
    parser.add_argument(
        "--local_rank", required=False, type=int, help="used by dist launchers"
    )
    args = parser.parse_args(args_in)

    parent_path = Path(__file__).parent.absolute()

    if not args.autotp:
        if not args.ipex_weight_only_quantization and not args.ipex_smooth_quant:
            path = Path(parent_path, "single_instance/run_generation.py")
            infer_cmd = ["python", path]
            infer_cmd.extend(["-m", str(args.model_name_or_path)])
            infer_cmd.extend(["--dtype", str(args.dtype)])
            infer_cmd.extend(["--input-tokens", str(args.input_tokens)])
            infer_cmd.extend(["--max-new-tokens", str(args.max_new_tokens)])
            infer_cmd.extend(["--num-iter", str(args.num_iter)])
            infer_cmd.extend(["--num-warmup", str(args.num_warmup)])
            infer_cmd.extend(["--batch-size", str(args.batch_size)])

            if args.greedy:
                infer_cmd.extend(["--greedy"])
            if args.ipex:
                infer_cmd.extend(["--ipex"])
            if args.deployment_mode:
                infer_cmd.extend(["--deployment-mode"])
            if args.profile:
                infer_cmd.extend(["--profile"])
            if args.benchmark:
                infer_cmd.extend(["--benchmark"])
            if args.token_latency:
                infer_cmd.extend(["--token-latency"])

            if args.prompt is not None:
                infer_cmd.extend(["--prompt", str(args.prompt)])
            if args.prompt is not None:
                infer_cmd.extend(["--config-file", str(args.config_file)])

            print("running model geneartion...")
            subprocess.run(infer_cmd)
        else:
            if args.config_file is None:
                config = AutoConfig.from_pretrained(
                    args.model_name_or_path, trust_remote_code=True
                )
            else:
                config = AutoConfig.from_pretrained(
                    args.config_file, trust_remote_code=True
                )
            import re

            if re.search("falcon", config.architectures[0], re.IGNORECASE) or re.search(
                "rw", config.architectures[0], re.IGNORECASE
            ):
                qpath = Path(parent_path, "single_instance/run_falcon_quantization.py")
            elif re.search("GPTJ", config.architectures[0], re.IGNORECASE):
                qpath = Path(parent_path, "single_instance/run_gpt-j_quantization.py")
            elif re.search("llama", config.architectures[0], re.IGNORECASE):
                qpath = Path(parent_path, "single_instance/run_llama_quantization.py")
            elif re.search("gptneox", config.architectures[0], re.IGNORECASE):
                qpath = Path(
                    parent_path, "single_instance/run_gpt-neox_quantization.py"
                )
            elif re.search("OPT", config.architectures[0], re.IGNORECASE):
                qpath = Path(parent_path, "single_instance/run_opt_quantization.py")

            infer_cmd = ["python", qpath]
            # 1) quantization
            if args.quantized_model_path == "":
                quant_cmd = ["python", qpath]
                quant_cmd.extend(["-m", str(args.model_name_or_path)])
                quant_cmd.extend(["--output-dir", str(args.output_dir)])

                if args.int8_bf16_mixed:
                    quant_cmd.extend(["--int8-bf16-mixed"])
                if args.int8:
                    quant_cmd.extend(["--int8"])
                if args.greedy:
                    quant_cmd.extend(["--greedy"])
                if args.ipex_weight_only_quantization:
                    quant_cmd.extend(["--ipex-weight-only-quantization"])
                    quant_cmd.extend(["--weight-dtype", str(args.weight_dtype)])
                    quant_cmd.extend(["--lowp-mode", str(args.lowp_mode)])
                    if args.gptq:
                        if args.low_precision_checkpoint == "":
                            gptq_cmd = [
                                "python",
                                Path(parent_path, "utils/run_gptq.py"),
                            ]
                            gptq_cmd.extend(["--model", str(args.model_name_or_path)])
                            gptq_cmd.extend(["--output-dir", str(args.output_dir)])
                            subprocess.run(gptq_cmd)
                            quant_cmd.extend(
                                [
                                    "--low-precision-checkpoint",
                                    str(args.output_dir) + "/gptq_checkpoint.pt",
                                ]
                            )
                        else:
                            quant_cmd.extend(
                                [
                                    "--low-precision-checkpoint",
                                    str(args.low_precision_checkpoint),
                                ]
                            )
                else:
                    quant_cmd.extend(["--ipex-smooth-quant"])
                    quant_cmd.extend(["--alpha", str(args.alpha)])
                    quant_cmd.extend(["--dataset", str(args.dataset)])
                print("quantizing model ...")
                subprocess.run(quant_cmd)
                infer_cmd.extend(
                    ["--quantized-model-path", str(args.output_dir) + "/best_model.pt"]
                )
            else:
                infer_cmd.extend(
                    ["--quantized-model-path", str(args.quantized_model_path)]
                )

            # 2) inference
            infer_cmd.extend(["-m", str(args.model_name_or_path)])
            infer_cmd.extend(["--input-tokens", str(args.input_tokens)])
            infer_cmd.extend(["--max-new-tokens", str(args.max_new_tokens)])
            infer_cmd.extend(["--num-iter", str(args.num_iter)])
            infer_cmd.extend(["--num-warmup", str(args.num_warmup)])
            infer_cmd.extend(["--batch-size", str(args.batch_size)])

            if args.int8_bf16_mixed:
                infer_cmd.extend(["--int8-bf16-mixed"])
            if args.greedy:
                infer_cmd.extend(["--greedy"])
            if args.profile:
                infer_cmd.extend(["--profile"])
            if args.benchmark:
                infer_cmd.extend(["--benchmark"])
            if args.token_latency:
                infer_cmd.extend(["--token-latency"])

            if args.prompt is not None:
                infer_cmd.extend(["--prompt", str(args.prompt)])
            if args.prompt is not None:
                infer_cmd.extend(["--config-file", str(args.config_file)])

            print("running model geneartion...")
            subprocess.run(infer_cmd)

    else:
        path = Path(parent_path, "distributed/run_generation_with_deepspeed.py")
        infer_cmd = ["python", path]
        if args.shard_model:
            spath = Path(parent_path, "utils/create_shard_model.py")
            shard_cmd = ["python", spath]
            shard_cmd.extend(["-m", str(args.model_name_or_path)])
            MODEL_CLASSES = {
                "gpt-j": ("/gptj_local_shard"),
                "gpt-neox": ("/gptneox_local_shard"),
                "llama": ("/llama_local_shard"),
                "opt": ("/opt_local_shard"),
                "falcon": ("/falcon_local_shard"),
            }
            model_type = next(
                (x for x in MODEL_CLASSES.keys() if x in args.model_name_or_path.lower()), "auto"
            )
            work_path = Path(str(args.output_dir))
            if not work_path.exists():
                Path.mkdir(work_path)
                model_path = Path(str(args.output_dir)+str(MODEL_CLASSES[model_type]))
                if not model_path.exists():
                    Path.mkdir(model_path)
            shard_cmd.extend(["--save-path", str(args.output_dir)+str(MODEL_CLASSES[model_type])])
            shard_cmd.extend(["--local_rank", str(args.local_rank)])
            subprocess.run(shard_cmd)
            infer_cmd.extend(["-m", str(args.output_dir)+str(MODEL_CLASSES[model_type])])
        else:
            infer_cmd.extend(["-m", str(args.model_name_or_path)])

        infer_cmd.extend(["--dtype", str(args.dtype)])
        infer_cmd.extend(["--input-tokens", str(args.input_tokens)])
        infer_cmd.extend(["--max-new-tokens", str(args.max_new_tokens)])
        infer_cmd.extend(["--num-iter", str(args.num_iter)])
        infer_cmd.extend(["--num-warmup", str(args.num_warmup)])
        infer_cmd.extend(["--batch-size", str(args.batch_size)])
        infer_cmd.extend(["--local_rank", str(args.local_rank)])
        if args.greedy:
            infer_cmd.extend(["--greedy"])
        if args.ipex:
            infer_cmd.extend(["--ipex"])
        if args.deployment_mode:
            infer_cmd.extend(["--deployment-mode"])
        if args.profile:
            infer_cmd.extend(["--profile"])
        if args.benchmark:
            infer_cmd.extend(["--benchmark"])
        if args.token_latency:
            infer_cmd.extend(["--token-latency"])

        if args.prompt is not None:
            infer_cmd.extend(["--prompt", str(args.prompt)])
        if args.config_file is not None:
            infer_cmd.extend(["--config-file", str(args.config_file)])

        if args.ipex_weight_only_quantization:
            infer_cmd.extend(["--ipex-weight-only-quantization"])
            infer_cmd.extend(["--weight-dtype", str(args.weight_dtype)])
            infer_cmd.extend(["--lowp-mode", str(args.lowp_mode)])
            if args.int8_bf16_mixed:
                infer_cmd.extend(["--int8-bf16-mixed"])

        print("running model geneartion with deepspeed (autotp)...")
        subprocess.run(infer_cmd)


if __name__ == "__main__":
    main()
