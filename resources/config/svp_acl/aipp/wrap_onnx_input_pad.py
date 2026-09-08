#!/usr/bin/env python3
"""在 ONNX 图输入后插入常量 Pad，供 CV610 动态 AIPP 输出有效图区域。"""

import argparse
import sys

import onnx
from onnx import TensorProto, checker, helper


SUPPORTED_TYPES = {
    TensorProto.FLOAT,
    TensorProto.FLOAT16,
    TensorProto.DOUBLE,
    TensorProto.UINT8,
    TensorProto.INT8,
    TensorProto.UINT16,
    TensorProto.INT16,
    TensorProto.INT32,
    TensorProto.INT64,
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="把较小的 AIPP 图输入 Pad 到原模型输入尺寸（NCHW）。"
    )
    parser.add_argument("--input-model", required=True)
    parser.add_argument("--output-model", required=True)
    parser.add_argument(
        "--input-name",
        help="原模型图输入名；模型只有一个业务输入时可以省略",
    )
    parser.add_argument(
        "--new-input-name",
        help="包装后的外部输入名；默认在原名后增加 _aipp",
    )
    parser.add_argument(
        "--content-size",
        nargs=2,
        type=int,
        required=True,
        metavar=("WIDTH", "HEIGHT"),
    )
    parser.add_argument(
        "--pads",
        nargs=4,
        type=int,
        required=True,
        metavar=("LEFT", "TOP", "RIGHT", "BOTTOM"),
    )
    parser.add_argument("--value", type=float, default=114.0)
    return parser.parse_args()


def dim_value(value_info, index):
    dim = value_info.type.tensor_type.shape.dim[index]
    return dim.dim_value if dim.HasField("dim_value") else None


def unique_name(graph, base):
    names = {item.name for item in graph.input}
    names.update(item.name for item in graph.output)
    names.update(item.name for item in graph.initializer)
    for node in graph.node:
        names.update(node.input)
        names.update(node.output)
        if node.name:
            names.add(node.name)
    if base not in names:
        return base
    suffix = 1
    while f"{base}_{suffix}" in names:
        suffix += 1
    return f"{base}_{suffix}"


def main():
    args = parse_args()
    model = onnx.load(args.input_model)
    graph = model.graph

    initializers = {item.name for item in graph.initializer}
    business_inputs = [item for item in graph.input if item.name not in initializers]
    if args.input_name:
        matches = [item for item in business_inputs if item.name == args.input_name]
        if not matches:
            raise ValueError(f"找不到业务输入: {args.input_name}")
        original_input = matches[0]
    elif len(business_inputs) == 1:
        original_input = business_inputs[0]
    else:
        names = ", ".join(item.name for item in business_inputs)
        raise ValueError(f"模型有多个业务输入，请指定 --input-name，可选值: {names}")

    tensor_type = original_input.type.tensor_type
    if tensor_type.elem_type not in SUPPORTED_TYPES:
        raise ValueError(f"不支持的输入数据类型: {tensor_type.elem_type}")
    dims = tensor_type.shape.dim
    if len(dims) != 4:
        raise ValueError("只支持 NCHW 四维输入")

    content_w, content_h = args.content_size
    left, top, right, bottom = args.pads
    if min(content_w, content_h) <= 0 or min(left, top, right, bottom) < 0:
        raise ValueError("content-size 必须为正数，pads 不得为负数")

    old_h = dim_value(original_input, 2)
    old_w = dim_value(original_input, 3)
    expected_h = content_h + top + bottom
    expected_w = content_w + left + right
    if old_h != expected_h or old_w != expected_w:
        raise ValueError(
            f"原输入为 {old_w}x{old_h}，但有效区加补边为 {expected_w}x{expected_h}"
        )

    default_new_name = f"{original_input.name}_aipp"
    new_input_name = args.new_input_name or unique_name(graph, default_new_name)
    if new_input_name == original_input.name:
        raise ValueError("新输入名必须与原输入名不同")
    if args.new_input_name and unique_name(graph, new_input_name) != new_input_name:
        raise ValueError(f"新输入名已被占用: {new_input_name}")

    original_name = original_input.name
    original_input.name = new_input_name
    dims[2].ClearField("dim_param")
    dims[2].dim_value = content_h
    dims[3].ClearField("dim_param")
    dims[3].dim_value = content_w

    pads_name = unique_name(graph, "__aipp_graph_pad_pads")
    value_name = unique_name(graph, "__aipp_graph_pad_value")
    node_name = unique_name(graph, "AippGraphPad")
    pads = [0, 0, top, left, 0, 0, bottom, right]
    graph.initializer.extend(
        [
            helper.make_tensor(pads_name, TensorProto.INT64, [8], pads),
            helper.make_tensor(value_name, tensor_type.elem_type, [1], [args.value]),
        ]
    )
    pad_node = helper.make_node(
        "Pad",
        [new_input_name, pads_name, value_name],
        [original_name],
        mode="constant",
        name=node_name,
    )
    graph.node.insert(0, pad_node)

    checker.check_model(model)
    onnx.save(model, args.output_model)
    print(f"已生成: {args.output_model}")
    print(f"ATC input_shape: {new_input_name}:1,3,{content_h},{content_w}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # 给转换服务器输出简洁、可操作的错误
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
